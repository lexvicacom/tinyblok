const std = @import("std");
const Io = std.Io;
const posix = std.posix;
const manifest = @import("manifest");
const rules = @import("rules.zig");
const tx_ring = @import("tx_ring.zig");

var boot_us: u64 = 0;
var clock_slots: [16]?i64 = [_]?i64{null} ** 16;
const default_linger_ms: i64 = 10_000;

pub fn main(init: std.process.Init) !void {
    const io = init.io;

    var stdout_buf: [4096]u8 = undefined;
    var stdout_writer = Io.File.stdout().writerStreaming(io, &stdout_buf);
    const stdout = &stdout_writer.interface;
    defer stdout.flush() catch {};

    var stderr_buf: [1024]u8 = undefined;
    var stderr_writer = Io.File.stderr().writerStreaming(io, &stderr_buf);
    const stderr = &stderr_writer.interface;
    defer stderr.flush() catch {};

    var label_stream = false;
    var linger_ms: i64 = default_linger_ms;
    const args = try init.minimal.args.toSlice(init.arena.allocator());
    var arg_i: usize = 1;
    while (arg_i < args.len) : (arg_i += 1) {
        const arg = args[arg_i];
        if (std.mem.eql(u8, arg, "-h") or std.mem.eql(u8, arg, "--help")) {
            try printHelp(stdout);
            return;
        }
        if (std.mem.eql(u8, arg, "--version")) {
            try stdout.print("soundcheck {s}\n", .{manifest.version});
            return;
        }
        if (std.mem.eql(u8, arg, "--label")) {
            label_stream = true;
            continue;
        }
        if (std.mem.eql(u8, arg, "--linger-ms")) {
            arg_i += 1;
            if (arg_i >= args.len) {
                try stderr.writeAll("soundcheck: --linger-ms requires a value\n\n");
                try printHelp(stderr);
                return error.InvalidArgs;
            }
            linger_ms = std.fmt.parseInt(i64, args[arg_i], 10) catch {
                try stderr.print("soundcheck: invalid --linger-ms value: {s}\n\n", .{args[arg_i]});
                try printHelp(stderr);
                return error.InvalidArgs;
            };
            if (linger_ms < 0) {
                try stderr.writeAll("soundcheck: --linger-ms must be >= 0\n\n");
                try printHelp(stderr);
                return error.InvalidArgs;
            }
            continue;
        }
        try stderr.print("soundcheck: unknown option: {s}\n\n", .{arg});
        try printHelp(stderr);
        return error.InvalidArgs;
    }

    boot_us = monotonicUs();

    var line_buf: [256]u8 = undefined;
    var line_len: usize = 0;
    var stdin_open = true;
    var linger_deadline: ?i64 = null;

    while (true) {
        fireDueClocks();
        try drainOutput(stdout, label_stream, null);
        try stdout.flush();

        if (!stdin_open) {
            if (!hasPendingClocks()) break;
            const deadline = linger_deadline orelse break;
            const now = tinyblok_now_ms();
            if (now >= deadline) break;
            var no_fds: [0]posix.pollfd = .{};
            _ = try posix.poll(&no_fds, boundedTimeoutMs(nextTimeoutMs(), deadline));
            continue;
        }

        var fds = [_]posix.pollfd{.{
            .fd = posix.STDIN_FILENO,
            .events = posix.POLL.IN,
            .revents = 0,
        }};
        _ = try posix.poll(&fds, nextTimeoutMs());

        if ((fds[0].revents & (posix.POLL.IN | posix.POLL.HUP | posix.POLL.ERR)) == 0) {
            continue;
        }

        var read_buf: [512]u8 = undefined;
        const n = try posix.read(posix.STDIN_FILENO, &read_buf);
        if (n == 0) {
            if (line_len > 0) {
                try handleLine(line_buf[0..line_len], stdout, stderr, label_stream);
                line_len = 0;
            }
            stdin_open = false;
            linger_deadline = eofLingerDeadline(linger_ms);
            continue;
        }

        for (read_buf[0..n]) |b| {
            if (b == '\n') {
                const line = std.mem.trimEnd(u8, line_buf[0..line_len], "\r");
                try handleLine(line, stdout, stderr, label_stream);
                line_len = 0;
            } else if (line_len < line_buf.len) {
                line_buf[line_len] = b;
                line_len += 1;
            } else {
                try stderr.writeAll("soundcheck: input line too long\n");
                line_len = 0;
            }
        }
    }

    fireDueClocks();
    try drainOutput(stdout, label_stream, null);
}

