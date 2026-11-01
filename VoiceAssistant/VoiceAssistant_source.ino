/*
 * ESP32-S3 Voice Assistant v1.0
 * Target: ESP32-S3-N16R8 (16 MB Flash + 8 MB OPI PSRAM)
 * Framework: Arduino-ESP32 3.3.5
 *
 * Audio hardware defaults (verify these pins against the exact carrier board):
 * INMP441: BCLK=GPIO5, WS=GPIO4, SD=GPIO6, L/R=GND (left channel)
 * MAX98357A: BCLK=GPIO15, LRC=GPIO16, DIN=GPIO7, VIN=5V, GND=GND
 * Speaker: connect only between MAX98357A SPK+ and SPK- (BTL output).
 *
 * Arduino-ESP32 3.3.5 auto-allocates I2S controllers. The microphone is
 * initialized first and stays active; the speaker starts later while mic RX
 * remains allocated, so ESP32-S3 can assign playback to the other controller.
 * INMP441 24-bit samples are transformed from 32-bit I2S slots to 16-bit PCM.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <NetworkClient.h>
#include <NetworkClientSecure.h>
#include <ESP_I2S.h>
#include <esp_srmodel.h>
#include <esp_mn_iface.h>
#include <esp_mn_models.h>
#include <esp_mn_speech_commands.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <ArduinoJson.h>
#include <esp_afe_config.h>
#include <esp_afe_sr_iface.h>
#include <esp_afe_sr_models.h>  // Declares esp_afe_handle_from_config() in ESP-SR
#include <esp_system.h>
#include <esp_partition.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <mbedtls/base64.h>
#include <memory>
#include <vector>

// --------------------------- Hardware configuration ------------------------
static constexpr int MIC_BCLK_PIN = 5;
static constexpr int MIC_WS_PIN   = 4;
static constexpr int MIC_DIN_PIN  = 6;
static constexpr int SPK_BCLK_PIN = 15;
static constexpr int SPK_WS_PIN   = 16;
static constexpr int SPK_DOUT_PIN = 7;

static constexpr uint32_t AUDIO_SAMPLE_RATE = 16000;
static constexpr uint32_t TTS_FALLBACK_RATE = 24000;
static constexpr uint32_t SERIAL_BAUD = 115200;
static constexpr size_t MAX_RECORD_SECONDS = 8;
static constexpr size_t MAX_AUDIO_BYTES = AUDIO_SAMPLE_RATE * MAX_RECORD_SECONDS * sizeof(int16_t);
static constexpr char AP_PASSWORD[] = "voice1234";
static constexpr char FW_VERSION[] = "1.1.1-audio-diag";

// --------------------------- Runtime objects --------------------------------
WebServer server(80);
Preferences prefs;
I2SClass micI2S;
I2SClass speakerI2S;

struct Config {
  String wifiSsid;
  String wifiPassword;

  String llmProvider = "deepseek";
  String llmEndpoint = "https://api.deepseek.com/chat/completions";
  String llmApiKey;
  String llmModel = "deepseek-chat";
  String systemPrompt = "你是一个简洁、准确的中文语音助手。回答适合语音播报，不要使用 Markdown 表格。";

  String asrProvider = "qwen-audio-asr-http";
  String asrEndpoint = "https://YOUR_WORKSPACE_ID.cn-beijing.maas.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation";
  String asrApiKey;
  String asrModel = "qwen-audio-3.1-asr-flash";

  String ttsProvider = "dashscope-qwen";
  String ttsEndpoint = "https://YOUR_WORKSPACE_ID.cn-beijing.maas.aliyuncs.com/api/v1/services/audio/tts/SpeechSynthesizer";
  String ttsApiKey;
  String ttsModel = "qwen-audio-3.1-tts-flash";
  String ttsVoice = "longanhuan_v3.1";
  float ttsSpeed = 1.0f;

  String wakeWordZh = "你好大肥鱼";
  String wakeWordPinyin = "ni hao da fei yu";
  uint8_t wakeThreshold = 20;       // XiaoZhi-style percentage; lower is more sensitive
  uint16_t wakeDurationMs = 3000;   // MultiNet detection window
  bool noiseSuppression = true;
  bool keepApEnabled = true;
  uint8_t speakerVolume = 70; // software playback volume, 0-100
};

Config cfg;
volatile bool wakePending = false;
volatile bool srStarted = false;
volatile bool assistantBusy = false;

// MultiNet runtime. The microphone I2S peripheral is shared with the cloud-ASR
// recording path, so both readers are serialized by micMutex.
static srmodel_list_t *srModels = nullptr;
static esp_mn_iface_t *multiNet = nullptr;
static model_iface_data_t *multiNetData = nullptr;
static char *multiNetName = nullptr;  // owned by srModels
static TaskHandle_t multiNetTaskHandle = nullptr;
static SemaphoreHandle_t micMutex = nullptr;
static volatile bool multiNetTaskRunning = false;
static size_t multiNetFrameSamples = 0;
static int16_t *multiNetFrame = nullptr;
static size_t multiNetFrameFill = 0;
static const esp_afe_sr_iface_t *wakeAfe = nullptr;
static esp_afe_sr_data_t *wakeAfeData = nullptr;
static int16_t *wakeAfeFeedFrame = nullptr;
static size_t wakeAfeFeedSamples = 0;
static size_t wakeAfeFeedChannels = 0;
static size_t wakeAfeFetchSamples = 0;
static String recognizerStatus = "未初始化";
bool wifiConnecting = false;
bool apActive = false;
unsigned long wifiAttemptAt = 0;
unsigned long lastWifiRetryAt = 0;
String stageText = "启动中";
String lastTranscript;
String lastReply;
String lastError;
String lastNsStatus = "未初始化";
bool micInitialized = false;

struct ChatTurn {
  String role;
  String content;
};
std::vector<ChatTurn> conversation;

// Streaming srmodels.bin uploader. A failed upload leaves the model partition
// blank/invalid, but the startup guard keeps the device in AP/Web mode.
static const esp_partition_t *modelUploadPartition = nullptr;
static bool modelUploadOk = false;
static size_t modelUploadOffset = 0;
static size_t modelUploadBuffered = 0;
static bool modelUploadHadError = false;
alignas(4) static uint8_t modelUploadBuffer[4096];

// --------------------------- Utility functions -----------------------------
String deviceId() {
  uint64_t mac = ESP.getEfuseMac();
  char id[9];
  snprintf(id, sizeof(id), "%06llX", (unsigned long long)(mac & 0xFFFFFF));
  return String(id);
}

String jsonString(const JsonVariantConst &v, const String &fallback = "") {
  if (v.is<const char*>()) return String(v.as<const char*>());
  if (v.is<String>()) return v.as<String>();
  return fallback;
}

void loadConfig() {
  prefs.begin("voicecfg", true);
  cfg.wifiSsid = prefs.getString("wssid", "");
  cfg.wifiPassword = prefs.getString("wpass", "");
  cfg.llmProvider = prefs.getString("lp", cfg.llmProvider);
  cfg.llmEndpoint = prefs.getString("le", cfg.llmEndpoint);
  cfg.llmApiKey = prefs.getString("lk", "");
  cfg.llmModel = prefs.getString("lm", cfg.llmModel);
  cfg.systemPrompt = prefs.getString("sp", cfg.systemPrompt);
  cfg.asrProvider = prefs.getString("ap", cfg.asrProvider);
  cfg.asrEndpoint = prefs.getString("ae", cfg.asrEndpoint);
  cfg.asrApiKey = prefs.getString("ak", "");
  cfg.asrModel = prefs.getString("am", cfg.asrModel);
  cfg.ttsProvider = prefs.getString("tp", cfg.ttsProvider);
  cfg.ttsEndpoint = prefs.getString("te", cfg.ttsEndpoint);
  cfg.ttsApiKey = prefs.getString("tk", "");
  cfg.ttsModel = prefs.getString("tm", cfg.ttsModel);
  cfg.ttsVoice = prefs.getString("tv", cfg.ttsVoice);
  cfg.ttsSpeed = prefs.getFloat("ts", 1.0f);
  cfg.wakeWordZh = prefs.getString("wz", cfg.wakeWordZh);
  cfg.wakeWordPinyin = prefs.getString("wp", cfg.wakeWordPinyin);
  cfg.wakeThreshold = prefs.getUChar("wth", cfg.wakeThreshold);
  cfg.wakeDurationMs = prefs.getUShort("wdur", cfg.wakeDurationMs);
  // Migrate the original placeholder to the user's selected wake phrase.
  if (cfg.wakeWordZh == "你好小智" || cfg.wakeWordZh.isEmpty()) {
    cfg.wakeWordZh = "你好大肥鱼";
  }
  if (cfg.wakeWordPinyin == "ni hao xiao zhi" || cfg.wakeWordPinyin.isEmpty() ||
      cfg.wakeWordPinyin == "ni3 hao3 da4 fei2 yu2") {
    cfg.wakeWordPinyin = "ni hao da fei yu";
  }
  cfg.noiseSuppression = prefs.getBool("ns", true);
  cfg.keepApEnabled = prefs.getBool("kap", true);
  cfg.speakerVolume = prefs.getUChar("vol", 70);
  if (cfg.speakerVolume > 100) cfg.speakerVolume = 70;
  prefs.end();
}

void saveConfig() {
  prefs.begin("voicecfg", false);
  prefs.putString("wssid", cfg.wifiSsid);
  prefs.putString("wpass", cfg.wifiPassword);
  prefs.putString("lp", cfg.llmProvider);
  prefs.putString("le", cfg.llmEndpoint);
  prefs.putString("lk", cfg.llmApiKey);
  prefs.putString("lm", cfg.llmModel);
  prefs.putString("sp", cfg.systemPrompt);
  prefs.putString("ap", cfg.asrProvider);
  prefs.putString("ae", cfg.asrEndpoint);
  prefs.putString("ak", cfg.asrApiKey);
  prefs.putString("am", cfg.asrModel);
  prefs.putString("tp", cfg.ttsProvider);
  prefs.putString("te", cfg.ttsEndpoint);
  prefs.putString("tk", cfg.ttsApiKey);
  prefs.putString("tm", cfg.ttsModel);
  prefs.putString("tv", cfg.ttsVoice);
  prefs.putFloat("ts", cfg.ttsSpeed);
  prefs.putString("wz", cfg.wakeWordZh);
  prefs.putString("wp", cfg.wakeWordPinyin);
  prefs.putUChar("wth", cfg.wakeThreshold);
  prefs.putUShort("wdur", cfg.wakeDurationMs);
  prefs.putBool("ns", cfg.noiseSuppression);
  prefs.putBool("kap", cfg.keepApEnabled);
  prefs.putUChar("vol", cfg.speakerVolume);
  prefs.end();
}

String ipAddress() {
  if (WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString();
  return WiFi.softAPIP().toString();
}

void setStage(const String &s) {
  stageText = s;
  Serial.printf("[STATE] %s\n", s.c_str());
}

void sendJson(int code, JsonDocument &doc) {
  String out;
  serializeJson(doc, out);
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json; charset=utf-8", out);
}

bool startsWithHttps(const String &input) {
  String url = input;
  url.trim();
  return url.length() >= 8 && url.substring(0, 8).equalsIgnoreCase("https://");
}

String normalizedHttpsUrl(const String &input) {
  String url = input;
  url.trim();
  if (url.length() >= 8 && url.substring(0, 8).equalsIgnoreCase("https://")) {
    url = String("https://") + url.substring(8);
  }
  return url;
}

std::unique_ptr<NetworkClientSecure> makeTlsClient() {
  auto c = std::unique_ptr<NetworkClientSecure>(new NetworkClientSecure());
  // Prototype mode: certificate verification is disabled for broad provider compatibility.
  // Do not expose this device or its API keys to untrusted networks.
  c->setInsecure();
  return c;
}

bool httpBegin(HTTPClient &http, NetworkClientSecure &client, const String &url, const char *endpointName) {
  if (!startsWithHttps(url)) {
    lastError = String(endpointName) + " API 地址必须使用 HTTPS；请检查保存后的完整地址是否以 https:// 开头";
    return false;
  }
  http.setConnectTimeout(15000);
  http.setTimeout(45000);
  http.setReuse(false);
  // Arduino-ESP32 3.3.5 HTTPClient expects its NetworkClient base type.
  // Pass the base explicitly to avoid overload-resolution failures in some
  // ArduinoDroid/Clang builds. NetworkClientSecure derives from NetworkClient.
  NetworkClient &baseClient = static_cast<NetworkClient &>(client);
  String normalizedUrl = normalizedHttpsUrl(url);
  return http.begin(baseClient, normalizedUrl);
}

void addBearer(HTTPClient &http, const String &key) {
  http.addHeader("Authorization", "Bearer " + key);
}

uint32_t readLE32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint16_t readLE16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
void writeLE16(uint8_t *p, uint16_t v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
void writeLE32(uint8_t *p, uint32_t v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff; }

// --------------------------- Web administration ----------------------------
const char INDEX_HTML[] PROGMEM =
  "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>语音助手控制台</title>\n"
  "<style>\n"
  ":root{color-scheme:dark;--bg:#0a1020;--panel:#151f33;--line:#2b3a56;--txt:#e9efff;--muted:#a6b4ce;--blue:#78aaff;--green:#41d6a2;--red:#ff8a8a}*{box-sizing:border-box}body{margin:0;background:linear-gradient(145deg,#101d36,#080d18 70%);font:15px system-ui,\"Microsoft YaHei\",sans-serif;color:var(--txt)}main{max-width:920px;margin:auto;padding:18px 13px 60px}.top{display:flex;justify-content:space-between;align-items:center;gap:10px}.brand{font-size:23px;font-weight:800}.muted,.hint{color:var(--muted);font-size:12px;line-height:1.55}.pill{padding:7px 11px;border:1px solid var(--line);border-radius:18px;color:var(--green);font-size:12px}.card{background:#151f33ee;border:1px solid var(--line);border-radius:15px;padding:16px;margin:13px 0}.card h2{font-size:16px;margin:0 0 13px}.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}.field{display:flex;flex-direction:column;gap:6px;margin-bo"
  "ttom:10px}.full{grid-column:1/-1}label{color:var(--muted);font-size:13px}input,select,textarea{background:#0b1426;color:var(--txt);border:1px solid #344866;border-radius:9px;padding:11px;font:inherit;width:100%;min-width:0}textarea{min-height:78px;resize:vertical}input[type=range]{padding:0;accent-color:var(--blue)}button{border:0;background:var(--blue);color:#07152e;font-weight:750;border-radius:9px;padding:11px 15px;font-size:14px}button.alt{background:#2a3a58;color:var(--txt)}.row{display:flex;gap:9px;align-items:center;flex-wrap:wrap}.row>*{flex:1}.row button{flex:0 0 auto}.tiles{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}.tile{background:#0d172a;border:1px solid var(--line);border-radius:10px;padding:11px;overflow-wrap:anywhere}.tile small{display:block;color:var(--muted)}.tile b{display:block;margin-top:5px}.status{white-space:pre-wrap;overflow-wrap:anywhere;color:var(--muted);font-size:13px}.warn{color:#ffd28b}.ok{colo"
  "r:var(--green)}.actions{display:flex;gap:9px;flex-wrap:wrap}.toast{position:fixed;bottom:16px;left:50%;transform:translateX(-50%);background:#263b5d;padding:11px 16px;border-radius:10px;display:none;max-width:92%;z-index:9}@media(max-width:650px){.grid{grid-template-columns:1fr}.full{grid-column:auto}.tiles{grid-template-columns:1fr 1fr}.top{align-items:flex-start}}\n"
  "</style></head><body><main>\n"
  "<div class=\"top\"><div><div class=\"brand\">ESP32-S3 语音助手</div><div class=\"muted\">本地设备管理 · 固件 <span id=\"fwVersion\">v1.1.1-audio-diag</span> · 音频诊断版</div></div><div class=\"pill\" id=\"online\">读取状态中</div></div>\n"
  "<section class=\"card\"><h2>设备状态</h2><div class=\"tiles\"><div class=\"tile\"><small>网络</small><b id=\"wifi\">--</b></div><div class=\"tile\"><small>访问地址</small><b id=\"ip\">--</b></div><div class=\"tile\"><small>可用 RAM</small><b id=\"heap\">--</b></div><div class=\"tile\"><small>可用 PSRAM</small><b id=\"psram\">--</b></div><div class=\"tile\"><small>语音状态</small><b id=\"stage\">--</b></div><div class=\"tile\"><small>降噪状态</small><b id=\"ns\">--</b></div><div class=\"tile\"><small>麦克风 I2S</small><b id=\"micStatus\">--</b></div></div><p class=\"status\" id=\"last\"></p></section>\n"
  "<form id=\"settings\">\n"
  "<section class=\"card\"><h2>1. Wi-Fi</h2><div class=\"field full\"><label>扫描到的网络</label><div class=\"row\"><select id=\"networks\"><option value=\"\">点击右侧扫描</option></select><button type=\"button\" class=\"alt\" onclick=\"scan()\">扫描</button></div></div><div class=\"grid\"><div class=\"field\"><label>SSID</label><input name=\"wifiSsid\" id=\"wifiSsid\" autocomplete=\"off\"></div><div class=\"field\"><label>密码（留空不修改）</label><input name=\"wifiPassword\" id=\"wifiPassword\" type=\"password\" autocomplete=\"new-password\"></div></div><label><input style=\"width:auto\" type=\"checkbox\" name=\"keepApEnabled\" id=\"keepApEnabled\"> 保留配置热点，方便故障恢复</label><p class=\"hint\">首次启动连接热点 Voice-Assistant-设备编号，密码 voice1234，访问 http://192.168.4.1 。</p></section>\n"
  "<section class=\"card\"><h2>2. 大模型（LLM）</h2><div class=\"grid\"><div class=\"field\"><label>服务商</label><select name=\"llmProvider\" id=\"llmProvider\"><option value=\"deepseek\">DeepSeek</option><option value=\"qwen-compatible\">阿里云百炼（OpenAI 兼容）</option><option value=\"openai\">OpenAI</option><option value=\"custom\">自定义兼容接口</option></select></div><div class=\"field\"><label>模型名称</label><input name=\"llmModel\" id=\"llmModel\" placeholder=\"deepseek-chat\"></div><div class=\"field full\"><label>完整 Chat Completions URL</label><input name=\"llmEndpoint\" id=\"llmEndpoint\" placeholder=\"https://api.deepseek.com/chat/completions\"></div><div class=\"field full\"><label>API Key（留空不修改）</label><input name=\"llmApiKey\" id=\"llmApiKey\" type=\"password\" placeholder=\"已保存的 Key 不会回传\"></div><div class=\"field full\"><label>系统提示词</label><textarea name=\"systemPrompt\" id=\"systemPrompt\"></textarea></div></div></section>\n"
  "<section class=\"card\"><h2>3. 语音识别（ASR）</h2><div class=\"grid\"><div class=\"field\"><label>服务商/协议</label><select name=\"asrProvider\" id=\"asrProvider\"><option value=\"qwen-audio-asr-http\">阿里云百炼 Qwen Audio ASR 3.1（原生 HTTP）</option><option value=\"openai-compatible\">OpenAI 兼容音频转写</option><option value=\"custom\">自定义 multipart 转写接口</option></select></div><div class=\"field\"><label>模型</label><input name=\"asrModel\" id=\"asrModel\" placeholder=\"qwen-audio-3.1-asr-flash / gpt-4o-mini-transcribe\"></div><div class=\"field full\"><label>完整转写 URL（按所选协议填写）</label><input name=\"asrEndpoint\" id=\"asrEndpoint\"></div><div class=\"field full\"><label>API Key（留空不修改）</label><input name=\"asrApiKey\" id=\"asrApiKey\" type=\"password\" placeholder=\"已保存的 Key 不会回传\"></div></div><p class=\"hint\">Qwen Audio ASR 使用百炼原生 HTTP 接口，固件把 WAV 转成 Base64 Data URI 发送；模型建议填 qwen-audio-3.1-asr-flash。也支持 OpenAI multipart 兼容接口。请先将 YOUR_WORKSPACE_ID 替换为百炼"
  "控制台中的 Workspace ID，并使用该地域对应的 API Key。</p></section>\n"
  "<section class=\"card\"><h2>4. 语音合成（TTS）</h2><div class=\"grid\"><div class=\"field\"><label>服务商/协议</label><select name=\"ttsProvider\" id=\"ttsProvider\"><option value=\"dashscope-qwen\">阿里云百炼 Qwen Audio TTS HTTP</option><option value=\"openai-compatible\">OpenAI 兼容 audio/speech</option></select></div><div class=\"field\"><label>模型</label><input name=\"ttsModel\" id=\"ttsModel\"></div><div class=\"field full\"><label>完整 TTS URL</label><input name=\"ttsEndpoint\" id=\"ttsEndpoint\"></div><div class=\"field\"><label>API Key（留空不修改）</label><input name=\"ttsApiKey\" id=\"ttsApiKey\" type=\"password\" placeholder=\"已保存的 Key 不会回传\"></div><div class=\"field\"><label>音色</label><input name=\"ttsVoice\" id=\"ttsVoice\"></div><div class=\"field\"><label>语速（0.5–2.0）</label><input name=\"ttsSpeed\" id=\"ttsSpeed\" type=\"number\" min=\"0.5\" max=\"2\" step=\"0.1\"></div></div><p class=\"hint\">DashScope 模式请求 WAV/24 kHz，并根据返回的音频 URL 下载后播放。Endpoint 请填写百炼控制台对应"
  "地域的 SpeechSynthesizer 地址。</p></section>\n"
  "<section class=\"card\"><h2>5. 自定义唤醒词（ESP-SR MultiNet 6）</h2><div class=\"grid\"><div class=\"field\"><label>唤醒词中文</label><input name=\"wakeWordZh\" id=\"wakeWordZh\" maxlength=\"48\"></div><div class=\"field\"><label>拼音（不带声调数字，音节用空格分开）</label><input name=\"wakeWordPinyin\" id=\"wakeWordPinyin\" placeholder=\"ni hao da fei yu\" maxlength=\"96\"></div><div class=\"field\"><label>识别灵敏度（1–99，越小越灵敏）</label><input name=\"wakeThreshold\" id=\"wakeThreshold\" type=\"number\" min=\"1\" max=\"99\"></div><div class=\"field\"><label>检测窗口（毫秒）</label><input name=\"wakeDurationMs\" id=\"wakeDurationMs\" type=\"number\" min=\"1000\" max=\"5000\" step=\"500\"></div></div><div class=\"field full\"><label>ESP-SR 模型文件（包含 mn6_cn + fst 的 srmodels.bin）</label><input id=\"modelFile\" type=\"file\" accept=\".bin,application/octet-stream\"><div class=\"row\"><button type=\"button\" class=\"alt\" onclick=\"uploadModel()\">上传 MultiNet 模型到 model 分区</button><span class=\"status\" id=\"modelStatus\">模型状态读取中</span></div></div><label><input style=\"width:auto\" type=\"checkbox\" id=\"noiseSuppression\" name=\"noiseSuppression\"> 启用云端 ASR 录音的额外 AFE WebRTC NS（唤醒检测路径始终使用 AFE + NS）</label><p class=\"hint warn\">默认唤醒词：你好大肥鱼；MultiNet 6 中文拼音：ni hao da fei yu（不带声调数字）。这不是重新训练神经网络：固件把拼音命令动态加入预训练的 mn6_cn。先用下方 GitHub Actions 生成包含 mn6_cn 和 fst 的通用 srmodels.bin，再上传。修改唤醒词/阈值后需要重启生效。上传会擦除整个 model 分区，请勿断电。</p></section>\n"
  "<div class=\"actions\"><button type=\"submit\">保存全部配置</button><button class=\"alt\" type=\"button\" onclick=\"loadConfig()\">重新读取</button><button class=\"alt\" type=\"button\" onclick=\"reboot()\">重启设备</button></div>\n"
  "</form>\n"
  "<section class=\"card\"><h2>扬声器音量</h2><div class=\"field\"><label for=\"speakerVolume\">播放音量：<b id=\"speakerVolumeLabel\">70%</b></label><input type=\"range\" id=\"speakerVolume\" min=\"0\" max=\"100\" step=\"1\" value=\"70\"></div><p class=\"hint\">0 为静音，100 为最大软件音量。松开滑块后自动保存。</p></section>\n"
  "<section class=\"card\"><h2>音频硬件检测</h2><div class=\"row\"><button type=\"button\" class=\"alt\" onclick=\"testMic()\">检测麦克风</button><button type=\"button\" class=\"alt\" onclick=\"testSpeaker()\">扬声器测试音</button></div><p class=\"hint\">麦克风检测会实际读取约 0.7 秒 I2S 音频数据，测试时请对着麦克风说话或轻敲外壳。扬声器测试会向 MAX98357A 输出短音；模块没有喇叭回读线路，需以实际听到声音为准。</p><p class=\"status\" id=\"audioTestResult\">尚未检测</p></section>\n"
  "<section class=\"card\"><h2>文本链路测试</h2><div class=\"field\"><label>输入一段文字，测试 LLM + TTS（无需唤醒词）</label><textarea id=\"testText\" placeholder=\"你好，请介绍一下你自己\"></textarea></div><button onclick=\"testChat()\">发送并语音播报</button><p class=\"status\" id=\"testResult\"></p></section>\n"
  "<p class=\"hint\">安全提示：此版本为个人局域网原型，HTTPS 证书校验暂时关闭；不要将管理端口暴露到公网。首次烧录后建议尽快设置独立管理密码（当前版本尚未加入网页登录）。</p>\n"
  "</main><div class=\"toast\" id=\"toast\"></div>\n"
  "<script>\n"
  "const $=id=>document.getElementById(id);function toast(t){$('toast').textContent=t;$('toast').style.display='block';setTimeout(()=>$('toast').style.display='none',2800)}\n"
  "async function api(path,opt={}){const r=await fetch(path,opt);let j={};try{j=await r.json()}catch(e){}if(!r.ok)throw Error(j.error||('HTTP '+r.status));return j}\n"
  "async function loadConfig(){try{let c=await api('/api/config');for(const [k,v] of Object.entries(c)){if($(k)){if($(k).type==='checkbox')$(k).checked=!!v;else $(k).value=v??''}}$('speakerVolumeLabel').textContent=$('speakerVolume').value+'%';$('online').textContent='设备在线'}catch(e){toast('读取失败：'+e.message)}}\n"
  "async function refresh(){try{let s=await api('/api/status');$('wifi').textContent=s.wifi;$('ip').textContent=s.ip;$('heap').textContent=s.heap+' KB';$('psram').textContent=s.psram+' KB';$('stage').textContent=s.stage;$('ns').textContent=s.ns;$('micStatus').textContent=s.micInitialized?'I2S 已初始化':'未初始化';if(s.firmwareVersion)$('fwVersion').textContent=s.firmwareVersion;$('modelStatus').textContent=s.modelStatus||s.recognizer||'未知';$('last').textContent='识别：'+(s.transcript||'--')+'\\n回答：'+(s.reply||'--')+'\\n错误：'+(s.error||'无');$('online').textContent='在线'}catch(e){$('online').textContent='离线'}}\n"
  "async function scan(){try{toast('扫描中…');let s=await api('/api/scan');$('networks').innerHTML='<option value=\"\">选择 Wi-Fi</option>';s.networks.forEach(n=>{let o=document.createElement('option');o.value=n.ssid;o.textContent=n.ssid+' ('+n.rssi+' dBm)'+(n.secure?' 🔒':'');$('networks').appendChild(o)});toast('扫描完成：'+s.networks.length+' 个网络')}catch(e){toast('扫描失败：'+e.message)}}\n"
  "$('networks').addEventListener('change',e=>{if(e.target.value)$('wifiSsid').value=e.target.value});\n"
  "$('settings').addEventListener('submit',async e=>{e.preventDefault();let d={};new FormData(e.target).forEach((v,k)=>d[k]=v);['noiseSuppression','keepApEnabled'].forEach(k=>d[k]=$(k).checked);d.ttsSpeed=Number(d.ttsSpeed||1);d.wakeThreshold=Number(d.wakeThreshold||20);d.wakeDurationMs=Number(d.wakeDurationMs||3000);try{let r=await api('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(d)});toast(r.message||'已保存');setTimeout(refresh,1500)}catch(e){toast('保存失败：'+e.message)}});\n"
  "async function reboot(){if(!confirm('确认重启设备？'))return;await api('/api/reboot',{method:'POST'});toast('设备正在重启')}\n"
  "async function uploadModel(){const f=$('modelFile').files[0];if(!f)return toast('先选择 srmodels.bin');if(!confirm('上传会先擦除整个 model 分区。请确认文件是为当前 ESP-SR/Arduino Core 版本生成的，且供电稳定。继续？'))return;const fd=new FormData();fd.append('model',f,f.name);$('modelStatus').textContent='正在上传：'+f.name;try{const r=await fetch('/api/model',{method:'POST',body:fd});let j={};try{j=await r.json()}catch(e){}if(!r.ok)throw Error(j.error||('HTTP '+r.status));$('modelStatus').textContent=j.message||'上传完成，请重启';toast('模型文件已写入，请重启设备');}catch(e){$('modelStatus').textContent='上传失败：'+e.message;toast('模型上传失败')}}\n" 
  "async function testChat(){let t=$('testText').value.trim();if(!t)return toast('请输入测试文本');$('testResult').textContent='请求中…';try{let r=await api('/api/chat',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({text:t})});$('testResult').textContent=r.reply||'完成';}catch(e){$('testResult').textContent='失败：'+e.message}}\n"
  "function testMic(){ $('audioTestResult').textContent='正在读取麦克风数据，请对着麦克风说话…';api('/api/audio/mic',{method:'POST'}).then(r=>{$('audioTestResult').textContent=r.message+'；样本数 '+r.samples+'，RMS '+r.rms+'，峰值 '+r.peak;}).catch(e=>$('audioTestResult').textContent='麦克风检测失败：'+e.message)}\n"
  "function testSpeaker(){ $('audioTestResult').textContent='正在播放测试音…';api('/api/audio/speaker',{method:'POST'}).then(r=>$('audioTestResult').textContent=r.message).catch(e=>$('audioTestResult').textContent='扬声器测试失败：'+e.message)}\n"
  "$('speakerVolume').addEventListener('input',()=>{$('speakerVolumeLabel').textContent=$('speakerVolume').value+'%'});$('speakerVolume').addEventListener('change',()=>api('/api/audio/volume',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({volume:Number($('speakerVolume').value)})}).then(()=>toast('音量已保存')).catch(e=>toast('音量保存失败：'+e.message)));\n"
  "loadConfig();refresh();setInterval(refresh,4000);\n"
  "</script></body></html>";

void handleRoot() {
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

const esp_partition_t *findModelPartition() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "model");
}

bool modelPartitionBlank(const esp_partition_t *part) {
  if (!part) return true;
  uint8_t probe[64];
  if (esp_partition_read(part, 0, probe, sizeof(probe)) != ESP_OK) return true;
  for (size_t i = 0; i < sizeof(probe); ++i) if (probe[i] != 0xFF) return false;
  return true;
}

bool flushModelUploadBuffer(bool padToWord) {
  if (!modelUploadPartition || modelUploadBuffered == 0) return true;
  size_t writeSize = modelUploadBuffered;
  if (padToWord && (writeSize & 3U)) {
    size_t padded = (writeSize + 3U) & ~((size_t)3U);
    memset(modelUploadBuffer + writeSize, 0xFF, padded - writeSize);
    writeSize = padded;
  }
  if (modelUploadOffset + writeSize > modelUploadPartition->size) return false;
  esp_err_t e = esp_partition_write(modelUploadPartition, modelUploadOffset, modelUploadBuffer, writeSize);
  if (e != ESP_OK) {
    Serial.printf("[MODEL] flash write failed at 0x%zx: %s\n", modelUploadOffset, esp_err_to_name(e));
    return false;
  }
  modelUploadOffset += writeSize;
  modelUploadBuffered = 0;
  return true;
}

void handleModelUploadStream() {
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    modelUploadPartition = findModelPartition();
    modelUploadOk = modelUploadPartition != nullptr;
    modelUploadHadError = !modelUploadOk;
    modelUploadOffset = 0;
    modelUploadBuffered = 0;
    String filename = upload.filename;
    filename.toLowerCase();
    if (!filename.endsWith(".bin")) modelUploadHadError = true;
    if (!modelUploadPartition) {
      lastError = "model 分区不存在，请先使用 ESP-SR 分区表重新烧录";
    } else if (!modelUploadHadError && assistantBusy) {
      modelUploadHadError = true;
      lastError = "语音助手正在处理任务，请稍后再上传模型";
    } else if (!modelUploadHadError) {
      // Stop model inference before erasing the partition it reads from.
      if (srStarted || multiNetTaskHandle != nullptr) {
        stopMultiNet();
      }
      if (multiNetTaskHandle != nullptr) {
        modelUploadHadError = true;
        modelUploadOk = false;
        lastError = "MultiNet 检测任务未能安全停止，已取消模型擦除";
        return;
      }
      setStage("准备写入 ESP-SR 模型分区");
      esp_err_t e = esp_partition_erase_range(modelUploadPartition, 0, modelUploadPartition->size);
      if (e != ESP_OK) {
        modelUploadHadError = true;
        lastError = String("擦除 model 分区失败：") + esp_err_to_name(e);
      }
    } else {
      lastError = "只接受 .bin 模型文件";
    }
    Serial.printf("[MODEL] upload start: %s, partition=%s, size=%u\n", upload.filename.c_str(), modelUploadPartition ? "found" : "missing", modelUploadPartition ? (unsigned)modelUploadPartition->size : 0);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (modelUploadHadError || !modelUploadPartition) return;
    const uint8_t *src = upload.buf;
    size_t remain = upload.currentSize;
    while (remain > 0) {
      size_t room = sizeof(modelUploadBuffer) - modelUploadBuffered;
      size_t n = remain < room ? remain : room;
      memcpy(modelUploadBuffer + modelUploadBuffered, src, n);
      modelUploadBuffered += n;
      src += n;
      remain -= n;
      if (modelUploadBuffered == sizeof(modelUploadBuffer) && !flushModelUploadBuffer(false)) {
        modelUploadHadError = true;
        lastError = "写入 model 分区失败";
        return;
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (modelUploadHadError || !modelUploadPartition) return;
    if (!flushModelUploadBuffer(true)) {
      modelUploadHadError = true;
      lastError = "写入模型文件末尾失败";
      return;
    }
    if (modelUploadOffset < 1024) {
      modelUploadHadError = true;
      lastError = "模型文件过小，不像有效的 srmodels.bin";
    } else {
      modelUploadOk = true;
      lastError = "模型包已写入；具体模型名称和 ESP-SR 版本兼容性仍需验证";
      setStage("模型已写入，请重启设备");
      Serial.printf("[MODEL] upload complete: %u bytes\n", (unsigned)modelUploadOffset);
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    modelUploadHadError = true;
    modelUploadOk = false;
    lastError = "模型上传中断；请重新上传完整 srmodels.bin";
  }
}

void handleModelUploadDone() {
  JsonDocument r;
  if (modelUploadHadError || !modelUploadOk) {
    r["error"] = lastError.isEmpty() ? "模型上传失败" : lastError;
    sendJson(500, r);
    return;
  }
  r["ok"] = true;
  r["bytes"] = modelUploadOffset;
  r["message"] = "通用 MultiNet 模型包已写入。重启后固件会把当前拼音命令动态加入模型；该文件本身不含专属唤醒词权重。";
  sendJson(200, r);
}

void handleStatus() {
  JsonDocument d;
  d["wifi"] = WiFi.status() == WL_CONNECTED ? "已连接" : "配置热点";
  d["ip"] = ipAddress();
  d["heap"] = ESP.getFreeHeap() / 1024;
  d["psram"] = ESP.getFreePsram() / 1024;
  d["stage"] = stageText;
  d["recognizer"] = recognizerStatus;
  d["ns"] = lastNsStatus;
  d["transcript"] = lastTranscript;
  d["reply"] = lastReply;
  d["error"] = lastError;
  d["firmwareVersion"] = FW_VERSION;
  d["micInitialized"] = micInitialized;
  d["speakerVolume"] = cfg.speakerVolume;
  const esp_partition_t *mp = findModelPartition();
  if (!mp) d["modelStatus"] = "model 分区不存在";
  else if (modelPartitionBlank(mp)) d["modelStatus"] = "model 分区为空，尚未上传 srmodels.bin";
  else d["modelStatus"] = recognizerStatus;
  sendJson(200, d);
}

void handleScan() {
  int n = WiFi.scanNetworks(false, true, false, 250);
  JsonDocument d;
  JsonArray arr = d["networks"].to<JsonArray>();
  for (int i = 0; i < n; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
    o["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  sendJson(200, d);
}

void handleGetConfig() {
  JsonDocument d;
  d["wifiSsid"] = cfg.wifiSsid;
  d["llmProvider"] = cfg.llmProvider; d["llmEndpoint"] = cfg.llmEndpoint;
  d["llmModel"] = cfg.llmModel; d["systemPrompt"] = cfg.systemPrompt;
  d["llmKeyConfigured"] = !cfg.llmApiKey.isEmpty();
  d["asrProvider"] = cfg.asrProvider; d["asrEndpoint"] = cfg.asrEndpoint;
  d["asrModel"] = cfg.asrModel; d["asrKeyConfigured"] = !cfg.asrApiKey.isEmpty();
  d["ttsProvider"] = cfg.ttsProvider; d["ttsEndpoint"] = cfg.ttsEndpoint;
  d["ttsModel"] = cfg.ttsModel; d["ttsVoice"] = cfg.ttsVoice;
  d["ttsSpeed"] = cfg.ttsSpeed; d["ttsKeyConfigured"] = !cfg.ttsApiKey.isEmpty();
  d["wakeWordZh"] = cfg.wakeWordZh; d["wakeWordPinyin"] = cfg.wakeWordPinyin;
  d["wakeThreshold"] = cfg.wakeThreshold; d["wakeDurationMs"] = cfg.wakeDurationMs;
  d["noiseSuppression"] = cfg.noiseSuppression; d["keepApEnabled"] = cfg.keepApEnabled;
  d["speakerVolume"] = cfg.speakerVolume;
  sendJson(200, d);
}

bool normalizeMultiNetPinyin(String &pinyin) {
  pinyin.trim();
  pinyin.toLowerCase();
  String normalized;
  bool previousSpace = true;
  for (size_t i = 0; i < pinyin.length(); ++i) {
    char c = pinyin[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      if (!previousSpace) { normalized += ' '; previousSpace = true; }
      continue;
    }
    if (c < 'a' || c > 'z') return false; // Mandarin MultiNet6 input uses untoned pinyin syllables.
    normalized += c;
    previousSpace = false;
  }
  normalized.trim();
  if (normalized.length() < 3 || normalized.length() > 96 || normalized.indexOf(' ') < 0) return false;
  pinyin = normalized;
  return true;
}

void handleSaveConfig() {
  if (server.arg("plain").length() > 10000) {
    JsonDocument e; e["error"] = "配置过大"; sendJson(413, e); return;
  }
  JsonDocument d;
  DeserializationError err = deserializeJson(d, server.arg("plain"));
  if (err) { JsonDocument e; e["error"] = "JSON 格式错误"; sendJson(400, e); return; }
  String oldWakePinyin = cfg.wakeWordPinyin;
  uint8_t oldWakeThreshold = cfg.wakeThreshold;
  uint16_t oldWakeDuration = cfg.wakeDurationMs;
  auto setString = [&](const char *key, String &dst) {
    if (d[key].is<const char*>()) dst = d[key].as<String>();
  };
  setString("wifiSsid", cfg.wifiSsid);
  setString("llmProvider", cfg.llmProvider); setString("llmEndpoint", cfg.llmEndpoint);
  setString("llmModel", cfg.llmModel); setString("systemPrompt", cfg.systemPrompt);
  setString("asrProvider", cfg.asrProvider); setString("asrEndpoint", cfg.asrEndpoint); setString("asrModel", cfg.asrModel);
  setString("ttsProvider", cfg.ttsProvider); setString("ttsEndpoint", cfg.ttsEndpoint);
  setString("ttsModel", cfg.ttsModel); setString("ttsVoice", cfg.ttsVoice);
  setString("wakeWordZh", cfg.wakeWordZh); setString("wakeWordPinyin", cfg.wakeWordPinyin);
  // Empty API key fields mean "keep existing key". Keys are never returned by GET /api/config.
  String key;
  key = jsonString(d["llmApiKey"]); if (!key.isEmpty()) cfg.llmApiKey = key;
  key = jsonString(d["asrApiKey"]); if (!key.isEmpty()) cfg.asrApiKey = key;
  key = jsonString(d["ttsApiKey"]); if (!key.isEmpty()) cfg.ttsApiKey = key;
  if (d["wifiPassword"].is<const char*>()) {
    key = d["wifiPassword"].as<String>();
    if (!key.isEmpty()) cfg.wifiPassword = key;
  }
  if (d["ttsSpeed"].is<float>()) {
    float v = d["ttsSpeed"].as<float>();
    if (v >= 0.5f && v <= 2.0f) cfg.ttsSpeed = v;
  }
  if (d["wakeThreshold"].is<int>()) {
    int v = d["wakeThreshold"].as<int>();
    if (v >= 1 && v <= 99) cfg.wakeThreshold = (uint8_t)v;
  }
  if (d["wakeDurationMs"].is<int>()) {
    int v = d["wakeDurationMs"].as<int>();
    if (v >= 1000 && v <= 5000) cfg.wakeDurationMs = (uint16_t)v;
  }
  if (!normalizeMultiNetPinyin(cfg.wakeWordPinyin)) {
    cfg.wakeWordPinyin = oldWakePinyin;
    cfg.wakeThreshold = oldWakeThreshold;
    cfg.wakeDurationMs = oldWakeDuration;
    JsonDocument e; e["error"] = "拼音格式错误：请使用小写英文字母，音节之间用空格分隔，不要写声调数字。例如 ni hao da fei yu"; sendJson(400, e); return;
  }
  if (d["noiseSuppression"].is<bool>()) cfg.noiseSuppression = d["noiseSuppression"].as<bool>();
  if (d["keepApEnabled"].is<bool>()) cfg.keepApEnabled = d["keepApEnabled"].as<bool>();
  saveConfig();
  if (!cfg.wifiSsid.isEmpty()) {
    WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPassword.c_str());
    wifiConnecting = true;
    wifiAttemptAt = millis();
    setStage("正在连接 Wi-Fi");
  }
  bool wakeChanged = oldWakePinyin != cfg.wakeWordPinyin || oldWakeThreshold != cfg.wakeThreshold || oldWakeDuration != cfg.wakeDurationMs;
  JsonDocument r; r["ok"] = true;
  r["message"] = wakeChanged ? "配置已保存；唤醒词/阈值需要重启后生效。Wi-Fi 正在连接" : "配置已保存；Wi-Fi 正在连接";
  r["restartRequired"] = wakeChanged;
  sendJson(200, r);
}

void handleReboot() {
  JsonDocument r; r["ok"] = true; sendJson(200, r);
  delay(300);
  ESP.restart();
}

// --------------------------- Audio capture / NS -----------------------------
// Forward declarations used by the capture path.
void softwareNoiseGate(int16_t *pcm, size_t count);
bool runEspSrNoiseSuppression(const int16_t *input, size_t samples,
                              int16_t **output, size_t *outputSamples);

// INMP441 transmits 24-bit audio in 32-bit I2S slots. Start the peripheral
// with 32-bit slots, then ask Arduino-ESP32 to convert RX samples to 16-bit PCM.
bool beginMic() {
  micInitialized = false;
  micI2S.end();
  micI2S.setPins(MIC_BCLK_PIN, MIC_WS_PIN, -1, MIC_DIN_PIN);
  if (!micI2S.begin(I2S_MODE_STD, AUDIO_SAMPLE_RATE,
                    I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO,
                    I2S_STD_SLOT_LEFT)) {
    Serial.println("[I2S] microphone begin failed");
    return false;
  }
  if (!micI2S.configureRX(AUDIO_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                          I2S_SLOT_MODE_MONO, I2S_RX_TRANSFORM_32_TO_16,
                          I2S_STD_SLOT_LEFT)) {
    Serial.println("[I2S] microphone RX transform configuration failed");
    micI2S.end();
    return false;
  }
  micI2S.setTimeout(250);
  micInitialized = true;
  Serial.printf("[I2S] INMP441 ready: BCLK=%d WS=%d SD=%d, 16 kHz mono PCM, port=%d\n",
                MIC_BCLK_PIN, MIC_WS_PIN, MIC_DIN_PIN, (int)micI2S.getPort());
  return true;
}

bool beginWakeAfe() {
  // MultiNet expects AFE-enhanced 16 kHz, 16-bit, mono samples. This is the
  // single-microphone VC pipeline with WebRTC NS; AEC is disabled because
  // this board has no playback-reference channel wired to the microphone AFE.
  afe_config_t *afeConfig = afe_config_init("M", nullptr, AFE_TYPE_VC, AFE_MODE_LOW_COST);
  if (afeConfig == nullptr) {
    lastError = "AFE 配置创建失败";
    return false;
  }
  afeConfig->aec_init = false;
  afeConfig->se_init = false;
  afeConfig->ns_init = true;
  afeConfig->afe_ns_mode = AFE_NS_MODE_WEBRTC;
  afeConfig->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
  afeConfig->vad_init = false;
  afeConfig->wakenet_init = false;

  wakeAfe = esp_afe_handle_from_config(afeConfig);
  if (wakeAfe == nullptr) {
    afe_config_free(afeConfig);
    lastError = "无法取得 ESP-SR AFE 接口";
    return false;
  }
  wakeAfeData = wakeAfe->create_from_config(afeConfig);
  afe_config_free(afeConfig);
  if (wakeAfeData == nullptr) {
    wakeAfe = nullptr;
    lastError = "ESP-SR AFE 初始化失败";
    return false;
  }

  int feedSamples = wakeAfe->get_feed_chunksize(wakeAfeData);
  int feedChannels = wakeAfe->get_feed_channel_num(wakeAfeData);
  int fetchSamples = wakeAfe->get_fetch_chunksize(wakeAfeData);
  if (feedSamples <= 0 || feedChannels != 1 || fetchSamples <= 0 || feedSamples > 4096 || fetchSamples > 4096) {
    wakeAfe->destroy(wakeAfeData);
    wakeAfeData = nullptr;
    wakeAfe = nullptr;
    lastError = "AFE 音频帧参数无效（需要单麦克风 16 kHz）";
    return false;
  }

  wakeAfeFeedSamples = (size_t)feedSamples;
  wakeAfeFeedChannels = (size_t)feedChannels;
  wakeAfeFetchSamples = (size_t)fetchSamples;
  wakeAfeFeedFrame = (int16_t *)heap_caps_malloc(
      wakeAfeFeedSamples * wakeAfeFeedChannels * sizeof(int16_t),
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (wakeAfeFeedFrame == nullptr) {
    wakeAfe->destroy(wakeAfeData);
    wakeAfeData = nullptr;
    wakeAfe = nullptr;
    wakeAfeFeedSamples = wakeAfeFeedChannels = wakeAfeFetchSamples = 0;
    lastError = "AFE 麦克风输入缓冲区分配失败";
    return false;
  }
  Serial.printf("[AFE] Wake AFE ready: feed=%u x %u, fetch=%u samples, WebRTC NS on\n",
                (unsigned)wakeAfeFeedSamples, (unsigned)wakeAfeFeedChannels,
                (unsigned)wakeAfeFetchSamples);
  return true;
}

void destroyWakeAfe() {
  if (wakeAfeData != nullptr && wakeAfe != nullptr) {
    wakeAfe->destroy(wakeAfeData);
  }
  wakeAfeData = nullptr;
  wakeAfe = nullptr;
  if (wakeAfeFeedFrame != nullptr) {
    free(wakeAfeFeedFrame);
    wakeAfeFeedFrame = nullptr;
  }
  wakeAfeFeedSamples = wakeAfeFeedChannels = wakeAfeFetchSamples = 0;
  multiNetFrameFill = 0;
}

void multiNetDetectionTask(void *arg) {
  (void)arg;
  const size_t feedBytes = wakeAfeFeedSamples * wakeAfeFeedChannels * sizeof(int16_t);
  bool wasPaused = false;
  while (multiNetTaskRunning) {
    if (assistantBusy || wakePending || !multiNetData || !multiNet || !wakeAfe || !wakeAfeData) {
      if (assistantBusy && !wasPaused && wakeAfe && wakeAfeData) {
        wakeAfe->reset_buffer(wakeAfeData);
        multiNet->clean(multiNetData);
        multiNetFrameFill = 0;
        wasPaused = true;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (wasPaused) {
      wakeAfe->reset_buffer(wakeAfeData);
      multiNet->clean(multiNetData);
      multiNetFrameFill = 0;
      wasPaused = false;
    }
    if (micMutex == nullptr || xSemaphoreTake(micMutex, pdMS_TO_TICKS(250)) != pdTRUE) {
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    if (assistantBusy || wakePending || !multiNetTaskRunning) {
      xSemaphoreGive(micMutex);
      continue;
    }
    size_t got = micI2S.readBytes((char *)wakeAfeFeedFrame, feedBytes);
    xSemaphoreGive(micMutex);
    if (got != feedBytes) continue;

    if (wakeAfe->feed(wakeAfeData, wakeAfeFeedFrame) < 0) continue;
    afe_fetch_result_t *afeResult = wakeAfe->fetch_with_delay(wakeAfeData, pdMS_TO_TICKS(1000));
    if (afeResult == nullptr || afeResult->ret_value == ESP_FAIL ||
        afeResult->data == nullptr || afeResult->data_size <= 0) {
      continue;
    }

    size_t outputSamples = (size_t)afeResult->data_size / sizeof(int16_t);
    for (size_t i = 0; i < outputSamples; ++i) {
      multiNetFrame[multiNetFrameFill++] = afeResult->data[i];
      if (multiNetFrameFill < multiNetFrameSamples) continue;
      multiNetFrameFill = 0;

      esp_mn_state_t state = multiNet->detect(multiNetData, multiNetFrame);
      if (state == ESP_MN_STATE_DETECTED) {
        esp_mn_results_t *results = multiNet->get_results(multiNetData);
        if (results != nullptr && results->num > 0) {
          Serial.printf("[MultiNet] detected '%s', probability=%.3f\n",
                        cfg.wakeWordPinyin.c_str(), results->prob[0]);
          wakePending = true;
        }
        multiNet->clean(multiNetData);
        break;
      } else if (state == ESP_MN_STATE_TIMEOUT) {
        multiNet->clean(multiNetData);
      }
    }
  }
  multiNetTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

void stopMultiNet() {
  multiNetTaskRunning = false;
  const uint32_t waitStart = millis();
  while (multiNetTaskHandle != nullptr && millis() - waitStart < 3000) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (multiNetTaskHandle != nullptr) {
    Serial.println("[MultiNet] task did not stop in time; leaving model allocated");
    return;
  }
  destroyWakeAfe();
  if (multiNetData != nullptr && multiNet != nullptr) {
    multiNet->destroy(multiNetData);
    multiNetData = nullptr;
  }
  if (srModels != nullptr) {
    esp_srmodel_deinit(srModels);
    srModels = nullptr;
  }
  multiNet = nullptr;
  multiNetName = nullptr;
  if (multiNetFrame != nullptr) {
    free(multiNetFrame);
    multiNetFrame = nullptr;
  }
  multiNetFrameSamples = 0;
  srStarted = false;
  recognizerStatus = "已停止";
}

bool beginSpeechEngine() {
  const esp_partition_t *modelPart = findModelPartition();
  if (modelPart == nullptr) {
    srStarted = false;
    recognizerStatus = "model 分区不存在";
    lastError = "ESP-SR 模型分区不存在：需要使用本项目分区表重新烧录";
    Serial.println("[MultiNet] model partition missing; Web/AP remains available.");
    return false;
  }
  if (modelPartitionBlank(modelPart)) {
    srStarted = false;
    recognizerStatus = "model 分区为空";
    lastError = "ESP-SR 模型分区为空：请上传包含 mn6_cn + fst 的 srmodels.bin";
    Serial.printf("[MultiNet] model partition at 0x%08X is blank.\n", (unsigned)modelPart->address);
    return false;
  }
  if (micMutex == nullptr) {
    micMutex = xSemaphoreCreateMutex();
    if (micMutex == nullptr) {
      recognizerStatus = "麦克风互斥锁创建失败";
      lastError = recognizerStatus;
      return false;
    }
  }

  srModels = esp_srmodel_init("model");
  if (srModels == nullptr || srModels->num <= 0) {
    if (srModels != nullptr) { esp_srmodel_deinit(srModels); srModels = nullptr; }
    srStarted = false;
    recognizerStatus = "模型索引读取失败";
    lastError = "无法读取 ESP-SR 模型索引；请确认上传的是 ESP-SR 2.1.5 的 MultiNet6 模型包";
    Serial.println("[MultiNet] esp_srmodel_init failed or returned no models.");
    return false;
  }

  multiNetName = esp_srmodel_filter(srModels, ESP_MN_PREFIX, "cn");
  if (multiNetName == nullptr) {
    esp_srmodel_deinit(srModels); srModels = nullptr;
    srStarted = false;
    recognizerStatus = "未找到 mn6_cn";
    lastError = "模型包中没有中文 MultiNet，请重新生成包含 mn6_cn + fst 的 srmodels.bin";
    Serial.println("[MultiNet] Chinese MultiNet model not found in model partition.");
    return false;
  }

  multiNet = esp_mn_handle_from_name(multiNetName);
  if (multiNet == nullptr) {
    esp_srmodel_deinit(srModels); srModels = nullptr; multiNetName = nullptr;
    srStarted = false;
    recognizerStatus = "MultiNet 接口加载失败";
    lastError = recognizerStatus;
    return false;
  }

  multiNetData = multiNet->create(multiNetName, cfg.wakeDurationMs);
  if (multiNetData == nullptr) {
    multiNet = nullptr;
    esp_srmodel_deinit(srModels); srModels = nullptr; multiNetName = nullptr;
    srStarted = false;
    recognizerStatus = "MultiNet 模型初始化失败";
    lastError = "MultiNet create 失败；请检查模型包与 ESP-SR 版本是否匹配";
    return false;
  }

  multiNet->set_det_threshold(multiNetData, cfg.wakeThreshold / 100.0f);
  esp_mn_commands_clear();
  char command[100];
  memset(command, 0, sizeof(command));
  cfg.wakeWordPinyin.toCharArray(command, sizeof(command));
  esp_err_t addResult = esp_mn_commands_add(1, command);
  if (addResult != ESP_OK) {
    multiNet->destroy(multiNetData); multiNetData = nullptr;
    multiNet = nullptr;
    esp_srmodel_deinit(srModels); srModels = nullptr; multiNetName = nullptr;
    srStarted = false;
    recognizerStatus = "拼音命令词格式不被模型接受";
    lastError = "MultiNet 拒绝拼音命令词，请检查拼音是否为小写字母并用空格分隔";
    Serial.printf("[MultiNet] esp_mn_commands_add failed: %s\n", esp_err_to_name(addResult));
    return false;
  }
  esp_mn_commands_update();
  multiNet->print_active_speech_commands(multiNetData);

  int chunkSamples = multiNet->get_samp_chunksize(multiNetData);
  if (chunkSamples <= 0 || chunkSamples > 4096) {
    multiNet->destroy(multiNetData); multiNetData = nullptr;
    multiNet = nullptr;
    esp_srmodel_deinit(srModels); srModels = nullptr; multiNetName = nullptr;
    srStarted = false;
    recognizerStatus = "MultiNet 音频帧长度无效";
    lastError = recognizerStatus;
    return false;
  }
  multiNetFrameSamples = (size_t)chunkSamples;
  multiNetFrame = (int16_t *)heap_caps_malloc(multiNetFrameSamples * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (multiNetFrame == nullptr) {
    multiNet->destroy(multiNetData); multiNetData = nullptr;
    multiNet = nullptr;
    esp_srmodel_deinit(srModels); srModels = nullptr; multiNetName = nullptr;
    multiNetFrameSamples = 0;
    srStarted = false;
    recognizerStatus = "MultiNet 音频缓冲区内存不足";
    lastError = recognizerStatus;
    return false;
  }

  if (!beginWakeAfe()) {
    free(multiNetFrame); multiNetFrame = nullptr;
    multiNetFrameSamples = 0;
    multiNet->destroy(multiNetData); multiNetData = nullptr;
    multiNet = nullptr;
    esp_srmodel_deinit(srModels); srModels = nullptr; multiNetName = nullptr;
    srStarted = false;
    recognizerStatus = "Wake AFE 初始化失败";
    return false;
  }

  if (micMutex == nullptr) micMutex = xSemaphoreCreateMutex();
  if (micMutex == nullptr) {
    destroyWakeAfe();
    free(multiNetFrame); multiNetFrame = nullptr;
    multiNet->destroy(multiNetData); multiNetData = nullptr;
    multiNet = nullptr;
    esp_srmodel_deinit(srModels); srModels = nullptr; multiNetName = nullptr;
    srStarted = false;
    recognizerStatus = "麦克风互斥锁创建失败";
    lastError = recognizerStatus;
    return false;
  }

  micI2S.setTimeout(250);
  multiNetTaskRunning = true;
  BaseType_t taskOk = xTaskCreatePinnedToCore(
      multiNetDetectionTask, "multinet_detect", 8192, nullptr, 5,
      &multiNetTaskHandle, 1);
  if (taskOk != pdPASS) {
    multiNetTaskRunning = false;
    multiNetTaskHandle = nullptr;
    destroyWakeAfe();
    free(multiNetFrame); multiNetFrame = nullptr;
    multiNetFrameSamples = 0;
    multiNet->destroy(multiNetData); multiNetData = nullptr;
    multiNet = nullptr;
    esp_srmodel_deinit(srModels); srModels = nullptr; multiNetName = nullptr;
    srStarted = false;
    recognizerStatus = "MultiNet 任务创建失败";
    lastError = recognizerStatus;
    return false;
  }
  srStarted = true;
  recognizerStatus = String("已加载 ") + multiNetName + "，唤醒词：" + cfg.wakeWordZh;
  lastError = "";
  Serial.printf("[MultiNet] ready model=%s phrase=%s pinyin=%s threshold=%u%% duration=%u ms MN-frame=%u AFE-fetch=%u samples\n",
                multiNetName, cfg.wakeWordZh.c_str(), cfg.wakeWordPinyin.c_str(),
                cfg.wakeThreshold, cfg.wakeDurationMs, (unsigned)multiNetFrameSamples,
                (unsigned)wakeAfeFetchSamples);
  return true;
}

void writeWavHeader(uint8_t *h, uint32_t dataBytes, uint32_t sampleRate) {
  memcpy(h, "RIFF", 4); writeLE32(h + 4, 36 + dataBytes); memcpy(h + 8, "WAVE", 4);
  memcpy(h + 12, "fmt ", 4); writeLE32(h + 16, 16); writeLE16(h + 20, 1);
  writeLE16(h + 22, 1); writeLE32(h + 24, sampleRate);
  writeLE32(h + 28, sampleRate * 2); writeLE16(h + 32, 2); writeLE16(h + 34, 16);
  memcpy(h + 36, "data", 4); writeLE32(h + 40, dataBytes);
}

bool captureSpeech(uint8_t **wavOut, size_t *wavLenOut) {
  *wavOut = nullptr; *wavLenOut = 0;
  // assistantBusy is set by the caller. Wait until the MultiNet reader has
  // released I2S, then own the microphone during the bounded cloud-ASR capture.
  if (micMutex == nullptr || xSemaphoreTake(micMutex, pdMS_TO_TICKS(1500)) != pdTRUE) {
    lastError = "无法独占麦克风 I2S";
    return false;
  }
  micI2S.setTimeout(300);

  const size_t capacitySamples = AUDIO_SAMPLE_RATE * MAX_RECORD_SECONDS;
  int16_t *pcm = (int16_t*)ps_malloc(capacitySamples * sizeof(int16_t));
  if (!pcm) pcm = (int16_t*)malloc(capacitySamples * sizeof(int16_t));
  if (!pcm) { micI2S.setTimeout(250); xSemaphoreGive(micMutex); lastError = "音频缓冲区内存不足"; return false; }

  int16_t block[256];
  size_t total = 0;
  uint32_t speechMs = 0, silenceMs = 0;
  bool speechStarted = false;
  const uint32_t startAt = millis();
  while (total < capacitySamples && millis() - startAt < MAX_RECORD_SECONDS * 1000UL) {
    size_t wantBytes = sizeof(block);
    size_t gotBytes = micI2S.readBytes((char*)block, wantBytes);
    size_t samples = gotBytes / sizeof(int16_t);
    if (!samples) { delay(2); continue; }

    double energy = 0;
    for (size_t i = 0; i < samples; ++i) energy += (double)block[i] * block[i];
    float rms = sqrtf((float)(energy / samples));
    bool voiced = rms > 420.0f;
    if (voiced) { speechStarted = true; speechMs += (uint32_t)(samples * 1000 / AUDIO_SAMPLE_RATE); silenceMs = 0; }
    else if (speechStarted) silenceMs += (uint32_t)(samples * 1000 / AUDIO_SAMPLE_RATE);

    memcpy(pcm + total, block, samples * sizeof(int16_t));
    total += samples;
    if (speechStarted && speechMs >= 350 && silenceMs >= 900) break;
  }

  micI2S.setTimeout(250);
  xSemaphoreGive(micMutex);
  if (!speechStarted || total < AUDIO_SAMPLE_RATE / 3) {
    free(pcm);
    lastError = "没有检测到足够长的语音，请靠近麦克风再说一次";
    return false;
  }

  // Optional single-channel noise suppression using ESP-SR AFE WebRTC NS.
  int16_t *cleanPcm = pcm;
  size_t cleanSamples = total;
  if (cfg.noiseSuppression) {
    int16_t *processed = nullptr;
    size_t processedSamples = 0;
    if (runEspSrNoiseSuppression(pcm, total, &processed, &processedSamples) && processedSamples > 1000) {
      free(pcm);
      cleanPcm = processed;
      cleanSamples = processedSamples;
      lastNsStatus = "ESP-SR AFE WebRTC NS 已处理";
    } else {
      lastNsStatus = "AFE NS 初始化失败，已回退软件噪声门";
      softwareNoiseGate(pcm, total);
    }
  } else {
    lastNsStatus = "已关闭";
  }

  size_t dataBytes = cleanSamples * sizeof(int16_t);
  uint8_t *wav = (uint8_t*)ps_malloc(dataBytes + 44);
  if (!wav) wav = (uint8_t*)malloc(dataBytes + 44);
  if (!wav) { free(cleanPcm); lastError = "WAV 缓冲区内存不足"; return false; }
  writeWavHeader(wav, dataBytes, AUDIO_SAMPLE_RATE);
  memcpy(wav + 44, cleanPcm, dataBytes);
  free(cleanPcm);
  *wavOut = wav;
  *wavLenOut = dataBytes + 44;
  return true;
}

void softwareNoiseGate(int16_t *pcm, size_t count) {
  // Conservative fallback only; AFE NS is preferred when its component/model is available.
  const size_t frame = 160;
  for (size_t off = 0; off < count; off += frame) {
    size_t n = min(frame, count - off);
    double e = 0;
    for (size_t i = 0; i < n; ++i) e += (double)pcm[off + i] * pcm[off + i];
    float rms = sqrtf((float)(e / n));
    if (rms < 500.0f) {
      for (size_t i = 0; i < n; ++i) pcm[off + i] = (int16_t)(pcm[off + i] * 0.18f);
    }
  }
}

bool runEspSrNoiseSuppression(const int16_t *input, size_t samples,
                              int16_t **output, size_t *outputSamples) {
  *output = nullptr; *outputSamples = 0;
  // AFE_TYPE_VC is the ESP-SR voice-communication path, which exposes nonlinear NS.
  afe_config_t *config = afe_config_init("M", nullptr, AFE_TYPE_VC, AFE_MODE_LOW_COST);
  if (!config) return false;
  config->aec_init = false;
  config->se_init = false;
  config->ns_init = true;
  // AFE_TYPE_VC uses the voice-communication pipeline; keep its library-owned
  // ns_model_name pointer intact and explicitly select the WebRTC NS path.
  config->afe_ns_mode = AFE_NS_MODE_WEBRTC;
  config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
  config->vad_init = false;
  config->wakenet_init = false;

  const esp_afe_sr_iface_t *afe = esp_afe_handle_from_config(config);
  if (!afe) { afe_config_free(config); return false; }
  esp_afe_sr_data_t *data = afe->create_from_config(config);
  afe_config_free(config);
  if (!data) return false;

  int feedChunk = afe->get_feed_chunksize(data);
  int feedChannels = afe->get_feed_channel_num(data);
  int fetchChunk = afe->get_fetch_chunksize(data);
  if (feedChunk <= 0 || feedChannels <= 0 || fetchChunk <= 0) { afe->destroy(data); return false; }

  size_t maxOutSamples = samples + AUDIO_SAMPLE_RATE;
  int16_t *out = (int16_t*)ps_malloc(maxOutSamples * sizeof(int16_t));
  if (!out) out = (int16_t*)malloc(maxOutSamples * sizeof(int16_t));
  int16_t *feed = (int16_t*)heap_caps_malloc(feedChunk * feedChannels * sizeof(int16_t), MALLOC_CAP_8BIT);
  if (!out || !feed) {
    if (out) free(out);
    if (feed) free(feed);
    afe->destroy(data);
    return false;
  }
  size_t outCount = 0;
  bool ok = true;
  for (size_t off = 0; off < samples; off += feedChunk) {
    size_t n = min((size_t)feedChunk, samples - off);
    memset(feed, 0, feedChunk * feedChannels * sizeof(int16_t));
    for (size_t i = 0; i < n; ++i) feed[i * feedChannels] = input[off + i];
    if (afe->feed(data, feed) < 0) { ok = false; break; }
    afe_fetch_result_t *res = afe->fetch_with_delay(data, pdMS_TO_TICKS(1200));
    if (!res || res->ret_value == ESP_FAIL || res->data_size <= 0) { ok = false; break; }
    size_t got = (size_t)res->data_size / sizeof(int16_t);
    if (outCount + got > maxOutSamples) got = maxOutSamples - outCount;
    memcpy(out + outCount, res->data, got * sizeof(int16_t));
    outCount += got;
  }
  free(feed);
  afe->destroy(data);
  if (!ok || outCount < 1000) { free(out); return false; }
  *output = out;
  *outputSamples = outCount;
  return true;
}

// --------------------------- ASR / LLM / TTS -------------------------------
String transcribeQwenAudioASR(const uint8_t *wav, size_t wavLen) {
  if (WiFi.status() != WL_CONNECTED) { lastError = "Wi-Fi 未连接，无法调用 Qwen ASR"; return ""; }
  if (cfg.asrApiKey.isEmpty()) { lastError = "请先在后台填写 ASR API Key"; return ""; }
  if (!startsWithHttps(cfg.asrEndpoint)) { lastError = "Qwen ASR API 地址无效：必须以 https:// 开头（已忽略首尾空格及协议大小写）"; return ""; }
  // Qwen Audio ASR accepts WAV as a Base64 Data URI. The 8-second capture
  // limit keeps the expanded JSON request well below the model's 10 MB limit.
  size_t encodedCap = 4 * ((wavLen + 2) / 3) + 8;
  unsigned char *encoded = (unsigned char*)ps_malloc(encodedCap);
  if (!encoded) encoded = (unsigned char*)malloc(encodedCap);
  if (!encoded) { lastError = "Qwen ASR Base64 缓冲区内存不足"; return ""; }
  size_t encodedLen = 0;
  int b64rc = mbedtls_base64_encode(encoded, encodedCap, &encodedLen, wav, wavLen);
  if (b64rc != 0) { free(encoded); lastError = "Qwen ASR Base64 编码失败"; return ""; }

  String dataUri;
  dataUri.reserve(22 + encodedLen);
  dataUri = "data:audio/wav;base64,";
  dataUri.concat((const char*)encoded, encodedLen);
  free(encoded);

  JsonDocument req;
  req["model"] = cfg.asrModel;
  JsonArray messages = req["input"]["messages"].to<JsonArray>();
  JsonObject message = messages.add<JsonObject>();
  message["role"] = "user";
  JsonArray content = message["content"].to<JsonArray>();
  JsonObject audio = content.add<JsonObject>();
  audio["type"] = "input_audio";
  audio["input_audio"]["data"] = dataUri;
  req["parameters"]["format"] = "wav";
  req["parameters"]["sample_rate"] = "16000";

  String body;
  body.reserve(dataUri.length() + 512);
  serializeJson(req, body);
  dataUri = String();

  auto tls = makeTlsClient();
  HTTPClient http;
  if (!httpBegin(http, *tls, cfg.asrEndpoint, "Qwen ASR")) return "";
  addBearer(http, cfg.asrApiKey);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-DashScope-SSE", "disable");
  http.setTimeout(70000);
  int code = http.POST(body);
  body = String();
  String response = http.getString();
  http.end();
  if (code < 200 || code >= 300) {
    lastError = "Qwen ASR HTTP " + String(code) + ": " + response.substring(0, 300);
    return "";
  }
  JsonDocument doc;
  if (deserializeJson(doc, response)) { lastError = "Qwen ASR 返回 JSON 解析失败"; return ""; }
  String recognized = jsonString(doc["output"]["text"]);
  if (recognized.isEmpty()) lastError = "Qwen ASR 没有返回识别文本";
  return recognized;
}

String transcribeAudio(const uint8_t *wav, size_t wavLen) {
  if (cfg.asrProvider == "qwen-audio-asr-http") return transcribeQwenAudioASR(wav, wavLen);
  if (WiFi.status() != WL_CONNECTED) { lastError = "Wi-Fi 未连接，无法调用 ASR"; return ""; }
  if (cfg.asrApiKey.isEmpty()) { lastError = "请先在后台填写 ASR API Key"; return ""; }
  if (!startsWithHttps(cfg.asrEndpoint)) { lastError = "ASR API 地址无效：必须以 https:// 开头（已忽略首尾空格及协议大小写）"; return ""; }

  String boundary = "----ESP32VoiceBoundary7MA4YWxkTrZu0gW";
  String head = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n" + cfg.asrModel + "\r\n";
  head += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\njson\r\n";
  head += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"voice.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
  String tail = "\r\n--" + boundary + "--\r\n";
  size_t totalLen = head.length() + wavLen + tail.length();
  uint8_t *body = (uint8_t*)ps_malloc(totalLen);
  if (!body) body = (uint8_t*)malloc(totalLen);
  if (!body) { lastError = "ASR 请求内存不足"; return ""; }
  size_t pos = 0;
  memcpy(body + pos, head.c_str(), head.length()); pos += head.length();
  memcpy(body + pos, wav, wavLen); pos += wavLen;
  memcpy(body + pos, tail.c_str(), tail.length());

  auto tls = makeTlsClient();
  HTTPClient http;
  if (!httpBegin(http, *tls, cfg.asrEndpoint, "ASR")) { free(body); return ""; }
  addBearer(http, cfg.asrApiKey);
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
  int code = http.POST(body, totalLen);
  free(body);
  String response = http.getString();
  http.end();
  if (code < 200 || code >= 300) {
    lastError = "ASR HTTP " + String(code) + ": " + response.substring(0, 260);
    return "";
  }
  JsonDocument doc;
  if (deserializeJson(doc, response)) { lastError = "ASR 返回 JSON 解析失败"; return ""; }
  String text = jsonString(doc["text"]);
  if (text.isEmpty()) lastError = "ASR 没有返回识别文本";
  return text;
}

String callLlm(const String &userText) {
  if (WiFi.status() != WL_CONNECTED) { lastError = "Wi-Fi 未连接，无法调用 LLM"; return ""; }
  if (cfg.llmApiKey.isEmpty()) { lastError = "请先在后台填写 LLM API Key"; return ""; }
  auto tls = makeTlsClient();
  HTTPClient http;
  if (!httpBegin(http, *tls, cfg.llmEndpoint, "LLM")) return "";
  addBearer(http, cfg.llmApiKey);
  http.addHeader("Content-Type", "application/json");

  JsonDocument req;
  req["model"] = cfg.llmModel;
  req["stream"] = false;
  JsonArray messages = req["messages"].to<JsonArray>();
  JsonObject sys = messages.add<JsonObject>(); sys["role"] = "system"; sys["content"] = cfg.systemPrompt;
  // Keep the most recent three user/assistant exchanges to limit request size.
  size_t start = conversation.size() > 6 ? conversation.size() - 6 : 0;
  for (size_t i = start; i < conversation.size(); ++i) {
    JsonObject m = messages.add<JsonObject>(); m["role"] = conversation[i].role; m["content"] = conversation[i].content;
  }
  JsonObject user = messages.add<JsonObject>(); user["role"] = "user"; user["content"] = userText;
  String body; serializeJson(req, body);
  int code = http.POST(body);
  String response = http.getString();
  http.end();
  if (code < 200 || code >= 300) { lastError = "LLM HTTP " + String(code) + ": " + response.substring(0, 280); return ""; }
  JsonDocument doc;
  if (deserializeJson(doc, response)) { lastError = "LLM 返回 JSON 解析失败"; return ""; }
  String answer = jsonString(doc["choices"][0]["message"]["content"]);
  if (answer.isEmpty()) { lastError = "LLM 没有返回文本内容"; return ""; }
  conversation.push_back({"user", userText});
  conversation.push_back({"assistant", answer});
  while (conversation.size() > 6) conversation.erase(conversation.begin());
  return answer;
}

bool writePcmWithVolume(const uint8_t *data, size_t dataLen, uint16_t channels) {
  if (data == nullptr || (channels != 1 && channels != 2)) return false;
  const int16_t *source = (const int16_t *)data;
  const size_t frames = dataLen / (sizeof(int16_t) * channels);
  int16_t output[256];
  size_t offset = 0;
  while (offset < frames) {
    const size_t count = min((size_t)256, frames - offset);
    for (size_t i = 0; i < count; ++i) {
      int32_t sample = source[(offset + i) * channels];
      if (channels == 2) sample = (sample + (int32_t)source[(offset + i) * 2 + 1]) / 2;
      output[i] = (int16_t)(sample * cfg.speakerVolume / 100);
    }
    const size_t bytes = count * sizeof(int16_t);
    if (speakerI2S.write((uint8_t *)output, bytes) != bytes) return false;
    offset += count;
  }
  return true;
}

bool playWav(const uint8_t *wav, size_t len) {
  if (!wav || len < 44 || memcmp(wav, "RIFF", 4) != 0 || memcmp(wav + 8, "WAVE", 4) != 0) {
    lastError = "TTS 音频不是 WAV/RIFF 格式"; return false;
  }
  uint16_t channels = 0, bits = 0, format = 0;
  uint32_t rate = TTS_FALLBACK_RATE;
  const uint8_t *data = nullptr; size_t dataLen = 0;
  size_t p = 12;
  while (p + 8 <= len) {
    uint32_t chunkSize = readLE32(wav + p + 4);
    if (p + 8 + chunkSize > len) break;
    if (memcmp(wav + p, "fmt ", 4) == 0 && chunkSize >= 16) {
      format = readLE16(wav + p + 8);
      channels = readLE16(wav + p + 10);
      rate = readLE32(wav + p + 12);
      bits = readLE16(wav + p + 22);
    } else if (memcmp(wav + p, "data", 4) == 0) {
      data = wav + p + 8; dataLen = chunkSize; break;
    }
    p += 8 + chunkSize + (chunkSize & 1);
  }
  if (format != 1 || bits != 16 || (channels != 1 && channels != 2) || !data || !dataLen) {
    lastError = "仅支持 16-bit PCM WAV（单声道或双声道）"; return false;
  }
  speakerI2S.end();
  speakerI2S.setPins(SPK_BCLK_PIN, SPK_WS_PIN, SPK_DOUT_PIN, -1);
  if (!speakerI2S.begin(I2S_MODE_STD, rate, I2S_DATA_BIT_WIDTH_16BIT,
                        I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
    lastError = "MAX98357A I2S 初始化失败"; return false;
  }
  const bool writeOk = writePcmWithVolume(data, dataLen, channels);
  speakerI2S.end();
  if (!writeOk) { lastError = "MAX98357A 音频数据写入不完整"; return false; }
  return true;
}

bool synthesizeAndPlay(const String &text) {
  if (WiFi.status() != WL_CONNECTED) { lastError = "Wi-Fi 未连接，无法调用 TTS"; return false; }
  if (cfg.ttsApiKey.isEmpty()) { lastError = "请先在后台填写 TTS API Key"; return false; }
  auto tls = makeTlsClient();
  HTTPClient http;
  if (!httpBegin(http, *tls, cfg.ttsEndpoint, "TTS")) return false;
  addBearer(http, cfg.ttsApiKey);
  http.addHeader("Content-Type", "application/json");

  JsonDocument req;
  req["model"] = cfg.ttsModel;
  if (cfg.ttsProvider == "dashscope-qwen") {
    JsonObject input = req["input"].to<JsonObject>();
    input["text"] = text; input["voice"] = cfg.ttsVoice;
    input["format"] = "wav"; input["sample_rate"] = 24000; input["rate"] = cfg.ttsSpeed;
  } else {
    req["input"] = text; req["voice"] = cfg.ttsVoice;
    req["response_format"] = "wav"; req["speed"] = cfg.ttsSpeed;
  }
  String body; serializeJson(req, body);
  int code = http.POST(body);
  String response;
  if (code < 200 || code >= 300) {
    response = http.getString(); http.end();
    lastError = "TTS HTTP " + String(code) + ": " + response.substring(0, 280);
    return false;
  }

  if (cfg.ttsProvider == "dashscope-qwen") {
    response = http.getString(); http.end();
    JsonDocument doc;
    if (deserializeJson(doc, response)) { lastError = "DashScope TTS JSON 解析失败"; return false; }
    String audioUrl = jsonString(doc["output"]["audio"]["url"]);
    if (audioUrl.isEmpty()) { lastError = "DashScope TTS 没有返回音频 URL"; return false; }
    auto tls2 = makeTlsClient();
    HTTPClient get;
    get.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!httpBegin(get, *tls2, audioUrl, "TTS 音频下载")) return false;
    int getCode = get.GET();
    if (getCode < 200 || getCode >= 300) { String e = get.getString(); get.end(); lastError = "下载 TTS 音频失败：" + String(getCode) + " " + e.substring(0, 180); return false; }
    String wav = get.getString(); get.end();
    if (wav.length() > 900000) { lastError = "TTS 音频过大"; return false; }
    return playWav((const uint8_t*)wav.c_str(), wav.length());
  }

  response = http.getString();
  http.end();
  if (response.length() > 900000) { lastError = "TTS 音频过大"; return false; }
  return playWav((const uint8_t*)response.c_str(), response.length());
}

void processUserText(const String &text, bool speak) {
  if (text.isEmpty()) return;
  lastError = "";
  lastTranscript = text;
  setStage("请求大模型");
  String reply = callLlm(text);
  if (reply.isEmpty()) { setStage("请求失败"); return; }
  lastReply = reply;
  if (speak) {
    setStage("语音合成与播放");
    if (!synthesizeAndPlay(reply)) Serial.printf("[TTS] %s\n", lastError.c_str());
  }
}

void handleVolume() {
  JsonDocument req;
  if (deserializeJson(req, server.arg("plain"))) { JsonDocument e; e["error"] = "JSON 格式错误"; sendJson(400, e); return; }
  int value = req["volume"] | -1;
  if (value < 0 || value > 100) { JsonDocument e; e["error"] = "音量范围必须是 0-100"; sendJson(400, e); return; }
  cfg.speakerVolume = (uint8_t)value;
  prefs.begin("voicecfg", false);
  prefs.putUChar("vol", cfg.speakerVolume);
  prefs.end();
  JsonDocument r; r["ok"] = true; r["volume"] = cfg.speakerVolume; sendJson(200, r);
}

void handleMicTest() {
  if (!micInitialized) { JsonDocument e; e["error"] = "INMP441 的 I2S 尚未初始化成功"; sendJson(503, e); return; }
  if (assistantBusy) { JsonDocument e; e["error"] = "语音助手正在处理，请稍后"; sendJson(409, e); return; }
  assistantBusy = true; // pause MultiNet's background reader while measuring the microphone
  if (micMutex == nullptr || xSemaphoreTake(micMutex, pdMS_TO_TICKS(1500)) != pdTRUE) {
    assistantBusy = false; JsonDocument e; e["error"] = "无法独占麦克风 I2S，请重试"; sendJson(503, e); return;
  }

  micI2S.setTimeout(120);
  int16_t block[256];
  uint64_t sumSquares = 0;
  uint32_t count = 0;
  int32_t peak = 0;
  const uint32_t started = millis();
  while (millis() - started < 700) {
    size_t got = micI2S.readBytes((char *)block, sizeof(block));
    size_t samples = got / sizeof(int16_t);
    for (size_t i = 0; i < samples; ++i) {
      int32_t sample = block[i];
      int32_t magnitude = sample < 0 ? -sample : sample;
      if (magnitude > peak) peak = magnitude;
      sumSquares += (uint64_t)((int64_t)sample * sample);
      ++count;
    }
    if (samples == 0) delay(2);
  }
  micI2S.setTimeout(250);
  xSemaphoreGive(micMutex);
  assistantBusy = false;

  const double rms = count ? sqrt((double)sumSquares / (double)count) : 0.0;
  const bool signalDetected = count > 0 && peak >= 32 && rms >= 5.0;
  JsonDocument r;
  r["ok"] = true; r["initialized"] = true; r["signal"] = signalDetected;
  r["samples"] = count; r["rms"] = (uint32_t)rms; r["peak"] = peak;
  r["message"] = signalDetected
      ? "INMP441 I2S 已初始化，并检测到声音输入。"
      : "I2S 已初始化，但没有检测到明显声音。请在检测期间说话或轻敲麦克风外壳；若数值仍接近 0，再检查 SD、L/R、3.3V 和 GND。";
  sendJson(200, r);
}

void handleSpeakerTest() {
  if (assistantBusy) { JsonDocument e; e["error"] = "语音助手正在处理，请稍后"; sendJson(409, e); return; }
  if (cfg.speakerVolume == 0) { JsonDocument e; e["error"] = "当前音量为 0，请先把音量滑条调高"; sendJson(400, e); return; }
  assistantBusy = true;
  speakerI2S.end();
  speakerI2S.setPins(SPK_BCLK_PIN, SPK_WS_PIN, SPK_DOUT_PIN, -1);
  if (!speakerI2S.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT,
                        I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
    assistantBusy = false;
    JsonDocument e; e["error"] = "MAX98357A I2S 初始化失败；请检查 BCLK、WS、DIN 接线"; sendJson(503, e); return;
  }

  int16_t tone[256];
  const int32_t amplitude = 10000 * cfg.speakerVolume / 100;
  for (int i = 0; i < 256; ++i) tone[i] = (int16_t)(amplitude * sinf(2.0f * PI * 880.0f * i / 16000.0f));
  size_t totalWritten = 0;
  for (int block = 0; block < 40; ++block) {
    size_t written = speakerI2S.write((uint8_t *)tone, sizeof(tone));
    totalWritten += written;
    if (written != sizeof(tone)) break;
  }
  speakerI2S.end();
  assistantBusy = false;

  if (totalWritten == 0) { JsonDocument e; e["error"] = "I2S 已启动，但没有成功写入测试音频"; sendJson(503, e); return; }
  JsonDocument r; r["ok"] = true; r["bytesWritten"] = totalWritten;
  r["message"] = String("测试音已发送到 MAX98357A，音量 ") + cfg.speakerVolume + "%。请确认喇叭是否听到短音。I2S 无法回读喇叭是否物理连接。";
  sendJson(200, r);
}

void handleChatTest() {
  JsonDocument req;
  if (deserializeJson(req, server.arg("plain"))) { JsonDocument e; e["error"] = "JSON 格式错误"; sendJson(400, e); return; }
  String text = jsonString(req["text"]);
  if (text.isEmpty()) { JsonDocument e; e["error"] = "text 不能为空"; sendJson(400, e); return; }
  if (assistantBusy) { JsonDocument e; e["error"] = "语音助手正在处理，请稍后"; sendJson(409, e); return; }
  assistantBusy = true;
  processUserText(text, true);
  assistantBusy = false;
  setStage(srStarted ? "MultiNet 等待唤醒词" : "MultiNet 未启动");
  JsonDocument res;
  if (!lastError.isEmpty()) res["error"] = lastError;
  res["reply"] = lastReply;
  sendJson(lastError.isEmpty() ? 200 : 502, res);
}

void setupWeb() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/scan", HTTP_GET, handleScan);
  server.on("/api/config", HTTP_GET, handleGetConfig);
  server.on("/api/config", HTTP_POST, handleSaveConfig);
  server.on("/api/reboot", HTTP_POST, handleReboot);
  server.on("/api/chat", HTTP_POST, handleChatTest);
  server.on("/api/audio/volume", HTTP_POST, handleVolume);
  server.on("/api/audio/mic", HTTP_POST, handleMicTest);
  server.on("/api/audio/speaker", HTTP_POST, handleSpeakerTest);
  server.on("/api/model", HTTP_POST, handleModelUploadDone, handleModelUploadStream);
  server.onNotFound([]() { JsonDocument e; e["error"] = "Not found"; sendJson(404, e); });
  server.begin();
}

// --------------------------- Wi-Fi and assistant flow ------------------------
void startNetworking() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  String apName = "Voice-Assistant-" + deviceId();
  bool apOk = WiFi.softAP(apName.c_str(), AP_PASSWORD);
  apActive = apOk;
  Serial.printf("[WiFi] AP %s: %s / %s / %s\n", apName.c_str(), apOk ? "OK" : "FAILED", WiFi.softAPIP().toString().c_str(), AP_PASSWORD);
  if (!cfg.wifiSsid.isEmpty()) {
    WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPassword.c_str());
    wifiConnecting = true;
    wifiAttemptAt = millis();
  }
}

void playWakeBeep() {
  speakerI2S.end();
  speakerI2S.setPins(SPK_BCLK_PIN, SPK_WS_PIN, SPK_DOUT_PIN, -1);
  if (!speakerI2S.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) return;
  int16_t tone[256];
  for (int i = 0; i < 256; ++i) tone[i] = (int16_t)(3500.0f * cfg.speakerVolume / 100.0f * sinf(2.0f * PI * 880.0f * i / 16000.0f));
  for (int n = 0; n < 3; ++n) speakerI2S.write((uint8_t*)tone, sizeof(tone));
  speakerI2S.end();
}

void handleWakeWord() {
  if (assistantBusy || !srStarted) return;
  assistantBusy = true;
  wakePending = false;
  lastError = "";
  setStage("唤醒成功，请说话");
  playWakeBeep();

  uint8_t *wav = nullptr; size_t wavLen = 0;
  if (captureSpeech(&wav, &wavLen)) {
    setStage("语音识别 ASR");
    String recognized = transcribeAudio(wav, wavLen);
    free(wav);
    if (!recognized.isEmpty()) processUserText(recognized, true);
  }

  assistantBusy = false;
  setStage(srStarted ? "MultiNet 等待唤醒词" : "MultiNet 未启动");
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(300);
  Serial.println();
  Serial.println("===== ESP32-S3 Voice Assistant =====");
  Serial.printf("Chip: %s, Flash: %u MB, PSRAM: %u KB\n", ESP.getChipModel(), ESP.getFlashChipSize() / (1024 * 1024), ESP.getPsramSize() / 1024);
  loadConfig();
  Serial.printf("[WakeWord] MultiNet target: %s / %s\n", cfg.wakeWordZh.c_str(), cfg.wakeWordPinyin.c_str());
  startNetworking();
  setupWeb();

  micMutex = xSemaphoreCreateMutex();
  if (micMutex == nullptr) {
    lastError = "麦克风互斥锁创建失败";
    setStage("音频资源初始化失败");
  }

  if (micMutex == nullptr) {
    // Keep network administration online; MultiNet can be retried only after reboot.
  } else if (!beginMic()) {
    lastError = "INMP441 I2S 初始化失败，请检查 GPIO 和供电";
    setStage("麦克风初始化失败");
  } else {
    setStage("加载 ESP-SR 模型");
    beginSpeechEngine();
    setStage(srStarted ? "MultiNet 等待唤醒词" : "MultiNet 未启动：检查 model 分区/模型包");
  }
  Serial.printf("Admin: http://192.168.4.1/ | FreeHeap=%u | FreePSRAM=%u\n", ESP.getFreeHeap(), ESP.getFreePsram());
}

void loop() {
  server.handleClient();
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnecting = false;
    if (lastWifiRetryAt == 0) {
      lastWifiRetryAt = millis();
      Serial.printf("[WiFi] Connected: %s\n", WiFi.localIP().toString().c_str());
    }
    if (!cfg.keepApEnabled && apActive) {
      WiFi.softAPdisconnect(true);
      apActive = false;
      Serial.println("[WiFi] Setup AP disabled by configuration");
    }
  } else {
    if (wifiConnecting && millis() - wifiAttemptAt > 25000) wifiConnecting = false;
    if (!cfg.wifiSsid.isEmpty() && !wifiConnecting && millis() - lastWifiRetryAt > 15000) {
      lastWifiRetryAt = millis();
      WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPassword.c_str());
      wifiConnecting = true; wifiAttemptAt = millis();
    }
  }
  if (wakePending && !assistantBusy) handleWakeWord();
  delay(2);
}
