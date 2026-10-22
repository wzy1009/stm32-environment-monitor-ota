# 第一阶段验证记录

实际验证日期：2026-10-22（北京时间）。

- Keil µVision：Rebuild `ENV_BASE`，ARM Compiler 5.06 update 6 build 750。
- 结果：0 Error(s)，0 Warning(s)。
- 程序体积：Code 4232 B、RO-data 772 B、RW-data 52 B、ZI-data 5732 B。
- 已生成 `firmware/MDK-ARM/Objects/ENV_BASE.hex`；生成物按 `.gitignore` 排除，不提交。
- 工程源文件路径完整性检查通过；HAL/CMSIS 许可证保留。
- 上传清单排除账号配置、编译缓存、安装包和后续 OTA 业务源文件。
- 原 OTA 工程与比赛副本文件内容一致性检查通过，未修改原工程。
- 尚未连接实板烧录，LED、OLED 和外部晶振行为均待实板验证。

本阶段是从 Flash 起始地址运行的独立基础工程；烧录会覆盖该位置已有固件，不能作为现有 BootLoader 的 OTA APP 升级包。