fn printHelp(writer: *std.Io.Writer) !void {
    try writer.print(
        \\soundcheck {s}
        \\
        \\Helps you debug patchbay files by running the generated tinyblok
        \\patchbay on your host.
        \\
        \\Usage:
        \\  soundcheck [--label] [--linger-ms N] [--help] [--version]
        \\  printf 'SUBJECT|payload\n' | soundcheck
        \\
        \\Input:
        \\  Newline-delimited SUBJECT|payload messages on stdin.
        \\
        \\Output:
        \\  Newline-delimited SUBJECT|payload messages on stdout. Top-level
        \\  inputs are passed through first, followed by patchbay emits.
        \\  With --label, rows are prefixed as in|SUBJECT|payload or
        \\  out|SUBJECT|payload.
        \\
        \\Timers:
        \\  After stdin EOF, pending timers keep running for up to 10000 ms.
        \\  Use --linger-ms N to change that window, or 0 to disable it.
        \\
    , .{manifest.version});
}

fn drainOutput(stdout: *std.Io.Writer, label_stream: bool, first_label: ?[]const u8) !void {
    if (label_stream) {
        try tx_ring.drainDelimitedLabeled(stdout, first_label, "out");
    } else {
        try tx_ring.drainDelimited(stdout);
    }
}

fn handleLine(
    line: []const u8,
    stdout: *std.Io.Writer,
    stderr: *std.Io.Writer,
    label_stream: bool,
) !void {
    if (line.len == 0) return;
    const sep = std.mem.indexOfScalar(u8, line, '|') orelse {
        try stderr.print("soundcheck: expected SUBJECT|payload, got: {s}\n", .{line});
        return;
    };

    const subject = std.mem.trim(u8, line[0..sep], " \t");
    const payload = line[sep + 1 ..];
    if (subject.len == 0) {
        try stderr.writeAll("soundcheck: empty subject\n");
        return;
    }

    rules.dispatch(subject, payload);
    fireDueClocks();
    try drainOutput(stdout, label_stream, "in");
}

fn fireDueClocks() void {
    const now = tinyblok_now_ms();
    for (clock_slots[0..rules.tinyblok_clock_slot_count], 0..) |deadline, i| {
        if (deadline) |dl| {
            if (now >= dl) {
                clock_slots[i] = null;
                rules.tinyblok_clock_slots[i].fire();
            }
        }
    }
}

fn hasPendingClocks() bool {
    for (clock_slots[0..rules.tinyblok_clock_slot_count]) |deadline| {
        if (deadline != null) return true;
    }
    return false;
}

fn eofLingerDeadline(linger_ms: i64) ?i64 {
    if (linger_ms == 0 or !hasPendingClocks()) return null;
    return tinyblok_now_ms() + linger_ms;
}

fn nextTimeoutMs() i32 {
    const now = tinyblok_now_ms();
    var next: ?i64 = null;
    for (clock_slots[0..rules.tinyblok_clock_slot_count]) |deadline| {
        if (deadline) |dl| {
            if (next == null or dl < next.?) next = dl;
        }
    }
    const dl = next orelse return -1;
    if (dl <= now) return 0;
    return @intCast(@min(dl - now, std.math.maxInt(i32)));
}

fn boundedTimeoutMs(next_clock_timeout: i32, linger_deadline_ms: i64) i32 {
    const now = tinyblok_now_ms();
    const linger_timeout: i32 = if (linger_deadline_ms <= now)
        0
    else
        @intCast(@min(linger_deadline_ms - now, std.math.maxInt(i32)));

    if (next_clock_timeout < 0) return linger_timeout;
    return @min(next_clock_timeout, linger_timeout);
}

fn monotonicUs() u64 {
    var ts: std.c.timespec = undefined;
    if (std.c.clock_gettime(.MONOTONIC, &ts) != 0) return 0;
    const sec: u64 = @intCast(ts.sec);
    const nsec: u64 = @intCast(ts.nsec);
    return sec * std.time.us_per_s + nsec / std.time.ns_per_us;
}

