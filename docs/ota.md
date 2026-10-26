# OTA 接入说明

源码已接入下载、提交、Boot安装及云任务恢复路径；本文描述代码行为，整机升级和故障恢复仍待实板验证。

## 构建与首次烧录

工具链沿用 Keil MDK、ARM Compiler 5.06 update 6（build 750）、STM32F1xx DFP 2.3.0，设备 STM32F103ZE。

| 工程 / Target | 用途 | 产物目录 |
|---|---|---|
| `firmware/MDK-ARM/ENV_BASE.uvprojx` / `ENV_OTA` | 配合Boot运行的APP，链接起点 `0x08008000`，定义 `ENV_OTA_APP` | `firmware/MDK-ARM/Objects/ENV_OTA.*` |
| 同工程 / `ENV_BASE` | 独立基础APP，起点 `0x08000000`，不启动OTA下载 | `firmware/MDK-ARM/Objects/ENV_BASE.*` |
| `bootloader/MDK-ARM/OTA_BOOT.uvprojx` / `OTA_BOOT` | 安装器与APP跳转 | `bootloader/MDK-ARM/OTA_BOOT/OTA_BOOT.*` |

在 Keil 选择对应 Target 后 Rebuild。Boot使用同目录 `boot_layout.sct`，驱动引用仓库 `firmware/Drivers`。也可运行 `./tools/build.ps1 -KeilRoot E:\keil5`，依次检查三个目标并导出 `build/ENV_OTA.bin` 与 `build/OTA_BOOT.bin`；构建日志位于 `build/`。HEX用于首次烧录，远程下发的是 `ENV_OTA` 的原始BIN，不能下发HEX或 `ENV_BASE`。

联网配置按 [云端说明](cloud.md) 在本地填写 `cloud_private.h`，在实际构建的APP Target追加 `CLOUD_USE_PRIVATE_CONFIG=1`。不要提交私有配置或包含凭据的产物。

首次部署须备份原固件，烧入Boot和 `ENV_OTA` HEX，并确认下载器不会整片擦除另一分区。不要再烧入起点为零的 `ENV_BASE`，否则会覆盖Boot。

Boot镜像在 `0x08007800` 放置8字节一次性初始化请求。只有标记完整时，Boot才允许为无效/空EEPROM建立初始记录；还会检查APP向量。初始化完成后将该标记写零，关闭入口。标记关闭后记录损坏不会自动作为新板重建；重新烧写Boot可能重新打开初始化入口，不能当作常规恢复操作。`first-setup` 是记录占位版本，实际运行版本由APP `FIRMWARE_VERSION` 上报。

## 存储分区

| 存储 | 范围 / 大小 | 用途 |
|---|---|---|
| 内部Flash | `0x08000000–0x08007FFF`，32 KiB | Boot；代码区止于 `0x080077FF`，最后2 KiB页保留初始化标记 |
| 内部Flash | `0x08008000–0x0807FFFF`，最大480 KiB | 单一APP安装区 |
| SRAM | `0x20000000` 起，64 KiB | 向量与栈地址检查边界 |
| W25Q128 | `0x00000000` 起，最大480 KiB | 待安装BIN暂存 |
| AT24C02 | `0x00` 起，64字节 | 元数据、CRC及pending标志 |

APP/Boot的 `ota_layout.h` 必须一致。W25Q128用SPI2和独立片选，AT24C02用I2C1；接线按 [硬件说明](hardware.md) 核实。

## 下发通知

设备继续订阅 `envCmd`，无需另建OTA主题。通过巴法云发送入口向 `envCmd/set` 发非保留消息，内容为五字段JSON。以下是模板，必须替换版本、URL、字节数及MD5后再发送：

```json
{"cmd":"ota","version":"目标版本","url":"http://固件服务器/ENV_OTA.bin","size":实际BIN字节数,"md5":"实际BIN的32位十六进制MD5"}
```

`size` 是JSON整数，`md5`来自实际发布的同一BIN；目标版本须与当前运行版本不同。通知只接受规定字段，拒绝重复字段、格式错误、相同版本和冲突请求。URL仅支持HTTP。服务器须直接返回HTTP 200及匹配的Content-Length；不支持重定向、chunked或压缩正文。

修改 `firmware/Core/Inc/ota_app.h` 的 `FIRMWARE_VERSION` 后重新构建，通知版本必须与该BIN一致。用下面的命令核对BIN向量、尺寸、URL和通知长度，并生成实际大小及MD5（仅写本地文件，不发送）：

```powershell
python tools/prepare_ota.py --bin build/ENV_OTA.bin --version 0.4.1 --url http://你的服务器/ENV_OTA.bin --out build/ota-notice.json
```

命令中的版本与URL必须替换为本次真实构建和可访问地址；设备不检查通知版本是否与BIN内文本一致。JSON UTF-8载荷最大360字节，包含主题和协议的MQTT包容量为448字节。

## 运行与失败边界

1. MQTT解析器交出完整通知，APP保存请求；接收通知不会立即安装。
2. 设备撤防且DHT11采集空闲时才尝试启动；MQTT还须在线、无半包且ESP数据流空闲。
3. 云任务进入 `OTA_OWNED`，停止正常MQTT收发，下载器独占AT应答与接收回调，关闭旧TCP连接并切换被动接收。
4. 检查存储和未提交元数据，擦除暂存区，分块HTTP下载至W25Q128；比较串口故障计数，验证长度、MD5和向量。
5. 写入并回读EEPROM元数据，最后提交 `0xA5` pending；APP复位，Boot重新验证暂存镜像，再擦写内部APP、校验并清除pending后跳转。

可恢复下载失败须确认尚未进入提交阶段，EEPROM记录有效且pending为零；主循环再次确认后释放回调、清通知，调用云恢复路径重新连接。提交阶段失败、元数据异常或pending非零不自动当作普通网络失败重试。屏幕提供OTA阶段/进度，串口提供诊断；这些路径的时序和恢复效果均待实板验证。

当前使用明文MQTT、HTTP与MD5。MD5用于一致性校验，不是固件签名或发布者认证；版本仅拒绝相等，没有实现可信防降级。内部只有一个APP区，没有A/B切换、旧固件回滚或首次启动健康确认。安装断电可依赖保留的暂存和pending再次尝试，不能据此声称断电恢复已验证。`OTA_INSTALL_POWER_CUT_TEST` 默认为0，正式演示前检查该宏。
