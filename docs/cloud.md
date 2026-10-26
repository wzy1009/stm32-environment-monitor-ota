# 云端接入与联调

本版通过 ESP8266 的 TCP 连接发送 MQTT 3.1.1 报文，服务器为 `bemfa.com:9501`。这是明文 TCP 演示接入，未实现 TLS。默认不含联网凭据，屏幕显示 `NET CONFIG`，本地监测仍可运行。

## 配置

1. 在巴法云 MQTT 控制台新建 `envState` 和 `envCmd` 两个主题。
2. 把 `firmware/Core/Inc/cloud_private.h.example` 复制成同目录的 `cloud_private.h`。
3. 在本地填写 `CLOUD_WIFI_SSID`、`CLOUD_WIFI_PASSWORD`、32 位十六进制用户私钥 `CLOUD_BEMFA_UID`。私钥作为 MQTT Client ID；主题名称可改，但两个主题必须不同，且与控制台一致。本版主题名限 1–40 个字母、数字或下划线。
4. 使用 `./tools/build.ps1 -KeilRoot <Keil安装目录> -PrivateConfig` 构建，脚本只在编译期间启用 `CLOUD_USE_PRIVATE_CONFIG=1`，之后恢复公开工程。也可在 Keil 的 C/C++ Define 中仅为 `ENV_APP` 加此宏；分享工程时恢复默认配置，不把密码写进工程文件。
5. 按 [硬件说明](hardware.md) 连接 ESP8266/P7 跳线，使用 115200、8N1 和兼容 `_CUR` 指令的 AT 固件，连接 2.4 GHz Wi-Fi。Rebuild 后烧录独立固件 `ENV_APP.hex`。

`cloud_private.h` 已加入 Git 忽略规则，不能提交个人配置。SSID 最多 32 字节，密码最多 64 字节；本版拒绝双引号、反斜杠和控制字符，避免拼接 AT 指令时破坏命令边界。缺失或非法配置会停在 `NET CONFIG`。

当前 APP 起始地址为 `0x08008000`，首次烧录须配套 Boot；OTA 只下发原始 APP BIN。见 [OTA 说明](ota.md)。

## 看状态

模块复位后依次配置 Wi-Fi、连接 TCP、等待 MQTT CONNACK 和订阅 SUBACK。只有订阅成功后才显示 `NET ONLINE`，连接过程中分别显示 `ESP CHECK`、`WIFI JOIN`、`SERVER CONNECT`、`MQTT LOGIN`、`MQTT SUBSCRIBE`。失败后保留对应阶段提示再重试。

设备向 `envState/up` 发布 JSON，只更新巴法云中 `envState` 的最新值，不向订阅者转发。需要其他客户端实时接收时，应另行调整发布主题；当前版本以控制台查看为准。

连接后上报一次，之后每 10 秒上报；状态变化和 `STATUS` 查询触发额外上报，两次状态上报最少间隔约 1 秒。示例仅说明格式：

```json
{"temp_x10":254,"humi_x10":602,"valid":1,"dht":1,"ready":1,"motion":0,"armed":0,"alarm":0,"alarm_count":0,"last_alarm_ms":0,"uptime_s":75}
```

| 字段 | 含义 |
|---|---|
| `temp_x10` / `humi_x10` | 温度、湿度乘 10；示例为 25.4 ℃、60.2% RH；采集失效时均为 `null` |
| `valid` / `dht` | 本次采集是否有效；DHT 状态：0 等待、1 成功、2 超时、3 校验错误、4 数据错误 |
| `ready` / `motion` | PIR 是否启用 / 过滤后是否检测到活动（当前取消额外预热等待，ready 为 1） |
| `armed` / `alarm` | 是否请求布防 / 是否已触发并保持报警；布防后仍有 3 秒延时 |
| `alarm_count` | 本次开机以来报警由 0 变 1 的次数；撤防、重连不清零，MCU 复位清零 |
| `last_alarm_ms` | 最近一次触发报警时的开机毫秒计数；无报警时为 0，约 49.7 天回绕 |
| `uptime_s` | 当前毫秒计数换算的秒数，随毫秒计数回绕 |

云端保留的最后一条 JSON 不表示设备此刻仍在线。本版没有遗嘱离线消息，查看时需结合消息接收时间；`uptime_s` 可辅助识别上报是否刷新。

## 远程控制

通过巴法云的消息发送入口，向 `envCmd/set` 发送以下大写纯文本，使用非保留消息；设备订阅的是 `envCmd`。主题的 `/set` 转发规则见下方官方文档。

| 消息 | 行为 |
|---|---|
| `ARM` | 布防；首次布防延时 3 秒；重复发送不会重置延时或清除已经触发的报警 |
| `DISARM` | 撤防，同时清除蜂鸣器与报警保持 |
| `STATUS` | 请求更新状态，仍受上报最小间隔约束 |

只接收完整匹配的命令，不接受额外空格、换行、JSON 或 `on/off`。错误主题、未知命令和保留消息不执行。订阅 QoS 为 0。KEY1 本地撤防始终优先；同一批接收中同时出现布防和撤防时，撤防优先。

