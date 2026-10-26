# 本机测试

使用 Python 3 和 Windows Tiny C Compiler，在仓库根目录运行：

```powershell
python tests/run_tests.py --tcc <tcc.exe的路径>
```

| 文件 | 检查内容 |
|---|---|
| `test_local_monitor.c` | DHT11 数据与虚拟波形、断线恢复、按键消抖、上电立即就绪、布防与报警 |
| `test_cloud.c` | MQTT/IPD、非法配置、远程命令、KEY1 优先、超时重连和 OTA 接收权限与网络交接、Wi-Fi 与订阅失败的独立提示、复位期串口噪声容忍和正常通信期错误中止 |
| `test_ota.c` | 通知与 HTTP 校验、正确下载、错误 MD5/长度/向量、旧 pending、EEPROM 异常、提交结果不明确、计时回绕、布防取消和串口丢字节 |
| `test_boot.c` | 运行真实安装器和 Flash 边界检查；正常安装、奇数尾字节、坏暂存/记录/向量、擦写失败、读回损坏、清 pending 失败和重试 |
| `test_package.py` | BIN 版本/向量/URL、初始 HEX 校验与合并、错误地址和校验和拒绝 |

`stubs` 和 `ota_stubs` 只替代外设边界、AT 与服务器应答，测试使用固定虚假凭据，不加入 Keil 固件。Boot 测试用 Windows 虚拟内存模拟内部 Flash，保留实际地址边界检查；它不是实板擦写或真实断电试验。

测试临时文件位于被 Git 忽略的 `build` 下。运行器会给本机编译副本移除 UTF-8 BOM；ARMCC 使用的源文件编码保持原样。只重跑升级部分可加 `--ota-only`。

软件验证不能替代供电、接线、外设时序、真实网络和安装断电验收，具体范围见 [验证记录](../docs/validation.md)。
