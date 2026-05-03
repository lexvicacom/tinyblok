// Embedded patchbay runtime. No IDF deps, no allocator, no globals.
// Each stateful op is a struct with fixed-capacity state owned by the caller.
// `publish!` is an indirection (Publisher fn pointer) so codegen-emitted rule
// files can be retargeted: in firmware it points at tinyblok_nats_publish; in
// host tests it can point at a buffer.
//
// Codegen target shape: walk `(on FILTER (-> X op1 op2 (publish! ...)))`,
// emit a Zig function that threads X through `state.opN.update(x)`,
// short-circuits on null, ends with `publisher(subject, payload, len)`.

// Float formatting goes through libc's snprintf rather than std.fmt — the latter
// pulls in __udivti3 (128-bit divide) which compiler-rt for riscv32-freestanding
// doesn't provide. snprintf is already linked by IDF.
extern fn snprintf(buf: [*]u8, len: usize, fmt: [*:0]const u8, ...) c_int;

pub const Publisher = *const fn (subject: [*:0]const u8, payload: [*]const u8, payload_len: usize) callconv(.c) c_int;

// Set once at startup; rules call this for every `publish!`.
pub var publisher: ?Publisher = null;

pub fn emit(subject: [*:0]const u8, payload: []const u8) void {
    const p = publisher orelse return;
    _ = p(subject, payload.ptr, payload.len);
}

// Format a float and emit. Matches the `(publish! VALUE)` shape where VALUE
// is a number that needs serializing back to text for the NATS wire.
pub fn emitFloat(subject: [*:0]const u8, value: f64, decimals: u8) void {
    var buf: [32]u8 = undefined;
    // %.*f: precision from arg list. Zig variadic call needs concrete c_int.
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

// --- stateful ops -----------------------------------------------------------
//
// Each op is a struct with .update(x) returning ?T. null = suppress, T = pass.
// Convention matches monoblok: gates pass the value through on success, return
// null on suppress. `publish!` is a no-op on null (caller checks).

/// Pass value through if |v - last_emitted| >= threshold. First sight always emits.
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

/// Append ".suffix" to base into a fixed buffer, NUL-terminate. Returns pointer
/// suitable for the C Publisher signature. Buffer must outlive the emit call.
pub fn subjectAppend(buf: []u8, base: []const u8, suffix: []const u8) ?[*:0]const u8 {
    if (base.len + 1 + suffix.len + 1 > buf.len) return null;
    @memcpy(buf[0..base.len], base);
    buf[base.len] = '.';
    @memcpy(buf[base.len + 1 ..][0..suffix.len], suffix);
    buf[base.len + 1 + suffix.len] = 0;
    return @ptrCast(buf.ptr);
}
