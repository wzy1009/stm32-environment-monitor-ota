# 验证记录

## V0.3 云端交互（2026-10-24）

- Keil µVision 5 / ARM Compiler 5.06 update 6 build 750，Rebuild `ENV_BASE`：0 Error(s)，0 Warning(s)。
- Code 16172 B、RO-data 1500 B、RW-data 184 B、ZI-data 10016 B。
- 生成 `firmware/MDK-ARM/Objects/ENV_CLOUD.hex`，编译产物不提交。
- 本机 C 测试共 10 组通过：本地监测的 8 组回归，加下面 2 组云端测试。

| 组 | 覆盖内容 |
|---|---|
| 9 | MQTT 编解码、分段输入、变长字段、缓冲区边界；错误主题、未知命令、保留消息、超范围 QoS、拒绝订阅；二进制 IPD 与 AT 文本分离 |
| 10 | 无效配置与指令注入拦截；CONNACK/SUBACK 后才在线；状态/失效数据上报；布防幂等、报警累计、KEY1 优先；定时与主动查询；断线、串口丢字节、截断帧、提示符/SEND OK/PING 超时后的重连；计时回绕 |

测试使用真实 `cloud_mqtt.c`、`cloud_task.c`、`esp_stream.c` 和 `monitor_state.c`，模拟 AT/UART 与云端应答。它们证明这些输入下的软件行为，不代表 ESP8266、真实网络或传感器已联调通过。

待实板验证：P7 跳线、AT 固件兼容性、2.4 GHz Wi-Fi、真实巴法云连接/订阅/控制、断网与复位恢复、DHT11 接收期间的串口中断和屏幕刷新并发。尚无烧录、云端在线截图、实测延迟或长时间运行数据。步骤见 [云端接入](cloud.md)。

## V0.2 本地监测与报警（2026-10-22）

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

## V0.1 基础环境（2026-10-22）

Keil 编译 0 错误、0 警告。Code 4232 B、RO-data 772 B、RW-data 52 B、ZI-data 5732 B。未进行实板烧录验证。
