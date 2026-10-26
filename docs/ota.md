# OTA 使用说明

## 工程与分区

同一个 `firmware/MDK-ARM/ENV_BASE.uvprojx` 中，`ENV_APP` 编译应用，`ENV_BOOT` 编译启动程序。源码与外设驱动共用；两部分必须配套首次烧录。

| 位置 | 分配 |
|---|---|
| 内部 Flash `0x08000000–0x080077FF` | 启动程序代码 |
| 内部 Flash `0x08007800` 所在的最后 2 KiB Boot 页 | 一次性初始化标记；OTA 不擦除 |
| 内部 Flash `0x08008000–0x0807FFFF` | 应用区，最大 480 KiB |
| 外部 SPI Flash `0x00000000` 起 | 待安装 BIN，最大 480 KiB |
| AT24C02 `0x00–0x3F` | 64 字节升级记录、CRC 和 pending 标志 |

分区定义只有一份：`firmware/Core/Inc/ota_layout.h`。APP 定义 `ENV_OTA_APP`，启动时将中断向量表重定位到应用区。Boot 则留在 Flash 首地址。

## 编译与首次烧录

在 Keil 分别 Rebuild 两个目标，或运行：

```powershell
./tools/build.ps1 -KeilRoot <Keil安装目录>
python tools/firmware_package.py merge --boot build/ENV_BOOT.hex --app build/ENV_APP.hex --out build/ENV_INITIAL.hex
```

需要自动生成初始版本和补丁版本两份演示文件时，可运行：

```powershell
./tools/build_demo.ps1 -KeilRoot <Keil安装目录> -Python python -PrivateConfig
```

`-PrivateConfig` 读取本地被忽略的 `cloud_private.h`，只在编译期间启用私有配置，并恢复公开工程文件；不使用这个开关时构建空联网配置版本。演示构建会生成 `build/versions/0.4.0`、`build/versions/0.4.1`、`ENV_INITIAL.hex` 和通知示例，并恢复源文件中的初始版本。私有构建的 BIN/HEX 含联网参数，仅在本地使用，不提交仓库。通知中的 `firmware.local` 仍须换成实际可访问的地址。

`build` 中的 `ENV_BOOT.hex`、`ENV_APP.hex` 和合并后的 `ENV_INITIAL.hex` 用于首次烧录；`ENV_APP.bin` 才是远程升级文件。合并工具会检查 HEX 校验和、分区、向量及初始化标记，不会写板。

先备份板上需要保留的程序与数据。首次烧录可以使用联合 HEX，避免分两次烧录时下载器的整片擦除把另一部分擦掉。没有 Boot 时，单独烧 APP 不能从复位入口正常启动。

第一次启动时，只有完整的一次性初始化标记存在，才允许为空或无效的 EEPROM 建立初始记录；随后关闭这个入口。已有 pending 任务、EEPROM 读取失败或不完整初始化标记配合无效记录时停机，避免覆盖状态。常规启动不会把损坏记录当作新板清空。重新烧 Boot 会重新引入初始化标记，不作为日常恢复步骤。

## 准备升级包

1. 将 `firmware/Core/Inc/ota_app.h` 中的 `FIRMWARE_VERSION` 从 `0.4.0` 改成下一个版本，例如 `0.4.1`。
2. 重新构建 `ENV_APP`，保存这个版本的实际 BIN，不改 BIN 内容。
3. 把 BIN 放到设备可访问的 HTTP 服务器。演示时可使用同一路由器下的电脑：

```powershell
python -m http.server 8080 --directory build
```

设备下载 URL 应使用电脑在该网络中的实际 IP，不能写 `127.0.0.1`。服务器需要直接返回 HTTP 200、正确的 Content-Length 和原始 BIN；本版不支持 HTTPS、重定向、chunked 或压缩正文。

4. 按实际 BIN 生成通知，示例中的地址必须换成自己的可访问地址：

```powershell
python tools/firmware_package.py notice --bin build/ENV_APP.bin --version 0.4.1 --url http://192.168.1.2:8080/ENV_APP.bin --out build/ota-notice.json
```

工具读取真实字节数与 MD5，并检查向量、版本文本、URL 和消息长度。通知版本需要与你构建的版本一致；这项检查不等同于签名认证。

## 下发与观察

通过巴法云向 `envCmd/set` 发非保留消息，消息体是生成的 JSON，例如：

```text
{"cmd":"ota","version":"0.4.1","url":"http://实际服务器/ENV_APP.bin","size":实际字节数,"md5":"实际MD5"}
```

等待设备撤防后开始下载。重复通知、相同版本、错误主题、保留消息以及不符合字段格式的通知不会开始新的升级。下载过程中按 KEY0 重新布防会取消尚未提交的升级。

OLED 依次显示准备、擦除暂存、下载、校验、提交；提交后复位，由 Boot 显示擦除、写入、校验和完成，再启动 APP。正常云端 MQTT 在下载期间暂停，以免与 HTTP 争抢 ESP8266；成功重启并联网后，`envState` 中的 `version` 应为新版本。

联网 AT 固件需要同时支持 `_CUR` Wi-Fi 指令与 `AT+CIPRECVMODE`、`AT+CIPRECVLEN`、`AT+CIPRECVDATA`。下载以被动模式每次最多取 1024 字节，SPI Flash 每页最多写 256 字节。协议参考：[Espressif ESP8266 AT 指令集](https://www.espressif.com/sites/default/files/documentation/4a-esp8266_at_instruction_set_en.pdf)。当前还没有本板模块版本的实测兼容性记录。

## 失败与恢复

- 下载或校验失败且 EEPROM 明确无待安装任务时，应用不改写内部 Flash；恢复云端后可重新发送通知。
- 提交点是最后单独写入的 `0xA5` 标志。提交结果不明确或 EEPROM 异常时保留暂存数据并锁定自动重试。
- Boot 安装前再次校验暂存文件，之后只擦应用页；内部读回的 MD5、向量验证通过后才清除 pending。
- 安装中断后，只要暂存数据和升级记录仍有效，重启可重新安装。模拟测试覆盖了中途写入失败后重试；真实断电恢复仍待实板验证。

这是一份应用区的重新安装方案，不是 A/B，也没有旧版本回滚、版本可信防降级或新应用健康确认。故障停机、EEPROM 或暂存损坏时，需要检查日志或恢复配套初始固件；不要随意清 pending 后强行启动未写完整的应用。