JSON 还包含 `version`（当前运行固件版本）和 `ota`（本次应用运行中最近的升级阶段：0 空闲、1 准备、2 擦除暂存、3 下载、4 校验、5 提交、6 失败）。重启后阶段归零，以版本和实际运行情况确认升级结果；本版没有首次启动健康确认。

OTA 下载独占 ESP8266，期间暂停 MQTT；失败且允许恢复时重连，成功安装后由新应用重新联网。升级通知也走 `envCmd/set`，格式见 [OTA 说明](ota.md)。

## 掉线与报警

每 30 秒发送 PING，等待响应最多 10 秒；AT 提示符和 SEND OK 等待最多 5 秒，MQTT 连接/订阅与不完整数据帧等待最多 10 秒。Wi-Fi 加入最多等 30 秒。异常后按 2、4、8、16、30 秒间隔重试，成功后恢复初始间隔。Wi-Fi/TCP 重建前会复位 ESP8266，MCU 和本地布防状态不复位。

断网期间本地采集、按键和报警继续运行；重连后提交最新状态、累计报警次数和最近触发计时。多次变化可能合并，不提供每条事件的持久存储、离线历史补传或手机推送；QoS 0 也不保证单条报文送达。

## 实板检查

1. 未填写配置时，确认 `NET CONFIG`，本地温湿度与 KEY0/KEY1 正常。
2. 填好配置并重编译，确认 `NET ONLINE` 和 `envState` JSON 定时刷新。
3. 发 `ARM`，等待 3 秒布防延时结束，走动触发报警；查看 `alarm` 和 `alarm_count`。
4. 发 `DISARM`，确认蜂鸣器停止、云端状态更新；按住 KEY1 再发 `ARM`，应保持撤防。
5. 断开路由器，确认本地仍可报警和撤防；恢复后检查自动重连和最新状态。
6. 同时持续采集、刷新屏幕并远程控制，检查 DHT 超时/校验失败、串口丢字节和重连情况。UART 接收中断保持开启，实际时序须实测。

以上为待执行的实板步骤；当前通过的是编译和本机模拟测试。

协议依据：[巴法云 MQTT 官方文档](https://cloud.bemfa.com/docs/src/mqtt.html)（MQTT 3.1.1、Client ID 认证、9501/9503 与 `/set`、`/up` 规则）。

## 连接失败看哪里

| 屏幕提示 | 排查方向 |
|---|---|
| `NET CONFIG` | 联网配置缺失、无效，或编译时未启用本地配置 |
| `ESP NO REPLY` / `ESP TX FAIL` | 模块供电、P7 跳线与 115200 串口收发 |
| `ESP RX FRAME` / `ESP RX NOISE` / `ESP RX PARITY` | 正常 AT 通信阶段的帧、噪声或校验错误，核对波特率、信号和串口配置 |
| `ESP RX OVERRUN` / `ESP RX FULL` | 硬件接收溢出 / 软件接收队列满 |
| `ESP RX REARM` | HAL 中断接收重新启动失败 |
| `ESP SETUP FAIL` | AT 固件是否支持当前配置指令 |
| `WIFI JOIN FAIL` | 热点是否开启、名称密码、2.4 GHz 与信号 |
| `SERVER FAIL` | 已进入服务器连接阶段，检查外网、DNS、端口可达性 |
| `MQTT LOGIN FAIL` | MQTT Client ID/私钥与认证阶段 |
| `MQTT SUB FAIL` | 主题是否在同一账号的 MQTT 控制台中实际创建，订阅是否获准 |

`NET OFFLINE` 不等同于 Wi-Fi 未关联。配置主题名必须与云端已有主题一致：已使用过巴法云的设备优先沿用原主题；`envCmd`、`envState` 只是模板示例，不能假定它们已在账户中存在。不要通过公开聊天或仓库粘贴 Wi-Fi 密码、私钥。

ESP8266 复位时的 ROM 启动日志可使用 74880 波特，随后 AT 固件使用 115200。程序在明确的 2 秒复位等待窗口内持续恢复接收、丢弃启动数据；开始 AT 握手前清空解析状态并重新记录错误计数基线。正常通信和 OTA 下载仍把新增丢字节或串口错误作为失败，不会跳过固件完整性检查。参考：[Espressif 启动日志说明](https://docs.espressif.com/projects/esptool/en/latest/esp8266/advanced-topics/boot-mode-selection.html)。

MQTT 使用持久会话，与原工程保持一致。登录确认允许新会话或已有会话两种 Session Present 标志，但只接受返回码 0；保留位和拒绝码仍判为失败。每次登录后重新订阅，订阅确认前不执行远程控制，保留命令仍被忽略。建立 MQTT TCP 前显式设置 `AT+CIPDINFO=0`、`AT+CIPRECVMODE=0`，让 ESP 的主动接收格式与 MCU 拆帧器一致。
