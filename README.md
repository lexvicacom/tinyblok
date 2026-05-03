# tinyblok

ESP-IDF project that links a Zig static library, as groundwork for one day linking the patchbay from [lexvicacom/monoblok](https://github.com/lexvicacom/monoblok) on-device and shipping sensor data back to a monoblok daemon.

Status:
- Connects to wifi (run make menuconfig)
- Run make build flash and then make monitor if you want to connect and run
- prints current temperature annoyingly already quantized to 1s
