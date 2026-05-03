// Embedded patchbay runtime. No IDF deps, no allocator, no globals.
// `publish!` enqueues into tx_ring; rules.collect() drains once at the end of
// each tick, after all dispatches have produced their emits.

const tx_ring = @import("tx_ring.zig");

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

pub fn emitInt(subject: [*:0]const u8, value: i64) void {
    var buf: [32]u8 = undefined;
    const n = snprintf(&buf, buf.len, "%lld", value);
    if (n <= 0) return;
    const len: usize = @min(@as(usize, @intCast(n)), buf.len - 1);
    emit(subject, buf[0..len]);
}

pub const Deadband = struct {
    threshold: f64,
    last: f64 = 0,
    seen: bool = false,

    pub fn update(self: *Deadband, x: f64) ?f64 {
        if (!self.seen) {
            self.seen = true;
            self.last = x;
            return x;
        }
        const d = if (x > self.last) x - self.last else self.last - x;
        if (d < self.threshold) return null;
        self.last = x;
        return x;
    }
};

/// Pass value through if it differs from last. First sight always emits.
pub const Squelch = struct {
    last: f64 = 0,
    seen: bool = false,

    pub fn update(self: *Squelch, x: f64) ?f64 {
        if (!self.seen) {
            self.seen = true;
            self.last = x;
            return x;
        }
        if (x == self.last) return null;
        self.last = x;
        return x;
    }
};

/// Tick-window moving average over N samples. Emits running mean every call.
pub fn MovingAvg(comptime N: usize) type {
    return struct {
        const Self = @This();
        buf: [N]f64 = [_]f64{0} ** N,
        head: usize = 0,
        count: usize = 0,
        sum: f64 = 0,

        pub fn update(self: *Self, x: f64) f64 {
            if (self.count == N) {
                self.sum -= self.buf[self.head];
            } else {
                self.count += 1;
            }
            self.buf[self.head] = x;
            self.sum += x;
            self.head = (self.head + 1) % N;
            return self.sum / @as(f64, @floatFromInt(self.count));
        }
    };
}

/// Fires once on false->true. First sight never fires (per monoblok semantics).
pub const RisingEdge = struct {
    last: bool = false,
    seen: bool = false,

    pub fn update(self: *RisingEdge, cond: bool) bool {
        if (!self.seen) {
            self.seen = true;
            self.last = cond;
            return false;
        }
        const fire = cond and !self.last;
        self.last = cond;
        return fire;
    }
};

/// Fires once on true->false. First sight never fires.
pub const FallingEdge = struct {
    last: bool = false,
    seen: bool = false,

    pub fn update(self: *FallingEdge, cond: bool) bool {
        if (!self.seen) {
            self.seen = true;
            self.last = cond;
            return false;
        }
        const fire = !cond and self.last;
        self.last = cond;
        return fire;
    }
};

/// Pass at most one sample per `interval_us` microseconds. First sight always passes.
/// Caller supplies `now_us` (so the runtime stays clock-source-agnostic).
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

// --- subject helpers --------------------------------------------------------

/// Append ".suffix" to base into a fixed buffer, NUL-terminate.
/// Caveat: the returned pointer is only safe with `emit` if `buf` outlives the
/// next tx_ring.drain — synthesized subjects must come from longer-lived storage.
pub fn subjectAppend(buf: []u8, base: []const u8, suffix: []const u8) ?[*:0]const u8 {
    if (base.len + 1 + suffix.len + 1 > buf.len) return null;
    @memcpy(buf[0..base.len], base);
    buf[base.len] = '.';
    @memcpy(buf[base.len + 1 ..][0..suffix.len], suffix);
    buf[base.len + 1 + suffix.len] = 0;
    return @ptrCast(buf.ptr);
}
