//! Per-op state-machine kernels. Pure logic, no allocator, no Context, no
//! Value, no slot tables. The single source of truth for what each
//! patchbay op *means*; both the host evaluator (builtins.zig) and the
//! tinyblok codegen target this surface.
//!
//! Source of truth: monoblok at lib/patchbay/src/kernel.zig. Tinyblok
//! vendors a verbatim copy under main/kernel.zig; refresh with
//! `make sync-kernel`.
//!
//! Style: each op is a small struct with default-initialized fields plus
//! one `update` method, mirroring the embedded runtime in
//! tinyblok/main/patchbay.zig. The host calls these via shims that load
//! the struct's fields out of a StateEntry slot, run `update`, and write
//! the fields back. Tinyblok uses the structs directly as fields of its
//! generated `State` aggregate.
//!
//! Snapshot compatibility: nothing here owns persistent storage. The host
//! continues to serialize StateEntry slots; the kernel only describes the
//! transition function applied to those slots.

/// `(squelch X)` (numeric form). Passes X through iff it differs from
/// the last X seen. First sight always passes. Returns nil on suppress.
///
/// The host's full `squelch` accepts any value type and compares
/// canonically-encoded bytes (so `1` and `1.0` are equal); this struct
/// is the f64 specialization the codegen target uses.
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

/// `(deadband DELTA X)`. Passes X through iff it differs from the last
/// accepted X by >= threshold. Suppresses sub-threshold noise. First
/// sight passes.
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

/// `(changed? X)` (numeric form). Boolean version of `squelch`; first
/// sight returns true.
pub const Changed = struct {
    last: f64 = 0,
    seen: bool = false,

    pub fn update(self: *Changed, x: f64) bool {
        if (!self.seen) {
            self.seen = true;
            self.last = x;
            return true;
        }
        if (x == self.last) return false;
        self.last = x;
        return true;
    }
};

/// `(delta X)`. X minus the last X. First sight returns 0.
pub const Delta = struct {
    last: f64 = 0,
    seen: bool = false,

    pub fn update(self: *Delta, x: f64) f64 {
        if (!self.seen) {
            self.seen = true;
            self.last = x;
            return 0;
        }
        const d = x - self.last;
        self.last = x;
        return d;
    }
};

/// `(rising-edge COND)`. Fires once on false→true. First sight never
/// fires. The struct stores the last seen boolean as 0/1 so it round-
/// trips through a numeric state slot unchanged.
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

/// `(falling-edge COND)`. Fires once on true→false. First sight never
/// fires.
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

/// `(transition COND)`. Three-valued edge detector: returns .rising on
/// false→true, .falling on true→false, .none on first sight or no
/// change. The host wraps this to evaluate one of two branches; tinyblok
/// can use it directly to dispatch.
pub const Transition = struct {
    pub const Edge = enum { none, rising, falling };

    last: bool = false,
    seen: bool = false,

    pub fn update(self: *Transition, cond: bool) Edge {
        if (!self.seen) {
            self.seen = true;
            self.last = cond;
            return .none;
        }
        const prev = self.last;
        self.last = cond;
        if (!prev and cond) return .rising;
        if (prev and !cond) return .falling;
        return .none;
    }
};

/// `(hold-off MS X)`. Passes X on first sight and on any call arriving
/// >= `interval_ms` after the previous pass; nil otherwise. Caller
/// supplies `now_ms` (so the runtime stays clock-source-agnostic).
///
/// This is the time-source-injected sibling of tinyblok's `Throttle`
/// (which uses microseconds and `now_us`); the field name is
/// `last_ms` here to match the host's `ctx.now_ms` units.
pub const HoldOff = struct {
    interval_ms: f64,
    last_ms: f64 = 0,
    seen: bool = false,

    pub fn update(self: *HoldOff, now_ms: f64) bool {
        if (!self.seen) {
            self.seen = true;
            self.last_ms = now_ms;
            return true;
        }
        if (now_ms - self.last_ms < self.interval_ms) return false;
        self.last_ms = now_ms;
        return true;
    }
};

