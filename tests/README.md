# 本机回归

需要Python 3和Windows Tiny C Compiler。在仓库根目录运行：

```powershell
python tests/run_tests.py --tcc E:\你的工具路径\tcc.exe
```

脚本临时目录位于仓库内部，结束后自动清理；对编译副本去除UTF-8 BOM以兼容TinyCC，不修改生产文件。

| 套件 | 生产代码与覆盖 |
|---|---|
| 本地监测 | 原8组DHT11虚拟波形、校验与异常、按键/预热/布防/报警状态机和计时回绕 |
| 云端交互 | 原2组MQTT/IPD与云生命周期回归；增加最大OTA载荷、错误主题/保留消息拦截、OTA通知交接、独占暂停与恢复 |
| OTA | 实际通知解析、HTTP解析、镜像校验、元数据与下载状态机；8个场景：正常513字节奇数长度固件、MD5错误、已有pending、HTTP长度不符、错误向量、EEPROM读取失败、提交写入结果不明确、计时回绕 |

`tests/stubs`模拟云端UART/AT边界；`tests/ota_stubs`模拟HAL，OTA测试使用内存模拟外部Flash、EEPROM与HTTP服务器。它们不加入Keil工程，也不代表真实Flash编程、Boot安装或板上网络时序通过。

覆盖与边界见[验证记录](../docs/validation.md)。
