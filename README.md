# tinyblok

ESP-IDF project that links a Zig static library, as groundwork for one day linking the patchbay from [lexvicacom/monoblok](https://github.com/lexvicacom/monoblok) on-device and shipping sensor data back to a monoblok daemon.

Status:
- Connects to wifi (run make menuconfig)
- Connects to a NATS broker over TCP (host/port also in menuconfig under "tinyblok") and sends `CONNECT` — no SUB, no reconnect, no PING/PONG yet
- Run make build flash and then make monitor if you want to connect and run
- prints current temperature annoyingly already quantized to 1s

## Why codegen

`tools/gen.py` compiles `patchbay.edn` to `main/rules.zig` ahead of build. Monoblok walks the parsed s-expr tree at runtime, with a per-message arena for scratch; on a microcontroller that's too much code and too much RAM, so on-device the rules become straight-line Zig with statically-allocated state slots. It's the same DSL implemented differently here; we now have two implementations though, so need to see if we can share possibly. That said, the forms are all quite simple.

## Why some of this is in C

The IDF-touching bits (Wi-Fi, NVS, lwIP sockets, the NATS client, the temperature sensor) live in C. ESP-IDF's headers lean heavily on macros — `ESP_ERROR_CHECK`, `WIFI_INIT_CONFIG_DEFAULT`, `IPSTR`/`IP2STR`, FreeRTOS event-group bits — that don't translate cleanly through `@cImport`, so calling them from Zig would mean writing thin C shims anyway. Keeping the IDF surface in C and reserving Zig for the dependency-free hot path (the patchbay runtime and the sample-and-publish loop) was just the shorter route. It also keeps the Zig side portable enough to host-test without dragging IDF in.
