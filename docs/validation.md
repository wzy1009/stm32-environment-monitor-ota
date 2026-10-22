# 验证记录

验证日期：2026-10-22（北京时间）。

## V0.2 本地监测与报警

- Keil µVision 5，ARM Compiler 5.06 update 6 build 750，Rebuild `ENV_BASE`。
- 结果：0 Error(s)，0 Warning(s)。
- 程序体积：Code 7180 B、RO-data 856 B、RW-data 76 B、ZI-data 6756 B。
- 输出 `firmware/MDK-ARM/Objects/ENV_LOCAL.hex`，编译生成物不提交。
- 原理图核对 DHT11 PG11、蜂鸣器 PB8、KEY0 PE4、KEY1 PE3；SR602 选择扩展 PG10。
- 本机 C 测试直接运行生产代码 `dht11.c` 和 `monitor_state.c`，8 组通过。

| 组 | 覆盖内容 |
|---|---|
| 1 | 数据帧、小数、负温、校验失败、非法字段 |
| 2 | 虚拟应答与 40 位数据、采样间隔、启动时长、DWT 计数回绕 |
| 3 | 总线卡高/卡低、数据失效、恢复、启动低电平超时释放 |
| 4 | 默认撤防、预热期间禁止报警 |
| 5 | 按键抖动、长按、按住按键复位 |
| 6 | 布防延时、短脉冲过滤、报警保持与撤防 |
| 7 | KEY1 优先、双键同按、按住 KEY1 时禁止布防 |
| 8 | 毫秒计数回绕时的预热、布防延时与输入过滤 |

传感器数据与波形均为程序模拟，不是实测。源码与复现方法见 [tests](../tests/README.md)。

### 待实板验证

OLED 显示、晶振、传感器接线与时序、SR602 预热和触发范围、按键与蜂鸣器联动，以及传感器断开后的表现，均待实板验证。步骤见 [操作说明](local-monitor.md)。云端与 OTA 不属于本次验证范围。

## V0.1 基础环境

Keil 编译 0 错误、0 警告。Code 4232 B、RO-data 772 B、RW-data 52 B、ZI-data 5732 B。未进行实板烧录验证。
