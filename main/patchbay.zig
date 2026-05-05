// Embedded patchbay runtime
// `publish!` enqueues into tx_ring; rules.collect() drains once at the end of
// each tick, after all dispatches have produced their emits.
//
// Op state machines (Squelch, Deadband, MovingAvg, edges) live in the kernel
// vendored from monoblok. Throttle stays local because it speaks microseconds
// against `tinyblok_uptime_us()` while monoblok's hold-off is millisecond-based.

const tx_ring = @import("tx_ring.zig");
const kernel = @import("kernel.zig");

pub const Squelch = kernel.Squelch;
pub const Deadband = kernel.Deadband;
pub const Changed = kernel.Changed;
pub const Delta = kernel.Delta;
pub const RisingEdge = kernel.RisingEdge;
pub const FallingEdge = kernel.FallingEdge;
pub const MovingAvg = kernel.MovingAvg;
pub const MovingSum = kernel.MovingSum;
pub const MovingMax = kernel.MovingMax;
pub const MovingMin = kernel.MovingMin;
pub const Bar = kernel.Bar;
pub const Count = kernel.Count;
pub const quantize = kernel.quantize;
pub const clamp = kernel.clamp;

// std.fmt pulls in __udivti3 which compiler-rt for riscv32-freestanding lacks.
extern fn snprintf(buf: [*]u8, len: usize, fmt: [*:0]const u8, ...) c_int;

extern fn tinyblok_nats_try_send(data: [*]const u8, data_len: usize) callconv(.c) isize;

/// Push everything currently buffered. Called once per tick from rules.collect().
pub fn flush() void {
    tx_ring.drain(tinyblok_nats_try_send);
}

pub fn emit(subject: [*:0]const u8, payload: []const u8) void {
    // Subject is borrowed by pointer in the ring; caller's storage must outlive the drain.
    var n: usize = 0;
    while (subject[n] != 0) : (n += 1) {}
    _ = tx_ring.enqueue(subject[0..n], payload);
}

pub fn emitFloat(subject: [*:0]const u8, value: f64, decimals: u8) void {
    var buf: [32]u8 = undefined;
    const prec: c_int = @intCast(decimals);
    const n = snprintf(&buf, buf.len, "%.*f", prec, value);
    if (n <= 0) return;
    const len: usize = @min(@as(usize, @intCast(n)), buf.len - 1);
    emit(subject, buf[0..len]);
}

// Reentrant emits: monoblok lets a rule marked `:reentrant true` publish a
// subject that re-enters rule eval, so a downstream `(on ...)` rule sees it.
// Generated code passes its filter-table dispatcher as `redispatch`; the cap
// mirrors monoblok's Context.max_depth.
pub const MAX_DEPTH: u8 = 8;

pub const Redispatch = *const fn (subject: []const u8, payload: []const u8, depth: u8) void;

pub fn emitReentrant(
    subject: [*:0]const u8,
    payload: []const u8,
    depth: u8,
    redispatch: Redispatch,
) void {
    emit(subject, payload);
    if (depth + 1 >= MAX_DEPTH) return;
    var n: usize = 0;
    while (subject[n] != 0) : (n += 1) {}
    redispatch(subject[0..n], payload, depth + 1);
}

pub fn emitReentrantFloat(
    subject: [*:0]const u8,
    value: f64,
    depth: u8,
    redispatch: Redispatch,
) void {
    var buf: [32]u8 = undefined;
    const n = snprintf(&buf, buf.len, "%.6f", value);
    if (n <= 0) return;
    const len: usize = @min(@as(usize, @intCast(n)), buf.len - 1);
    emitReentrant(subject, buf[0..len], depth, redispatch);
}

pub fn emitInt(subject: [*:0]const u8, value: i64) void {
    var buf: [32]u8 = undefined;
    const n = snprintf(&buf, buf.len, "%lld", value);
    if (n <= 0) return;
    const len: usize = @min(@as(usize, @intCast(n)), buf.len - 1);
    emit(subject, buf[0..len]);
}

pub const Throttle = struct {
    interval_us: u64,
    last_us: u64 = 0,
    seen: bool = false,

    pub fn update(self: *Throttle, x: f64, now_us: u64) ?f64 {
        if (!self.seen or (now_us -% self.last_us) >= self.interval_us) {
            self.seen = true;
            self.last_us = now_us;
            return x;
        }
        return null;
    }
};

pub fn subjectAppend(buf: []u8, base: []const u8, suffix: []const u8) ?[*:0]const u8 {
    if (base.len + 1 + suffix.len + 1 > buf.len) return null;
    @memcpy(buf[0..base.len], base);
    buf[base.len] = '.';
    @memcpy(buf[base.len + 1 ..][0..suffix.len], suffix);
    buf[base.len + 1 + suffix.len] = 0;
    return @ptrCast(buf.ptr);
}
