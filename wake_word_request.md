# 说明：本项目已不再使用 WakeNet 定制申请流程

当前版本改为 ESP-SR MultiNet 6 中文命令词路线，不需要为“你好大肥鱼”单独申请或训练 WakeNet 权重。

- 通用模型：`mn6_cn` + `fst`
- 运行时命令词：`ni hao da fei yu`
- 显示文本：`你好大肥鱼`

固件启动时会通过 `esp_mn_commands_add()` 将拼音命令动态添加到预训练 MultiNet。该机制不是重新训练神经网络。模型包由 `.github/workflows/build-multinet-model.yml` 生成。

请参阅仓库根目录 `README.md` 的手机端 GitHub Actions 构建说明。
