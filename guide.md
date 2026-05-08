# soundcheck

`soundcheck` is a native host CLI for debugging tinyblok patchbay files. It
builds the current `patchbay.edn` into `main/zig/rules.zig`, runs that generated
patchbay on your machine, and prints the resulting message stream.

## Build

```sh
make soundcheck
```

The target first regenerates `main/zig/rules.zig`, then builds `./soundcheck`.

## Input

`soundcheck` reads newline-delimited messages from stdin:

```text
SUBJECT|payload
```

Example:

```sh
printf 'tinyblok.temp|31\n' | ./soundcheck
```

## Output

By default, stdout uses the same shape:

```text
SUBJECT|payload
```

Top-level inputs are passed through first, followed by any patchbay emits:

```sh
printf 'tinyblok.temp|31\n' | ./soundcheck
```

```text
tinyblok.temp|31
tinyblok.temp.avg30s|31.000000
tinyblok.temp.raw|31
tinyblok.temp.max30s|31.000000
tinyblok.temp.min30s|31.000000
tinyblok.temp.count|1
```

## Label Mode

Use `--label` when you want to distinguish inputs from derived output:

```sh
printf 'tinyblok.temp|31\n' | ./soundcheck --label
```

```text
in|tinyblok.temp|31
out|tinyblok.temp.avg30s|31.000000
out|tinyblok.temp.raw|31
out|tinyblok.temp.max30s|31.000000
out|tinyblok.temp.min30s|31.000000
out|tinyblok.temp.count|1
```

Timer-fired messages are also labeled `out`.

## Timers After EOF

Piped input closes stdin immediately, but rules such as `sample!`, `debounce!`,
and wall-clock `bar!` may need time to fire. After stdin EOF, `soundcheck`
keeps pending timers alive for up to 10 seconds by default.

Change the EOF timer window with `--linger-ms N`:

```sh
printf 'tinyblok.rssi|-80\n' | ./soundcheck --label --linger-ms 1200
```

```text
in|tinyblok.rssi|-80
out|tinyblok.rssi.stable|-80.000000
out|tinyblok.rssi.avg5s|-80.000000
out|tinyblok.rssi.clamped|-80.000000
out|tinyblok.rssi.settled|-80.000000
```

Disable EOF timer waiting:

```sh
printf 'tinyblok.rssi|-80\n' | ./soundcheck --linger-ms 0
```

## Help

```sh
./soundcheck --help
./soundcheck --version
```

