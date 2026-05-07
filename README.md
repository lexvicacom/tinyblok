# tinyblok

ESP-IDF firmware for ESP32-C6. It runs a tiny patchbay on-device and publishes sensor data to NATS. The DSL/runtime matches the pure Zig op kernel vendored from [lexvicacom/monoblok](https://github.com/lexvicacom/monoblok). [Intro blog post](https://alexjreid.dev/posts/tinyblok/).

## Status

- Connects to Wi-Fi, then NATS over TCP or TLS.
- Supports no auth, user/pass, or NATS `.creds` auth - **works with Synadia Cloud** and other operator mode clusters
- Publishes heap, RSSI, uptime, and temperature-derived subjects from [`patchbay.edn`](./patchbay.edn).
- You can configure Wi-Fi and NATS details with `make menuconfig`; ESP-IDF writes them to local `sdkconfig`.
- Run `make build flash`, then `make monitor` to try it on hardware.

<img width="1145" height="630" alt="Screenshot 2026-05-03 at 16 26 21" src="https://github.com/user-attachments/assets/b3b0980e-fd8d-4564-9d7a-4d0aae1448ed" />

## How it fits

[`tools/gen.py`](./tools/gen.py) compiles [`patchbay.edn`](./patchbay.edn) into [`main/zig/rules.zig`](./main/zig/rules.zig). The generated Zig is straight-line rule code with static state slots, which is much friendlier to a microcontroller than walking an s-expression tree at runtime.

The reusable ops live in [`main/zig/kernel.zig`](./main/zig/kernel.zig). That file is vendored from monoblok and should stay byte-identical; use `make sync-kernel` or `make sync-kernel-remote` when it changes upstream.

## Patchbay Lite

Tinyblok is a firmware-sized patchbay for telemetry. Rules are compiled ahead
of time into deterministic Zig, run with fixed state, read local sensor pumps,
derive new subjects, answer fixed request/reply subjects, and publish the
results to NATS without allocating on the hot path.

It is not trying to be a full monoblok runtime on an ESP32-C6. Runtime patch
loading, dynamic graph edits, JSON/event document processing, inbound bridges,
and fleet-management features are outside the current scope.

### What's there and not there

Tinyblok intentionally implements a static, numeric subset of monoblok's
patchbay. Supported forms include `when`, `->`, comparisons, `deadband`,
`squelch`, `moving-*`, `round`, `quantize`, `clamp`, `throttle`, edge
gates, `publish!`, `count!`, `bar!`, `sample!`, `debounce!`, `on-req`,
and `reply!`.

Not yet supported in Tinyblok: `if`, `do`, `transition`, `on-silence`,
`aggregate!`, JSON forms, string/subject builders beyond publish-target
`subject-append`, general arithmetic, `changed?`, `delta`, `hold-off`,
`rate`, `percentile`, `median`, `stddev`, `variance`, `min`, `max`,
`abs`, `sign`, `lvc`, and `bridge`.

>This does not mean never - some are trivial, some make no sense to even bother with (bridge, for instance is implicit). Others are tricky due to static code gen.

## Drivers

A driver is just a function named from [`patchbay.edn`](./patchbay.edn):

```clojure
(pump "tinyblok.temp" :from tinyblok_read_temp_c :type f32 :hz 1)
```

Codegen declares the function for Zig and adds it to a C pump table. [`main/c/drivers.c`](./main/c/drivers.c) arms one `esp_timer` per pump, posts onto `esp_event`, then calls back into Zig. Use C for IDF-heavy sources, Zig for dependency-free ones.

## Request/reply

`on-req` declares fixed NATS service subjects that Tinyblok subscribes to after
every broker connect. The requester owns the `_INBOX` reply subject; Tinyblok
only parses the incoming `MSG` reply-to field and sends `reply!` back to it.
That keeps request handling static like the rest of Patchbay Lite: no arbitrary
runtime `SUB`, no generated inboxes, and no pending request table on-device.

This is useful for small control-plane actions that should not be continuous
telemetry: pinging a device, reading uptime, asking it to reload published
metadata, starting a sensor sweep, or triggering a one-shot diagnostic sample.
It also works naturally for fleet queries. If many devices subscribe to the
same request subject, one `nats req`-style request is effectively a broadcast:
each device receives the same request and replies to the requester's inbox.
Clients that expect fleet replies should wait for multiple responses rather
than stopping after the first one.

```clojure
(on-req "tinyblok.req.ping"
  (reply! "pong"))

(on-req "tinyblok.req.uptime"
  (-> uptime-s
      (round 3)
      (reply!)))
```

From a NATS client:

```sh
nats req tinyblok.req.ping ''
nats req tinyblok.req.uptime ''
```

<img width="1145" height="630" alt="NATS request/reply ping example" src="./docs/pong.png" />

## TX ring

[`main/zig/tx_ring.zig`](./main/zig/tx_ring.zig) sits between rule eval and the NATS socket. `publish!` queues a subject/payload record; [`main/zig/main.zig`](./main/zig/main.zig) drains it through [`main/c/nats.c`](./main/c/nats.c). If Wi-Fi or the broker stalls, the ring drops the oldest samples rather than blocking rule evaluation.

## Why C and Zig

[`main/c/stub.c`](./main/c/stub.c), [`main/c/nats.c`](./main/c/nats.c), [`main/c/drivers.c`](./main/c/drivers.c), and [`main/c/sources.c`](./main/c/sources.c) own the ESP-IDF surface: Wi-Fi, NVS, sockets, TLS, timers, events, and sensors. Zig owns the portable patchbay path: generated rules, op kernels, and the publish queue.

That split keeps IDF macros out of `@cImport` and keeps the Zig side small enough to host-test with `make test`.

## Commands

Use the top-level [`Makefile`](./Makefile):

```sh
make gen
make test
make build
make flash
make monitor
make menuconfig
```

Checked-in defaults belong in [`sdkconfig.defaults`](./sdkconfig.defaults); local choices live in `sdkconfig`. Real secrets do not: use [`secrets/nats.creds.example`](./secrets/nats.creds.example) as the local `.creds` template.
