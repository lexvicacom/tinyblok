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
}