export fn tinyblok_tx_ring_lock() callconv(.c) void {}
export fn tinyblok_tx_ring_unlock() callconv(.c) void {}

export fn tinyblok_nats_try_send(_: [*]const u8, _: usize) callconv(.c) isize {
    return -1;
}

export fn tinyblok_nats_reply(
    subject: [*]const u8,
    subject_len: usize,
    payload: [*]const u8,
    payload_len: usize,
) callconv(.c) c_int {
    _ = tx_ring.enqueue(subject[0..subject_len], payload[0..payload_len]);
    return 0;
}

export fn tinyblok_clock_arm(slot_id: usize, us_until: u64) callconv(.c) void {
    if (slot_id >= clock_slots.len) return;
    const due_us = tinyblok_uptime_us() +| us_until;
    clock_slots[slot_id] = @as(i64, @intCast(due_us / 1000));
}

export fn tinyblok_uptime_us() u64 {
    const now = monotonicUs();
    if (now <= boot_us) return 0;
    return now - boot_us;
}

export fn tinyblok_now_ms() i64 {
    return @intCast(tinyblok_uptime_us() / 1000);
}

export fn tinyblok_hello_c(
    payload_ptr: [*]const u8,
    payload_len: usize,
    out_ptr: [*]u8,
    out_len: usize,
) usize {
    return copyPrefixPayload("hello from host c stub: ", payload_ptr[0..payload_len], out_ptr[0..out_len]);
}

export fn tinyblok_hello_zig(
    payload_ptr: [*]const u8,
    payload_len: usize,
    out_ptr: [*]u8,
    out_len: usize,
) usize {
    return copyPrefixPayload("hello from zig: ", payload_ptr[0..payload_len], out_ptr[0..out_len]);
}

fn copyPrefixPayload(prefix: []const u8, payload: []const u8, out: []u8) usize {
    var n: usize = 0;
    for (prefix) |b| {
        if (n >= out.len) return n;
        out[n] = b;
        n += 1;
    }
    for (payload) |b| {
        if (n >= out.len) return n;
        out[n] = b;
        n += 1;
    }
    return n;
}

export fn tinyblok_free_heap() u32 {
    return 0;
}

export fn tinyblok_wifi_rssi() c_int {
    return 0;
}

export fn tinyblok_read_temp_c() f32 {
    return 0;
}

export fn vTaskDelay(_: u32) void {}

test "generated rules registries are internally consistent" {
    try std.testing.expectEqual(rules.tinyblok_pump_count, rules.tinyblok_pumps.len);
    try std.testing.expectEqual(rules.tinyblok_request_sub_count, rules.tinyblok_request_subs.len);
    try std.testing.expectEqual(rules.tinyblok_clock_slot_count, rules.tinyblok_clock_slots.len);
    try std.testing.expect(rules.tinyblok_clock_slot_count <= clock_slots.len);
    try std.testing.expect(rules.tinyblok_clock_slot_count <= 32);
}

test "generated pump metadata is bounded" {
    for (rules.tinyblok_pumps) |pump| {
        const subject = std.mem.span(pump.subject);
        try std.testing.expect(subject.len > 0);
        try std.testing.expect(subject.len <= tx_ring.SUBJ_MAX);
        try std.testing.expect(pump.period_us > 0);
    }
}

test "generated request subjects are bounded and unique" {
    for (rules.tinyblok_request_subs, 0..) |sub, i| {
        const subject = std.mem.span(sub.subject);
        try std.testing.expect(subject.len > 0);
        try std.testing.expect(subject.len <= 128);

        for (rules.tinyblok_request_subs[0..i]) |prev| {
            try std.testing.expect(!std.mem.eql(u8, subject, std.mem.span(prev.subject)));
        }
    }
}

test "generated dispatch keeps telemetry inside ring bounds" {
    const dropped_before = tx_ring.dropped;

    rules.dispatch("tinyblok.temp", "31");

    try std.testing.expect(tx_ring.used() <= tx_ring.capacity());
    try std.testing.expect(tx_ring.count() > 0);
    try std.testing.expectEqual(dropped_before, tx_ring.dropped);
}
