# STM32 环境监测与远程升级终端

用战神 STM32F103ZET6 开发板做一个环境监测和报警终端：DHT11 采集温湿度，SR602 检测人体活动，OLED 显示状态，ESP8266 连接巴法云。除了本地按键，也可以在云端布防、撤防和下发固件更新。

## 现在做到哪了

当前版本 **V0.4.0**，本地监测、云端控制和 OTA 的代码已接通，应用与启动程序均编译通过。温湿度采集、报警状态机、MQTT、HTTP 下载以及启动程序安装的模拟测试已通过；接线、真实联网和实际升级效果仍待实板验收。

- DHT11 约每 2 秒采样；超时或校验失败时显示错误，云端温湿度上报 `null`。
- SR602 等待 60 秒预热，过滤短于 100 ms 的脉冲。KEY0 切换布防，KEY1 优先撤防；布防后延时 3 秒，报警保持到撤防。
- 巴法云可查看温湿度、活动、布防、报警、累计报警次数和当前固件版本；支持 `ARM`、`DISARM`、`STATUS`。
- OTA 先下载到板载 SPI Flash，检查大小、MD5 和启动向量，确认完整后才提交安装。重启后由启动程序改写应用区，读回校验成功才清除安装标志。
- OLED 显示当前版本、监测状态和升级阶段、进度；下载失败后保留错误显示，可恢复的网络失败会重新连接云端。

公开工程不含 Wi-Fi 密码和云端私钥，默认显示 `NET CONFIG`，本地功能可以运行。联网配置见 [云端接入](docs/cloud.md)。

## 打开哪个工程

只打开 **`firmware/MDK-ARM/ENV_BASE.uvprojx`**，里面有两个用途不同的目标：

| 目标 | 用途 | 起始地址 |
|---|---|---|
| `ENV_APP` | 唯一的监测、云端和 OTA 应用 | `0x08008000` |
| `ENV_BOOT` | 开机检查、升级安装和跳转应用 | `0x08000000` |

两者共用一份驱动和 OTA 数据格式。`firmware/Boot` 只有启动程序代码，没有另一套监测应用，也没有额外的独立基础应用目标。

开发环境：Keil µVision 5、ARM Compiler 5.06 update 6 build 750、STM32F1xx_DFP 2.3.0。仓库包含需要的 HAL、CMSIS 和启动文件。

## 编译、烧录与演示

1. 核对 [硬件接线](docs/hardware.md)，填写本地联网配置。
2. 在 Keil 分别 Rebuild `ENV_BOOT`、`ENV_APP`，或执行 `./tools/build.ps1 -KeilRoot <Keil安装目录>`；使用本地联网配置时追加 `-PrivateConfig`。
3. 首次烧录需要 **Boot 和 APP 两部分**，可以按 [OTA 说明](docs/ota.md) 合成一份 `ENV_INITIAL.hex`。先备份板上要保留的程序与数据，首次烧录会覆盖对应 Flash 分区。
4. 一键生成两份演示版本可运行 `./tools/build_demo.ps1 -KeilRoot <Keil安装目录> -PrivateConfig`；通知 URL 需要换成实际地址。
5. 按 [演示清单](docs/demo.md) 检查本地采集、报警、云端控制、固件升级和失败重试。

OTA 下发的是 `ENV_APP.bin`，不能下发 HEX 或 Boot BIN。代码使用明文 MQTT/HTTP 和 MD5 一致性校验，尚未实现签名验证、A/B 回滚或首次启动健康确认。

## 目录

```text
firmware/
├── Core/       监测、云端、OTA 和共用外设驱动
├── Boot/       启动程序与内部 Flash 安装
├── Drivers/    HAL、CMSIS 及许可证
└── MDK-ARM/    唯一 Keil 工程入口和启动文件
docs/           接线、配置、升级、演示与验证记录
tests/          生产代码的本机模拟测试
tools/          构建、初始 HEX 合并和升级通知生成
```

SR602 检测的是人体活动，不能可靠判断静止人员是否还在。功能完成度和验证边界见 [验证记录](docs/validation.md)，实际提交日期以 GitHub 记录为准。
