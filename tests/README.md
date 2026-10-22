# 本机测试

`test_local_monitor.c` 直接测试生产代码 `monitor_state.c` 和 `dht11.c`，用虚拟传感器电平与周期计数代替真实 GPIO。

本次使用 Windows Tiny C Compiler，8 组通过。已有 TCC 时在仓库根目录运行：

```powershell
tcc -Wall -Werror -Ifirmware/Core/Inc tests/test_local_monitor.c firmware/Core/Src/monitor_state.c firmware/Core/Src/dht11.c -o local-monitor-tests.exe
./local-monitor-tests.exe
```

覆盖数据校验、虚拟波形采集、断线恢复、计数回绕、消抖和布防报警。测试不能替代实物引脚、供电、传感器时序和显示效果验证。
