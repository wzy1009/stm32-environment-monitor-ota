# 本机测试

共 10 组，使用 Windows Tiny C Compiler。已有 TCC 时在仓库根目录运行：

```powershell
tcc -Wall -Werror -Ifirmware/Core/Inc tests/test_local_monitor.c firmware/Core/Src/monitor_state.c firmware/Core/Src/dht11.c -o local-monitor-tests.exe
./local-monitor-tests.exe
tcc -Wall -Werror -Itests/stubs -Ifirmware/Core/Inc tests/test_cloud.c firmware/Core/Src/cloud_task.c firmware/Core/Src/cloud_mqtt.c firmware/Core/Src/esp_stream.c firmware/Core/Src/monitor_state.c -o cloud-tests.exe
./cloud-tests.exe
```

- `test_local_monitor.c`：8 组，测试生产状态机与 DHT11 驱动，以虚拟电平和周期计数代替 GPIO。
- `test_cloud.c`：2 组，测试生产 MQTT 编解码、IPD 拆帧和云端状态机；模拟 AT/UART 与服务器应答。
- `stubs`：仅用于本机测试，包含固定的虚假凭据与 AT 接口替身；不加入 Keil 工程。

测试覆盖与边界见 [验证记录](../docs/validation.md)。软件模拟不能替代实板接线、供电、串口/DHT11 并发时序或真实云端验证。
