# tinyblok

ESP-IDF project uses the patchbay DSL from [lexvicacom/monoblok](https://github.com/lexvicacom/monoblok) on a MCU dev board, shipping sensor data back to a NATS cluster (or monoblok) over TCP. [Read the introductory blog post](https://alexjreid.dev/posts/tinyblok/)

## Status
- Connects to wifi (run make menuconfig to setup)
- Connects to a NATS broker over TCP (host/port config also in menuconfig under "tinyblok")
- Run make build flash and then make monitor if you want to connect and run
- emits to a handful of `tinyblock.>` subjects on specified broker, see the [patchbay.edn](./patchbay.edn)

<img width="1145" height="630" alt="Screenshot 2026-05-03 at 16 26 21" src="https://github.com/user-attachments/assets/b3b0980e-fd8d-4564-9d7a-4d0aae1448ed" />

## Why codegen

A small Python tool compiles the patchbay s-expression file into Zig ahead of build. While monoblok walks the parsed tree at runtime with a per-message arena for scratch; on a microcontroller that is not feasible, so on-device the rules become straight-line Zig with statically-allocated state slots. The op kernels themselves (squelch, deadband, moving-*, edges, bars, …) live in a shared kernel that both monoblok and tinyblok call into, so the DSL has one implementation and the codegen step is just wiring things at build (comp!?) time. Using `comptime` was too hard for me at this stage but I actually think Python is fine here.

## Drivers

A driver is just a function. `(pump "tinyblok.temp" :from tinyblok_read_temp_c :type f32 :hz 1)` in the patchbay says "call this function at 1 Hz, format the return value, and publish it to `tinyblok.temp`." Codegen turns each `(pump ...)` form into an `extern fn` declaration on the Zig side and an entry in a static `tinyblok_pumps[]` table the C side reads. The function itself can be written in either language: a C function (like `tinyblok_read_temp_c`, which calls into IDF's temperature sensor API) or an exported Zig function (`export fn ... callconv(.c)`) both satisfy the same `extern fn` contract. Pick C when the driver needs IDF headers, pick Zig when it doesn't. Adding a sensor means writing one function and one line of patchbay. See [alexjreid.dev/posts/tinyblok](https://alexjreid.dev/posts/tinyblok/) for worked examples.

The C side (`main/drivers.c`) wires that table into IDF's native event loop. Each pump gets its own `esp_timer` armed at the requested period; the timer fires onto a private `esp_event` base, and a single handler indexes into the pump table to call the generated `fire()` into Zig. Sample reads, formatting, and rule evaluation all run on the `esp_event` task, so the main Zig loop only handles network drain.

Only polled drivers exist today. Push-style drivers (GPIO ISRs, UART RX) are not yet implemented; the plan is for them to use a slightly different `(pump ...)` form (no `:hz`, since the source decides cadence) and register their own event ids on the same `esp_event` base, reusing the handler shape so an interrupt-driven sensor looks the same to the patchbay as a polled one.

## TX ring

A ring buffer sits between rule eval and the NATS socket. `publish!` enqueues a record (subject borrowed by pointer with payload inline); the loop drains it via non-blocking `send()` once per tick. A slow broker or Wi-Fi retransmit burst drops the oldest queued samples rather than stalling the rule loop, so when the broker comes back it gets a catch-up burst of recent data instead of stale history. Default capacity is 8 KB ≈ 256 messages at typical payload sizes; this is configurable.

A lot of the time old messages have no value and can just be dropped, which is what the ring already does. In contexts where signal is spotty and old messages have value in spite of their age, such as a remote sensor where every reading matters and the link is flaky for hours at a time, a future option is to spool overflow records to LittleFS on flash so a long outage can be flushed when the broker returns.

A related future addition is a producer-side timestamp header (such as `X-Measured-At: <unix_ms>`) so downstream consumers can distinguish "when the device measured this" from "when the broker ingested it". Useful precisely when the ring delivers a catch-up burst on reconnect.

## Why some of this is in C

The IDF-touching bits (Wi-Fi, NVS, lwIP sockets, the NATS client, the temperature sensor) live in C. ESP-IDF's headers lean heavily on macros like `ESP_ERROR_CHECK`, `WIFI_INIT_CONFIG_DEFAULT`, `IPSTR`/`IP2STR`, and FreeRTOS event-group bits, which don't translate cleanly through `@cImport`, so calling them from Zig would mean writing thin C shims anyway. Keeping the IDF surface in C and reserving Zig for the dependency-free hot path (the patchbay runtime and the sample-and-publish loop) was just the shorter route. It also keeps the Zig side portable enough to host-test without dragging IDF in.

The driver layer falls under the same rule. `main/drivers.c` is the bridge between the generated pump table and IDF's native event loop: it arms one `esp_timer` per pump, posts onto a private `esp_event` base, and a single handler dispatches back into Zig. Doing that interop from Zig would mean reimplementing macros like `ESP_EVENT_DEFINE_BASE` and `ESP_ERROR_CHECK` by hand, so the bridge stays in C and Zig only sees the `fire()` callback at the end of it.
