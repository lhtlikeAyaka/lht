# ESP32-S3 独立语音助手（MultiNet 6 自定义命令词版）

本工程以现有 Arduino-ESP32 语音助手为基础，将旧的 WakeNet/ESP_SR 封装调用替换为 ESP-SR MultiNet 6 中文命令词识别。默认目标词为 **“你好大肥鱼”**。

> 这不是重新训练神经网络。MultiNet 6 是预训练的中文命令词模型；固件启动时调用 `esp_mn_commands_add()`，把 `ni hao da fei yu` 动态加入识别词表。中文拼音不写声调数字。小智 AI 的自定义唤醒词机制也是这一路线。

## 目标硬件

- 主控：ESP32-S3-N16R8（16 MB Flash、8 MB OPI PSRAM）
- 麦克风：INMP441，单声道 I2S
- 功放：MAX98357A，I2S
- Arduino-ESP32：3.3.5
- ESP-SR 模型包：脚本默认固定到 ESP-SR `v2.1.5`，使用 `mn6_cn` 和 `fst`

默认引脚（需和实际载板核对）：

| 信号 | GPIO |
|---|---:|
| INMP441 BCLK | 5 |
| INMP441 WS/LRCLK | 4 |
| INMP441 SD | 6 |
| MAX98357A BCLK | 15 |
| MAX98357A LRC | 16 |
| MAX98357A DIN | 7 |

INMP441 的 L/R 接 GND（左声道），供电 3.3 V。MAX98357A 供电 5 V；扬声器只接 SPK+ 与 SPK-，不要把任一喇叭端接 GND。

## 目录

```text
VoiceAssistant/VoiceAssistant.ino       主固件
partitions/voiceassistant_multinet.csv  16 MB 分区表（含 6 MB model 分区）
tools/build_multinet_model.sh           获取 ESP-SR v2.1.5 并打包 mn6_cn + fst
.github/workflows/build-multinet-model.yml  GitHub Actions：生成 srmodels.bin
.github/workflows/build-firmware.yml        GitHub Actions：编译 ESP32-S3 固件
```

## 只用手机在 GitHub 上构建

把本目录中的全部文件上传到一个 GitHub 仓库，保持目录结构不变。可在手机浏览器打开 GitHub 网站操作。

### 1. 生成通用 MultiNet 模型包

1. 打开仓库的 **Actions** 页面。
2. 选择 **Build ESP-SR MultiNet 6 model bundle**。
3. 点击 **Run workflow**，等待任务完成。
4. 打开成功的工作流记录，在 **Artifacts** 中下载 `esp-sr-mn6-cn-model`。
5. 解压后取得 `srmodels.bin`。

该文件包含预训练的 `mn6_cn` 和 `fst`，是通用模型包，不包含“你好大肥鱼”的专属神经网络权重。目标拼音由固件在启动时动态写入 MultiNet 命令表。

### 2. 编译固件

1. 在 **Actions** 中选择 **Build ESP32-S3 voice assistant firmware**。
2. 点击 **Run workflow**。
3. 成功后下载 `voice-assistant-esp32s3-firmware` Artifact。
4. 该构建以 Arduino-ESP32 3.3.5、ESP32-S3、16 MB Flash、OPI PSRAM 和本仓库分区表为目标。

GitHub Actions 的编译结果必须以实际运行状态为准。本仓库交付时尚未在真实 ESP32-S3 板子上完成编译、烧录和麦克风识别测试；若 CI 编译失败，应以 Actions 日志定位问题，不要直接把二进制刷入设备。

## 3. 首次烧录与分区迁移

本工程的分区表与原 `esp_sr_16.csv` 不同：新增/扩大了 `model` 分区，并相应压缩 SPIFFS。

- `app0`：3 MB
- `app1`：3 MB
- `spiffs`：约 3.875 MB
- `model`：6 MB，起始地址 `0x9F0000`
- `coredump`：64 KB

**首次切换到这张分区表，需要完整擦除/重新烧录分区表和固件，不能只做普通 OTA。** 这会清除 NVS、Wi-Fi 配置、API Key 等现有设置，也会清除 SPIFFS 内容。先记录配置。具体烧录偏移和文件名以 Actions Artifact 中生成的实际文件为准；不要将 `srmodels.bin` 当作应用固件烧录。

