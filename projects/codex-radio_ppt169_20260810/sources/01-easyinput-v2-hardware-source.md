# EasyInput V2.0 硬件资料摘要

来源：https://ultraway.feishu.cn/wiki/CTSqwgdYFiP6ZqkAfaTcOWKCnUb

文档标题：《EasyInput V2.0 开发板介绍与硬件信息》

## 产品定位

EasyInput V2.0 是 WaytoAGI AI 硬件课程的统一实践开发板。出厂时已完成组装和固件写入，可以先当作 Vibe Coding AI 键盘使用；键盘是已经完成的使用形态，开发板才是本质。学习者可继续把它改成桌面控制器、状态提示器、语音交互设备或其他硬件形态。

课程对“AI 硬件”的解释不是把 Codex 等大模型运行在 ESP32 中，而是用 AI 工具协助开发硬件，并让开发板与电脑侧 AI 应用协同。

## 开发板、固件与 App

- 开发板负责感知按键和旋钮、采集声音，并通过灯光或电脑操作给出反馈。
- 固件决定键和旋钮怎样工作，以及数据怎样传输。
- USB/BLE 用于键盘输入、状态读取和配置同步。
- Wi-Fi 主要用于传输开发板麦克风采集的音频。
- EasyInput App 完成语音转文字、AI 处理和可视化配置。

## 公开硬件参数

- 主控：ESP32-S3R8
- PSRAM：8 MB，集成在主控芯片封装内
- Flash：16 MB 外置 W25Q128 系列
- 输入：8 个机械键，1 个支持旋转和按压的旋钮
- 灯光：5 颗 WS2812 RGB 灯，以及独立状态/充电灯
- 音频输入：I2S 数字麦克风
- 音频输出：数字功放与扬声器接口
- 连接：USB-C、BLE、2.4 GHz Wi-Fi
- 调试：BOOT 恢复入口、UART 调试触点

## 与 Codex 任务电台的关系

原始文档把 8 MB PSRAM 与扬声器定义为硬件已具备、出厂固件尚未完整使用的能力。Codex 任务电台是课程自定义固件和 Mac App 案例：启用 PSRAM 音频 buffer、Wi-Fi 语音上行、I2S 扬声器播放和四槽信箱灯，不应与出厂固件的默认能力混为一谈。

## 配图

- `easyinput-v2-assembled-front.png`：组装完成的开发板正面部件标注
- `easyinput-default-keymap.png`：出厂默认键位与旋钮功能
- `easyinput-v2-pcb-front.png`：PCB 正面结构
- `easyinput-v2-pcb-back.png`：PCB 背面核心电路和接口
- `easyinput-v2-schematic-1.png`：供电、连接与实体交互原理图
- `easyinput-v2-schematic-2.png`：主控、存储与音频链路原理图

