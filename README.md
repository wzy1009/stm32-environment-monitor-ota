# STM32 环境监测与远程升级终端

基于战神 STM32F103ZET6 开发板，计划实现 DHT11 温湿度监测、SR602 人体活动检测、布防报警、云端状态查看和 OTA 远程升级。

## 当前提交：第一阶段 / 开发环境与基础工程

- 从已有 OTA 工程副本提取 HAL、CMSIS、启动代码和 OLED 驱动，整理为独立基础工程。
- 8 MHz 外部晶振，PLL 配置为 72 MHz；LED0 每 500 ms 翻转一次。
- 128×64 OLED 显示 `ENV MONITOR`、`STAGE 1 READY` 和主控型号。
- OLED 初始化或显示失败时，LED1 常亮；LED0 继续运行。
- 当前阶段没有接入 DHT11、SR602、蜂鸣器报警、云端业务或 OTA 安装逻辑；后续分阶段补充。

## 开发环境

| 项目 | 配置 |
|---|---|
| 开发板 | 战神 STM32F1 V1，参考原理图 V3.0；STM32F103ZET6 |
| IDE | Keil µVision 5 |
| 编译器 | ARM Compiler 5.06 update 6，build 750 |
| 设备包 | Keil STM32F1xx_DFP 2.3.0 |
| 依赖 | 工程随附 STM32F1 HAL 和 CMSIS，保留其许可证 |
| Flash / SRAM | 512 KiB / 64 KiB |
| 当前程序地址 | `0x08000000`，独立基础程序，不是 OTA APP 镜像 |

## 编译与验证

1. 安装上述 Keil 编译器和设备包，打开 `firmware/MDK-ARM/ENV_BASE.uvprojx`。
2. 选择 `ENV_BASE`，执行 Rebuild；HEX 输出在 `firmware/MDK-ARM/Objects/`。
3. 实板验证前先备份原固件：本阶段 HEX 从 `0x08000000` 开始，会覆盖原 BootLoader，不能当作 OTA 升级包下发。
4. 使用现有 SWD 或 USART1 下载流程烧录；检查 LED0 闪烁、OLED 启动文字。
5. 本次编译结果见 `docs/validation.md`；实板验证需另行记录，编译成功不代表硬件已测试。

## 目录

```text
firmware/Core/       基础应用、系统时钟、OLED 驱动
firmware/Drivers/    HAL、CMSIS 和各自许可证
firmware/MDK-ARM/    Keil 工程和启动汇编
docs/               引脚、阶段计划、验证记录
```

不上传 IDE 安装包、账号密钥、个人配置和编译缓存。本阶段未配置 Wi-Fi 或云平台凭据。

## 后续阶段

1. 本地监测：DHT11、SR602、OLED 数据页、布防/撤防、蜂鸣器报警。
2. 云端交互：状态上报、远程布防/撤防、报警事件同步。
3. OTA 与联调：接入已有升级链路、版本展示、故障测试和演示材料。

阶段计划不代表功能已实现。提交日期使用 GitHub 实际记录，不回填历史上传时间。