/// `(bar! N X)` and `(bar! :ms N X)` — OHLC bar. `cap != 0` selects tick
/// mode, `window_ms != 0` selects time mode. Field layout matches the
/// host's `state.Ohlc` exactly so a host slot can be reinterpreted as
/// `*Bar` without a copy.
pub const Bar = struct {
    open: f64 = 0,
    high: f64 = 0,
    low: f64 = 0,
    count: u32 = 0,
    cap: u32 = 0,
    window_ms: u64 = 0,
    window_start_ms: i64 = 0,
    last_close: f64 = 0,

    pub const Close = struct {
        open: f64,
        high: f64,
        low: f64,
        close: f64,
    };

    pub fn tickUpdate(self: *Bar, x: f64) ?Close {
        if (self.count == 0) {
            self.open = x;
            self.high = x;
            self.low = x;
            self.count = 1;
        } else {
            if (x > self.high) self.high = x;
            if (x < self.low) self.low = x;
            self.count += 1;
        }
        if (self.count < self.cap) return null;
        const out: Close = .{ .open = self.open, .high = self.high, .low = self.low, .close = x };
        self.count = 0;
        return out;
    }

    pub fn timeUpdate(self: *Bar, now_ms: i64, x: f64) ?Close {
        const w: i64 = @intCast(self.window_ms);
        const aligned = @divFloor(now_ms, w) * w;

        if (self.count == 0) {
            self.open = x;
            self.high = x;
            self.low = x;
            self.count = 1;
            self.window_start_ms = aligned;
            self.last_close = x;
            return null;
        }

        if (self.window_start_ms != aligned) {
            const out: Close = .{
                .open = self.open, .high = self.high, .low = self.low, .close = self.last_close,
            };
            self.open = x;
            self.high = x;
            self.low = x;
            self.count = 1;
            self.window_start_ms = aligned;
            self.last_close = x;
            return out;
        }

        if (x > self.high) self.high = x;
        if (x < self.low) self.low = x;
        self.count += 1;
        self.last_close = x;
        return null;
    }

    pub fn timeTick(self: *Bar, now_ms: i64) ?Close {
        if (self.window_ms == 0) return null;
        if (self.count == 0) return null;
        const w: i64 = @intCast(self.window_ms);
        if (now_ms - self.window_start_ms < w) return null;
        const out: Close = .{
            .open = self.open, .high = self.high, .low = self.low, .close = self.last_close,
        };
        self.count = 0;
        self.window_start_ms = 0;
        return out;
    }
};

/// Tick-window moving average over N samples. Emits running mean every
/// call. Allocator-free: storage is comptime-sized.
///
/// Host equivalent for runtime-N is `state.Ring`, which also tracks
/// monotonic deques for max/min; this struct is the avg-only specialization
/// codegen uses on the MCU.
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

/// Tick-window running sum over N samples.
pub fn MovingSum(comptime N: usize) type {
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
            return self.sum;
        }
    };
}

/// Monotonic-deque sliding extremum over N samples. `cmp(a, b) = true` means
/// `a` dominates `b` (i.e. a stays in the deque, b is popped). For max use
/// `a > b`, for min use `a < b`. The deque holds logical sample counters; we
/// reconstruct buffer positions via `% N`.
fn MovingExtremum(comptime N: usize, comptime cmp: fn (f64, f64) bool) type {
    return struct {
        const Self = @This();
        buf: [N]f64 = [_]f64{0} ** N,
        deque: [N]u32 = [_]u32{0} ** N,
        deque_head: u32 = 0,
        deque_len: u32 = 0,
        counter: u32 = 0,

        pub fn update(self: *Self, x: f64) f64 {
            const cap: u32 = @intCast(N);
            const idx = self.counter;

            if (self.counter >= cap) {
                const evict = self.counter - cap;
                if (self.deque_len > 0 and self.deque[self.deque_head] == evict) {
                    self.deque_head = (self.deque_head + 1) % cap;
                    self.deque_len -= 1;
                }
            }
            self.buf[idx % cap] = x;

            while (self.deque_len > 0) {
                const tail_pos = (self.deque_head + self.deque_len - 1) % cap;
                const tail_idx = self.deque[tail_pos];
                const tail_val = self.buf[tail_idx % cap];
                if (cmp(x, tail_val) or x == tail_val) {
                    self.deque_len -= 1;
                } else break;
            }
            const insert_pos = (self.deque_head + self.deque_len) % cap;
            self.deque[insert_pos] = idx;
            self.deque_len += 1;

            self.counter = idx + 1;
            return self.buf[self.deque[self.deque_head] % cap];
        }
    };
}

