# Tinyblok Component Firmware Example

This is a minimal ESP-IDF project that uses Tinyblok as its firmware component.
Tinyblok owns `app_main`, setup portal, Wi-Fi, NATS, pump timers, and patchbay
runtime. This project supplies only:

- `patchbay.yml`: rules and pumps.
- `components/board/board.c`: board-specific native symbols.

The board component is linked with `WHOLE_ARCHIVE` because Tinyblok discovers
`tinyblok_app_native_symbols()` at runtime. Without that, the static linker may
drop the component and patchbay init will fail when it sees `board_*` symbols.

Prepare the Tinyblok checkout with monoblok present:

```sh
git clone --recurse-submodules <tinyblok-url>
cd tinyblok
```

Or, from an existing Tinyblok checkout:

```sh
git submodule update --init --recursive
```

Build it from the Tinyblok repo root:

```sh
make component-example-build
make component-example-flash-monitor
```

Or from this directory:

```sh
make build
make flash-monitor
```

The default patchbay is publish-only. Request handlers such as `on-req` create
NATS subscriptions, so the broker account must allow the subject. A permissions
violation is logged as a warning and the client backs off/retries, but it will
not be a stable connection until the account permissions or request subject are
fixed.

For a copied project, update `TINYBLOK_REPO` in `CMakeLists.txt` or replace the
`EXTRA_COMPONENT_DIRS` entry with an absolute path to `tinyblok/main`. Tinyblok
expects monoblok at `third_party/monoblok` inside that checkout. To use a
separate monoblok checkout, set `TINYBLOK_MONOBLOK_DIR` in CMake or pass it on
the build command:

```sh
idf.py -DTINYBLOK_MONOBLOK_DIR=/path/to/monoblok build
```

The setup portal still provides Wi-Fi and NATS runtime configuration. The custom
partition table mirrors the root Tinyblok project because the firmware image is
larger than ESP-IDF's default 1 MB app partition.
