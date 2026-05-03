# tinyblok

ESP-IDF project that links a Zig static library, as groundwork for one day linking the patchbay from [lexvicacom/monoblok](https://github.com/lexvicacom/monoblok) on-device and shipping sensor data back to a monoblok daemon.

Status:
- Connects to wifi (run make menuconfig)
- Connects to a NATS broker over TCP (host/port also in menuconfig under "tinyblok") and sends `CONNECT`. No SUB, no reconnect, no PING/PONG yet
- Run make build flash and then make monitor if you want to connect and run
- prints current temperature annoyingly already quantized to 1s

## Why codegen

A small Python tool compiles the patchbay s-expression file into Zig ahead of build. Monoblok walks the parsed tree at runtime with a per-message arena for scratch; on a microcontroller that's too much code and too much RAM, so on-device the rules become straight-line Zig with statically-allocated state slots. It's the same DSL implemented differently here; we now have two implementations though, so need to see if we can share possibly. That said, the forms are all quite simple.

## TX ring

A ring buffer sits between rule eval and the NATS socket. `publish!` enqueues a record (subject borrowed by pointer, payload inline); the loop drains it via non-blocking `send()` once per tick. A slow broker or Wi-Fi retransmit burst drops the oldest queued samples rather than stalling the rule loop, so when the broker comes back it gets a catch-up burst of recent data instead of stale history. Default capacity is 8 KB ≈ 256 messages at typical payload sizes; configurable.

A lot of the time old messages have no value and can just be dropped, which is what the ring already does. In contexts where signal is spotty and old messages have value in spite of their age, such as a remote sensor where every reading matters and the link is flaky for hours at a time, a future option is to spool overflow records to LittleFS on flash so a long outage can be flushed when the broker returns.

A related future addition is a producer-side timestamp header (such as `X-Measured-At: <unix_ms>`) so downstream consumers can distinguish "when the device measured this" from "when the broker ingested it". Useful precisely when the ring delivers a catch-up burst on reconnect.

## Why some of this is in C

The IDF-touching bits (Wi-Fi, NVS, lwIP sockets, the NATS client, the temperature sensor) live in C. ESP-IDF's headers lean heavily on macros like `ESP_ERROR_CHECK`, `WIFI_INIT_CONFIG_DEFAULT`, `IPSTR`/`IP2STR`, and FreeRTOS event-group bits, which don't translate cleanly through `@cImport`, so calling them from Zig would mean writing thin C shims anyway. Keeping the IDF surface in C and reserving Zig for the dependency-free hot path (the patchbay runtime and the sample-and-publish loop) was just the shorter route. It also keeps the Zig side portable enough to host-test without dragging IDF in.
