# AGENTS.md

Repository guidance for Codex and other coding agents working in this tree.

## Project Shape

Tinyblok is ESP-IDF firmware for ESP32-C6. It is C firmware that embeds a
monoblok-derived C patchbay parser/evaluator and publishes sensor data to NATS.

Keep `main/` as the ESP-IDF component root:

- `main/c/` holds firmware C code, the Tinyblok patchbay glue, NATS, Wi-Fi, sockets, TLS, sensors, the TX ring, and vendored TweetNaCl.
- `main/c/patchbay/` vendors the monoblok C patchbay arena, parser, evaluator, validator, and builtins. Keep it close to upstream.
- `main/c/vendor/yyjson/` vendors yyjson for JSON patchbay helpers.
- `main/CMakeLists.txt` registers C sources and embeds `patchbay.edn` into a generated header.

## Common Commands

Use the top-level `Makefile`. Run it from a shell where ESP-IDF is already
exported, install ESP-IDF at `$(HOME)/esp-idf-v6.0.1`, or pass
`IDF_PATH=/path/to/esp-idf` or `IDF_EXPORT=/path/to/esp-idf/export.sh`.

```sh
make gen                # no-op note; patchbay.edn is runtime-parsed
make soundcheck         # build/run host C patchbay validator
make test               # host C patchbay validation
make build              # idf.py build
make flash              # build and flash to $(PORT)
make monitor            # serial monitor, exit with Ctrl+]
make flash-monitor      # build, flash, monitor
make menuconfig         # Wi-Fi, NATS, TLS, auth settings
```

`PORT` is optional. Leave it unset for ESP-IDF auto-detection, or pass a serial
device such as `PORT=/dev/cu.usbmodem101`.

## Source Ownership

- `main/c/stub.c`: `app_main`, NVS, Wi-Fi, patchbay init, NATS connect, driver install, main loop.
- `main/c/nats.c`: NATS client, plaintext/TLS, auth, reconnect behavior.
- `main/c/creds.c`, `main/c/ed25519.c`, `main/c/tweetnacl.c`: creds auth support, only built when enabled.
- `main/c/drivers.c` and `main/c/sources.c`: pump timer/event layer and sensor reads.
- `main/c/tinyblok_patchbay.c`: embeds Tinyblok-specific `(pump ...)`, `(fn ...)`, `(on-req ...)`, and evaluator hooks.
- `main/c/tinyblok_tx_ring.c`: bounded publish queue drained by the main loop.
- `main/c/user.c`: user-owned C helpers named from `patchbay.edn`.
- `main/c/patchbay/*`: vendored monoblok patchbay core. Keep local changes minimal and document Tinyblok compatibility paths.

## Important Rules

- Do not edit `sdkconfig` for committed defaults; use `sdkconfig.defaults` for checked-in config.
- Never commit real secrets. `sdkconfig` and `secrets/*` are gitignored; `secrets/nats.creds.example` is the placeholder.
- New tinyblok compile-time settings belong in `main/Kconfig.projbuild`, not ad hoc C defines.
- Registered `(fn ...)` declarations use `:type` for return type and optional `:input` for arity/input. Omit `:input` for zero-arg scalar reads; use `:input bytes :type bytes` for request payload transforms; scalar `:input` functions are threaded ops.
- New compiled symbols named from `patchbay.edn` must be added to `tinyblok_patchbay.c`'s native symbol table.
- Keep ESP-IDF-heavy code out of `main/c/patchbay/`; the vendored patchbay core should stay host-portable.

## Memory Model

- Use stack buffers for temporary formatting, parsing, and subject construction.
- Patchbay parse trees and temporary eval values live in arenas. Long-lived rule state is owned by `pb_eval_state`.
- Use fixed rings or fixed pools for persistent windows such as aggregates, samples, and queued publishes.
- Use heap allocation only at boot/config boundaries, not per pump tick, per publish, or per clock fire.

## Firmware Style

Borrow the embedded-relevant parts of TigerBeetle's Tiger Style, adapted to Tinyblok rather than copied wholesale:

- Bound all runtime work. Loops, queues, payloads, subjects, sample windows, reconnect attempts, and generated-rule walks should have clear upper limits.
- Keep pump/timer/publish paths deterministic. Do not allocate, block unexpectedly, recurse, or do unbounded parsing in hot paths.
- Prefer explicit integer widths for protocol, wire-format, sensor, timing, and C ABI values.
- Name values with units or capacity meaning where it prevents mistakes: `period_ms`, `payload_len`, `subject_len_max`, `publish_count`, `samples_cap`.
- Assert invariants at subsystem boundaries: parsed rule shape, ring indexes/counts, payload lengths, subject lengths, ABI type sizes, and impossible enum states.
- Handle every operating error explicitly. ESP-IDF errors, socket/TLS failures, auth failures, short reads/writes, sensor failures, and publish drops should be logged or surfaced intentionally.
- Keep control flow simple around external events. Let Wi-Fi, driver, timer, and NATS callbacks enqueue or signal work; keep substantial policy in code that runs at Tinyblok's own pace.
- Be conservative with new dependencies. ESP-IDF and vendored support code are already enough surface area; add dependencies only when the reliability or maintenance win is clear.
- Treat large functions, compound conditions, and unclear negations as review smells. Split only when the resulting shape makes state, bounds, and error handling easier to verify.

## Build Notes

`main/CMakeLists.txt` intentionally lists optional sources eagerly, then applies Kconfig conditionals after `idf_component_register` with `target_sources` and `idf_component_optional_requires`. ESP-IDF's component manager parses the registration call greedily.

Vendored code such as yyjson should not inherit project warning policy where
that creates irrelevant `-Werror` failures. Suppress warnings narrowly on the
vendored source file.

## Verification

For source/layout changes, run at least:

```sh
make test
make build
```

`make test` builds a host C patchbay validator and checks `patchbay.edn`.
Firmware behavior still needs `make flash-monitor` on hardware.