如果设备此前已经使用了其他分区表，不要在未确认分区表已更新前通过网页上传模型。固件会查找标签严格为 `model` 的数据分区。

## 4. 上传模型和设置唤醒词

1. 设备启动后会建立热点 `Voice-Assistant-xxxxxx`，密码 `voice1234`。
2. 手机连接热点，浏览器打开 `http://192.168.4.1`。
3. 进入“自定义唤醒词”区域，选择上一步得到的 `srmodels.bin`，点击上传。
4. 上传成功后重启设备。
5. 唤醒词配置默认值：
   - 中文：`你好大肥鱼`
   - 拼音：`ni hao da fei yu`
   - 阈值：20
   - 检测窗口：3000 ms
6. 修改拼音、阈值或时长后保存并重启。

拼音应使用小写英文字母、音节间空格分隔、不带声调数字，例如 `ni hao da fei yu`。中文显示文本与拼音字段是分开的，固件不会自动把汉字转换成拼音。

阈值越小通常越灵敏，也可能增加误触发；阈值越大通常更严格，也可能增加漏唤醒。建议先用 20 做基线，再用真实房间环境测试调整。

## 音频链路说明与当前限制

- 本版 INMP441 初始化为 32-bit I2S 槽，再转换为 16-bit PCM。
- MultiNet 前接入单麦 AFE（`AFE_TYPE_VC`，WebRTC NS 开启，AEC 关闭），将 AFE 输出的 16 kHz / 16-bit / mono 音频帧送入 MultiNet；这符合 ESP-SR 文档对 MultiNet 输入的要求。
- 因为只有一个 INMP441 麦克风，没有独立播放参考通道，所以这里不启用 AEC。AEC 不能用 NS 替代。
- 云端 ASR 录音路径仍保留可选的额外 AFE WebRTC NS。
- INMP441 与 MAX98357A 使用不同 I2S 对象/自动分配外设；麦克风读取通过互斥锁与录音路径串行化。
- `model` 分区缺失或为空时，程序不会继续初始化 MultiNet，Wi-Fi AP 和 Web 管理页面仍应保持可用。损坏或版本不兼容的非空模型包仍需要通过日志排查。
- AFE/MultiNet 的线程调度、内存占用、实际唤醒率和误触发率都必须在真实 ESP32-S3 板子上验证。本次未完成 CI 编译和板载测试，不能视为已验证固件。

## 模型版本与技术依据

- Arduino-ESP32 3.3.5 的 ESP Component Registry 依赖 ESP-SR `^2.1.5`；本仓库模型打包脚本 pin 在 `v2.1.5`，降低模型/运行库不一致风险。
- `movemodel.py` 依据 `CONFIG_SR_MN_CN_MULTINET6_QUANT=y` 选择 `mn6_cn`，并在 MultiNet 6 配置下加入 `fst`。
- ESP-SR 文档说明 MultiNet 需要 AFE 处理后的 16 kHz / 16-bit / mono 音频；固件已为唤醒路径建立单麦 AFE。
- ESP-SR Arduino 模型加载文档要求使用 `movemodel.py` 生成 `srmodels.bin`，并写入标签为 `model` 的分区。

参考：

- [XiaoZhi 自定义唤醒词说明](https://xiaozhi.dev/docs/development/assets-customization/)
- [XiaoZhi 自定义唤醒词源码](https://github.com/78/xiaozhi-esp32/blob/main/main/audio/wake_words/custom_wake_word.cc)
- [ESP-SR 官方仓库](https://github.com/espressif/esp-sr)
- [ESP-SR 模型选择和加载](https://docs.espressif.com/projects/esp-sr/zh_CN/latest/esp32/flash_model/README.html)
- [ESP-SR MultiNet 命令词接口](https://docs.espressif.com/projects/esp-sr/zh_CN/latest/esp32/speech_command_recognition/README.html)
- [Arduino-ESP32 I2S API](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/i2s.html)

## 重要安全提示

此固件目前是个人局域网原型。管理页尚未加入网页登录鉴权；代码对 HTTPS 证书校验的处理也不适合直接暴露到公网。不要把设备管理端口映射到公网。上传模型会擦除整个 `model` 分区，过程中不要断电。
