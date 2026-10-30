# ESP32-S3 独立语音助手（ESP-IDF + Arduino-ESP32 + ESP-SR MultiNet 6）

目标硬件：ESP32-S3-N16R8（16 MB Flash、8 MB OPI PSRAM）、INMP441 麦克风、MAX98357A 功放。

默认唤醒短语为 **“你好大肥鱼”**，拼音 `ni hao da fei yu`。工程使用预训练的 ESP-SR MultiNet 6 中文模型 `mn6_cn` 和 FST；固件通过 `esp_mn_commands_add()` 将后台输入的拼音动态加入命令表。**这不是训练一个新的神经网络，也不是生成专属唤醒词权重。**

## 目录要求

请将 ZIP 内的所有文件按原目录结构放到 GitHub 仓库根目录，尤其不要漏掉 `main/`：

- `main/main.cpp`：定义 `app_main()`，初始化 Arduino 并启动语音助手任务。
- `main/CMakeLists.txt`：把 Arduino、ESP-SR、ArduinoJson 组件链接到应用。
- `main/idf_component.yml`：固定 Arduino-ESP32 3.3.5、ESP-SR 2.1.5、ArduinoJson 7.4.2。
- `VoiceAssistant/VoiceAssistant.ino`：完整语音助手业务代码及 Web 后台。
- `tools/build_multinet_model.sh`：独立生成通用 `srmodels.bin` 的脚本。
- `.github/workflows/build-multinet-model.yml`：独立模型包构建流程。
- `.github/workflows/build-firmware.yml`：固件和模型包一体化构建流程。

## 两个 Actions 工作流均保留

### 1. 独立生成 MultiNet 模型包

在仓库 Actions 里运行 **Build ESP-SR MultiNet 6 model bundle**。成功后下载 Artifact `esp-sr-mn6-cn-model`，其中有 `srmodels.bin` 和 `model-info.txt`。脚本固定到 Component Registry ESP-SR 2.1.5 对应的官方源码提交，而不是不存在的 `v2.1.5` Git 标签。

### 2. 编译固件

运行 **Build ESP32-S3 voice assistant firmware**。ESP-SR 的 IDF 构建步骤会按自定义分区表生成 `build/srmodels/srmodels.bin`，并连同固件一起输出。

构建环境固定为 ESP-IDF 5.5.1、Arduino-ESP32 3.3.5、ESP-SR 2.1.5。`main.cpp` 使用显式 `app_main()`：先调用 `initArduino()`，再在 8 KB FreeRTOS 任务中运行原有 `setup()` / `loop()`。因此 `CONFIG_AUTOSTART_ARDUINO` 关闭，避免依赖 Arduino 组件自动提供另一个 `app_main()`。

## 唤醒词设置

默认：
- 中文显示：`你好大肥鱼`
- MultiNet 拼音：`ni hao da fei yu`
- 阈值：20
- 检测窗口：3000 ms

可以在设备 Web 管理页面修改中文显示词、拼音、阈值和检测窗口。拼音使用小写英文字母、音节间空格分隔、不带声调数字；固件不会自动把汉字转拼音。更改唤醒词配置后需要重启。

## 分区及烧录注意事项

- app0：3 MiB，偏移 `0x010000`
- app1：3 MiB，偏移 `0x310000`
- SPIFFS：约 3.875 MiB
- model：6 MiB，偏移 `0x9F0000`
- partition table：偏移 `0x008000`
- bootloader：偏移 `0x000000`

首次改用本分区表必须完整重刷 bootloader、partition table 和 app；这会清除 NVS 与现有设置。模型包应写入 `model` 分区，**不要把 `srmodels.bin` 当作 app 固件刷入**。只在 Actions 构建成功且 Artifact 文件齐全后才开始烧录。

## 当前验证边界

已核对 ESP-SR 2.1.5 的头文件布局及模型选择配置，并修正构建入口缺少 `app_main()` 的问题。此 ZIP 尚未在你的 GitHub Actions 环境实际运行，也没有完成真实硬件烧录、麦克风识别率或长时间稳定性测试；以新一轮 Actions 的实际结果为准。

Web 管理页面当前未加入登录鉴权，固件 HTTPS 客户端的证书验证也不是生产级安全配置。不要将管理端口映射到公网。