fn gtFloat(a: f64, b: f64) bool { return a > b; }
fn ltFloat(a: f64, b: f64) bool { return a < b; }

pub fn MovingMax(comptime N: usize) type {
    return MovingExtremum(N, gtFloat);
}

pub fn MovingMin(comptime N: usize) type {
    return MovingExtremum(N, ltFloat);
}

/// `(count!)` / `(count! COND)`. Running u64 counter incremented each call
/// (or only when `inc = true` for the conditional form). Returns the new
/// total. Wrap-around at u64 max is undefined-but-not-UB; if you care,
/// guard upstream.
pub const Count = struct {
    n: u64 = 0,

    pub fn update(self: *Count, inc: bool) ?u64 {
        if (!inc) return null;
        self.n += 1;
        return self.n;
    }
};

/// `(quantize STEP X)`. Snap X to the nearest multiple of STEP. Caller
/// guards `step != 0`.
pub fn quantize(step: f64, x: f64) f64 {
    return @round(x / step) * step;
}

/// `(clamp LO HI X)`. Caller guards `lo <= hi`.
pub fn clamp(lo: f64, hi: f64, x: f64) f64 {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

test {
    const std = @import("std");

    {
        var sq: Squelch = .{};
        try std.testing.expectEqual(@as(?f64, 1), sq.update(1));
        try std.testing.expectEqual(@as(?f64, null), sq.update(1));
        try std.testing.expectEqual(@as(?f64, 2), sq.update(2));
    }

    {
        var db: Deadband = .{ .threshold = 0.5 };
        try std.testing.expectEqual(@as(?f64, 10), db.update(10));
        try std.testing.expectEqual(@as(?f64, null), db.update(10.2));
        try std.testing.expectEqual(@as(?f64, 10.6), db.update(10.6));
    }

    {
        var c: Changed = .{};
        try std.testing.expect(c.update(1));
        try std.testing.expect(!c.update(1));
        try std.testing.expect(c.update(2));
    }

    {
        var d: Delta = .{};
        try std.testing.expectEqual(@as(f64, 0), d.update(10));
        try std.testing.expectEqual(@as(f64, 5), d.update(15));
        try std.testing.expectEqual(@as(f64, -3), d.update(12));
    }

    {
        var re: RisingEdge = .{};
        try std.testing.expect(!re.update(false));
        try std.testing.expect(!re.update(false));
        try std.testing.expect(re.update(true));
        try std.testing.expect(!re.update(true));
        try std.testing.expect(!re.update(false));
        try std.testing.expect(re.update(true));
    }

    {
        var fe: FallingEdge = .{};
        try std.testing.expect(!fe.update(true));
        try std.testing.expect(fe.update(false));
        try std.testing.expect(!fe.update(false));
        try std.testing.expect(fe.update(false) == false); // re-asserts no fire
    }

    {
        var tr: Transition = .{};
        try std.testing.expectEqual(Transition.Edge.none, tr.update(false));
        try std.testing.expectEqual(Transition.Edge.rising, tr.update(true));
        try std.testing.expectEqual(Transition.Edge.none, tr.update(true));
        try std.testing.expectEqual(Transition.Edge.falling, tr.update(false));
    }

    {
        var ho: HoldOff = .{ .interval_ms = 100 };
        try std.testing.expect(ho.update(0));
        try std.testing.expect(!ho.update(50));
        try std.testing.expect(!ho.update(99));
        try std.testing.expect(ho.update(100));
        try std.testing.expect(!ho.update(150));
    }

    {
        var ma = MovingAvg(3){};
        try std.testing.expectEqual(@as(f64, 1), ma.update(1));
        try std.testing.expectEqual(@as(f64, 1.5), ma.update(2));
        try std.testing.expectEqual(@as(f64, 2), ma.update(3));
        try std.testing.expectEqual(@as(f64, 3), ma.update(4));
    }

    {
        var ms = MovingSum(3){};
        try std.testing.expectEqual(@as(f64, 1), ms.update(1));
        try std.testing.expectEqual(@as(f64, 3), ms.update(2));
        try std.testing.expectEqual(@as(f64, 6), ms.update(3));
        try std.testing.expectEqual(@as(f64, 9), ms.update(4));
        try std.testing.expectEqual(@as(f64, 12), ms.update(5));
    }

    {
        var mx = MovingMax(3){};
        try std.testing.expectEqual(@as(f64, 5), mx.update(5));
        try std.testing.expectEqual(@as(f64, 5), mx.update(3));
        try std.testing.expectEqual(@as(f64, 5), mx.update(4));
        try std.testing.expectEqual(@as(f64, 4), mx.update(2));
        try std.testing.expectEqual(@as(f64, 4), mx.update(1));
        try std.testing.expectEqual(@as(f64, 9), mx.update(9));
        try std.testing.expectEqual(@as(f64, 9), mx.update(0));
        try std.testing.expectEqual(@as(f64, 9), mx.update(0));
        try std.testing.expectEqual(@as(f64, 0), mx.update(0));
    }

    {
        var mn = MovingMin(3){};
        try std.testing.expectEqual(@as(f64, 5), mn.update(5));
        try std.testing.expectEqual(@as(f64, 3), mn.update(3));
        try std.testing.expectEqual(@as(f64, 3), mn.update(4));
        try std.testing.expectEqual(@as(f64, 2), mn.update(2));
        try std.testing.expectEqual(@as(f64, 2), mn.update(8));
        try std.testing.expectEqual(@as(f64, 2), mn.update(7));
        try std.testing.expectEqual(@as(f64, 7), mn.update(9));
    }

    {
        var c: Count = .{};
        try std.testing.expectEqual(@as(?u64, 1), c.update(true));
        try std.testing.expectEqual(@as(?u64, 2), c.update(true));
        try std.testing.expectEqual(@as(?u64, null), c.update(false));
        try std.testing.expectEqual(@as(?u64, 3), c.update(true));
    }

    {
        try std.testing.expectEqual(@as(f64, 10), quantize(5, 11));
        try std.testing.expectEqual(@as(f64, 10), quantize(5, 12.4));
        try std.testing.expectEqual(@as(f64, 15), quantize(5, 12.6));
        try std.testing.expectEqual(@as(f64, 0.1), quantize(0.1, 0.13));
    }

    {
        try std.testing.expectEqual(@as(f64, 0), clamp(0, 10, -5));
        try std.testing.expectEqual(@as(f64, 10), clamp(0, 10, 15));
        try std.testing.expectEqual(@as(f64, 5), clamp(0, 10, 5));
    }

    {
        var b: Bar = .{ .cap = 3 };
        try std.testing.expect(b.tickUpdate(10) == null);
        try std.testing.expect(b.tickUpdate(15) == null);
        const c = b.tickUpdate(12).?;
        try std.testing.expectEqual(@as(f64, 10), c.open);
        try std.testing.expectEqual(@as(f64, 15), c.high);
        try std.testing.expectEqual(@as(f64, 10), c.low);
        try std.testing.expectEqual(@as(f64, 12), c.close);
        try std.testing.expect(b.tickUpdate(20) == null);
    }

    {
        var b: Bar = .{ .window_ms = 1000 };
        try std.testing.expect(b.timeUpdate(0, 10) == null);
        try std.testing.expect(b.timeUpdate(500, 15) == null);
        try std.testing.expect(b.timeUpdate(900, 8) == null);
        const c = b.timeUpdate(1000, 12).?;
        try std.testing.expectEqual(@as(f64, 10), c.open);
        try std.testing.expectEqual(@as(f64, 15), c.high);
        try std.testing.expectEqual(@as(f64, 8), c.low);
        try std.testing.expectEqual(@as(f64, 8), c.close);
    }

    {
        var b: Bar = .{ .window_ms = 1000 };
        _ = b.timeUpdate(100, 10);
        _ = b.timeUpdate(500, 20);
        try std.testing.expect(b.timeTick(900) == null);
        const c = b.timeTick(1100).?;
        try std.testing.expectEqual(@as(f64, 10), c.open);
        try std.testing.expectEqual(@as(f64, 20), c.high);
        try std.testing.expectEqual(@as(f64, 10), c.low);
        try std.testing.expectEqual(@as(f64, 20), c.close);
        try std.testing.expect(b.timeTick(1200) == null);
        try std.testing.expect(b.timeUpdate(1300, 5) == null);
    }
}
