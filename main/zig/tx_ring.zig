// Ring of NATS PUB records. The esp_event task enqueues; zig_main drains.
// Metadata is protected by tinyblok_tx_ring_{lock,unlock}. Drain copies one
// head record while locked, sends it without the lock, then re-locks to commit.
//
// Records are stored unframed:
//   [u16 subject_len][u16 payload_len][subject_len bytes][payload_len bytes]
//
// The oldest complete record is dropped on overflow. An in-progress send is
// pinned, and records are never split across the wrap boundary.

const std = @import("std");

extern fn snprintf(buf: [*]u8, len: usize, fmt: [*:0]const u8, ...) c_int;
extern fn tinyblok_tx_ring_lock() callconv(.c) void;
extern fn tinyblok_tx_ring_unlock() callconv(.c) void;

pub const RING_CAP: usize = 8192;
pub const SUBJ_MAX: usize = 64;
pub const PAYLOAD_MAX: usize = 64;

const HEADER_BYTES: usize = @sizeOf(u16) + @sizeOf(u16);

var buf: [RING_CAP]u8 = undefined;
var head: usize = 0;
var tail: usize = 0;
var used_bytes: usize = 0; // record bytes only (excludes slack)
var slack_at_end: usize = 0; // unused tail region created by wrap-around

// Bytes of the current head record's NATS frame already pushed to the socket.
// Non-zero means the head record is pinned.
var frame_bytes_sent: usize = 0;
var drain_pinned: bool = false;

pub var dropped: u32 = 0;

pub fn used() usize {
    tinyblok_tx_ring_lock();
    defer tinyblok_tx_ring_unlock();
    return used_bytes;
}

pub fn capacity() usize {
    return RING_CAP;
}

export fn tinyblok_tx_ring_used() callconv(.c) usize {
    tinyblok_tx_ring_lock();
    defer tinyblok_tx_ring_unlock();
    return used_bytes;
}

export fn tinyblok_tx_ring_count() callconv(.c) usize {
    tinyblok_tx_ring_lock();
    defer tinyblok_tx_ring_unlock();
    return countLocked();
}

/// Caller (the NATS layer) invokes this after a socket teardown so the next
/// drain restarts the head record from byte 0 instead of resuming mid-record
/// into a fresh connection (which the broker would parse as garbage).
export fn tinyblok_tx_ring_reset_in_flight() callconv(.c) void {
    tinyblok_tx_ring_lock();
    defer tinyblok_tx_ring_unlock();
    frame_bytes_sent = 0;
}

/// Number of records currently buffered. Walks the ring; cheap (cap is 8 KiB).
pub fn count() usize {
    tinyblok_tx_ring_lock();
    defer tinyblok_tx_ring_unlock();
    return countLocked();
}

fn countLocked() usize {
    if (used_bytes == 0) return 0;
    var t = tail;
    var remaining = used_bytes;
    var n: usize = 0;
    while (remaining > 0) {
        if (slack_at_end > 0 and t == RING_CAP - slack_at_end) {
            t = 0;
        }
        const subj_len = readU16(t);
        const pl_len = readU16(t + 2);
        const rec_size = HEADER_BYTES + subj_len + pl_len;
        t += rec_size;
        if (t >= RING_CAP) t -= RING_CAP;
        remaining -= rec_size;
        n += 1;
    }
    return n;
}

/// Append a record. Evicts oldest records to make room if necessary.
/// Only fails if the record itself is malformed or larger than RING_CAP.
pub fn enqueue(subject: []const u8, payload: []const u8) bool {
    tinyblok_tx_ring_lock();
    defer tinyblok_tx_ring_unlock();

    if (subject.len == 0 or subject.len > SUBJ_MAX) {
        dropped += 1;
        return false;
    }
    if (payload.len > PAYLOAD_MAX) {
        dropped += 1;
        return false;
    }

    const need = HEADER_BYTES + subject.len + payload.len;
    if (need > RING_CAP) {
        dropped += 1;
        return false;
    }

    // Make room by evicting records from the tail. Each iteration drops one
    // record (or reclaims slack). Stop when the record fits contiguously.
    while (true) {
        if (tryWrite(subject, payload, need)) return true;
        if (!evictOne()) {
            // Can't evict (tail record is pinned mid-send and is the only
            // one). Drop this record rather than corrupt the stream.
            dropped += 1;
            return false;
        }
    }
}

/// Try to place the record. Returns true on success.
fn tryWrite(subject: []const u8, payload: []const u8, need: usize) bool {
    if (used_bytes == 0) {
        // Reset to start of buffer to maximize contiguous space.
        head = 0;
        tail = 0;
        slack_at_end = 0;
        if (need > RING_CAP) return false;
        writeRecord(0, subject, payload);
        head = need;
        used_bytes = need;
        return true;
    }

    if (head > tail) {
        // Linear layout: data is [tail .. head), free is [head .. RING_CAP) plus [0 .. tail).
        const tail_to_end = RING_CAP - head;
        if (need <= tail_to_end) {
            writeRecord(head, subject, payload);
            head += need;
            if (head == RING_CAP) head = 0;
            used_bytes += need;
            return true;
        }
        // Wrap head to 0; need strictly fewer bytes than tail (head==tail means full).
        if (need < tail) {
            slack_at_end = tail_to_end;
            writeRecord(0, subject, payload);
            head = need;
            used_bytes += need;
            return true;
        }
        return false;
    } else {
        // head <= tail with data: wrapped layout, free is [head .. tail).
        // (head == tail with used_bytes > 0 means full.)
        const gap = if (tail > head) tail - head else 0;
        if (need < gap) { // strict: avoid head catching up to tail
            writeRecord(head, subject, payload);
            head += need;
            used_bytes += need;
            return true;
        }
        return false;
    }
}

/// Drop the oldest record (advance tail). Returns false if nothing can be
/// evicted (only the in-progress head record remains).
fn evictOne() bool {
    if (used_bytes == 0) return false;

    // Reclaim end-of-buffer slack if tail sits on it.
    if (slack_at_end > 0 and tail == RING_CAP - slack_at_end) {
        tail = 0;
        slack_at_end = 0;
        if (used_bytes == 0) return true;
    }

    // The tail record is the head of the FIFO; if drain has copied it or sent
    // part of it, it is pinned until drain commits progress.
    if (drain_pinned or frame_bytes_sent > 0) return false;

    const subj_len = readU16(tail);
    const pl_len = readU16(tail + 2);
    const rec_size = HEADER_BYTES + subj_len + pl_len;
    tail += rec_size;
    used_bytes -= rec_size;
    if (tail == RING_CAP - slack_at_end and slack_at_end > 0) {
        tail = 0;
        slack_at_end = 0;
    } else if (tail >= RING_CAP) {
        tail -= RING_CAP;
    }
    dropped += 1;
    return true;
}

fn writeRecord(off: usize, subject: []const u8, payload: []const u8) void {
    var p = off;
    writeU16(p, @intCast(subject.len));
    p += 2;
    writeU16(p, @intCast(payload.len));
    p += 2;
    if (subject.len > 0) {
        @memcpy(buf[p..][0..subject.len], subject);
        p += subject.len;
    }
    if (payload.len > 0) {
        @memcpy(buf[p..][0..payload.len], payload);
    }
}

inline fn writeU16(off: usize, v: u16) void {
    buf[off] = @intCast(v & 0xff);
    buf[off + 1] = @intCast((v >> 8) & 0xff);
}

inline fn readU16(off: usize) u16 {
    return @as(u16, buf[off]) | (@as(u16, buf[off + 1]) << 8);
}

pub const TrySendFn = *const fn (data: [*]const u8, data_len: usize) callconv(.c) isize;

const DrainRecord = struct {
    subject: [SUBJ_MAX]u8 = undefined,
    payload: [PAYLOAD_MAX]u8 = undefined,
    subject_len: usize = 0,
    payload_len: usize = 0,
    rec_size: usize = 0,
    sent: usize = 0,
};

const SendStatus = enum { complete, blocked, err };

/// Host-side drain used by the native soundcheck CLI. Firmware drains records
/// as NATS PUB frames; this emits the stored record shape directly.
pub fn drainDelimited(writer: anytype) !void {
    try drainDelimitedLabeled(writer, null, null);
}

/// Same as drainDelimited, with optional leading labels. `first_label` is used
/// for the first record drained in this call; `rest_label` is used afterward.
pub fn drainDelimitedLabeled(writer: anytype, first_label: ?[]const u8, rest_label: ?[]const u8) !void {
    var first = true;
    while (true) {
        var rec: DrainRecord = undefined;

        {
            tinyblok_tx_ring_lock();
            defer tinyblok_tx_ring_unlock();
            if (!copyHeadForDrain(&rec)) return;
            drain_pinned = false;
            advance(rec.rec_size);
            frame_bytes_sent = 0;
        }

        const label = if (first) first_label orelse rest_label else rest_label;
        first = false;
        if (label) |l| {
            try writer.writeAll(l);
            try writer.writeByte('|');
        }
        try writer.writeAll(rec.subject[0..rec.subject_len]);
        try writer.writeByte('|');
        try writer.writeAll(rec.payload[0..rec.payload_len]);
        try writer.writeByte('\n');
    }
}

/// Drain records into the socket. Stops on EAGAIN (try_send returns 0) or
/// error (negative). Resumes mid-frame via frame_bytes_sent.
pub fn drain(try_send: TrySendFn) void {
    while (true) {
        var rec: DrainRecord = undefined;

        {
            tinyblok_tx_ring_lock();
            defer tinyblok_tx_ring_unlock();
            if (!copyHeadForDrain(&rec)) return;
        }

        var hdr: [SUBJ_MAX + 32]u8 = undefined;
        const subject = rec.subject[0..rec.subject_len];
        const payload = rec.payload[0..rec.payload_len];
        const hdr_n = snprintf(&hdr, hdr.len, "PUB %.*s %u\r\n", @as(c_int, @intCast(rec.subject_len)), subject.ptr, @as(c_uint, @intCast(rec.payload_len)));
        if (hdr_n <= 0) {
            dropCopiedRecord(rec.rec_size);
            continue;
        }
        const hdr_len: usize = @intCast(hdr_n);
        if (hdr_len >= hdr.len) {
            dropCopiedRecord(rec.rec_size);
            continue;
        }

        var sent = rec.sent;
        var status = sendSegment(try_send, hdr[0..hdr_len], &sent, 0);
        if (status == .complete) status = sendSegment(try_send, payload, &sent, hdr_len);
        if (status == .complete) status = sendSegment(try_send, "\r\n", &sent, hdr_len + rec.payload_len);

        tinyblok_tx_ring_lock();
        defer tinyblok_tx_ring_unlock();
        drain_pinned = false;
        switch (status) {
            .complete => {
                advance(rec.rec_size);
                frame_bytes_sent = 0;
            },
            .blocked => {
                frame_bytes_sent = sent;
                return;
            },
            .err => {
                frame_bytes_sent = 0;
                return;
            },
        }
    }
}

fn copyHeadForDrain(out: *DrainRecord) bool {
    if (used_bytes == 0) return false;
    normalizeTailSlack();
    if (used_bytes == 0) return false;

    const subj_len = readU16(tail);
    const pl_len = readU16(tail + 2);
    const rec_size = HEADER_BYTES + subj_len + pl_len;

    if (subj_len > SUBJ_MAX or pl_len > PAYLOAD_MAX or rec_size > used_bytes) {
        dropped += 1;
        advance(@min(rec_size, used_bytes));
        frame_bytes_sent = 0;
        drain_pinned = false;
        return false;
    }

    const subj_off = tail + HEADER_BYTES;
    const payload_off = subj_off + subj_len;
    @memcpy(out.subject[0..subj_len], buf[subj_off..][0..subj_len]);
    @memcpy(out.payload[0..pl_len], buf[payload_off..][0..pl_len]);
    out.subject_len = subj_len;
    out.payload_len = pl_len;
    out.rec_size = rec_size;
    out.sent = frame_bytes_sent;
    drain_pinned = true;
    return true;
}

fn normalizeTailSlack() void {
    if (slack_at_end > 0 and tail == RING_CAP - slack_at_end) {
        tail = 0;
        slack_at_end = 0;
    }
}

fn dropCopiedRecord(rec_size: usize) void {
    tinyblok_tx_ring_lock();
    defer tinyblok_tx_ring_unlock();
    advance(rec_size);
    frame_bytes_sent = 0;
    drain_pinned = false;
    dropped += 1;
}

/// Send `slice` accounting for `sent.*` bytes already pushed across the whole
/// record's wire form.
fn sendSegment(try_send: TrySendFn, slice: []const u8, sent: *usize, slice_start: usize) SendStatus {
    if (slice.len == 0) return .complete;
    if (sent.* >= slice_start + slice.len) return .complete;
    const skip = if (sent.* > slice_start) sent.* - slice_start else 0;
    var off: usize = skip;
    while (off < slice.len) {
        const n = try_send(slice.ptr + off, slice.len - off);
        if (n < 0) {
            sent.* = slice_start + off;
            return .err;
        }
        if (n == 0) {
            sent.* = slice_start + off;
            return .blocked;
        }
        off += @intCast(n);
    }
    sent.* = slice_start + slice.len;
    return .complete;
}

fn advance(n: usize) void {
    tail += n;
    used_bytes -= n;
    if (tail == RING_CAP - slack_at_end and slack_at_end > 0) {
        tail = 0;
        slack_at_end = 0;
    } else if (tail >= RING_CAP) {
        tail -= RING_CAP;
    }
}

fn testReset() void {
    head = 0;
    tail = 0;
    used_bytes = 0;
    slack_at_end = 0;
    frame_bytes_sent = 0;
    drain_pinned = false;
    dropped = 0;
}

fn testDropOne() void {
    var rec: DrainRecord = undefined;
    if (copyHeadForDrain(&rec)) {
        drain_pinned = false;
        advance(rec.rec_size);
        frame_bytes_sent = 0;
    }
}

var test_send_cap: usize = 0;
var test_send_buf: [RING_CAP * 2]u8 = undefined;
var test_send_len: usize = 0;

fn testSendReset(cap: usize) void {
    test_send_cap = cap;
    test_send_len = 0;
}

fn testSendLimited(data: [*]const u8, data_len: usize) callconv(.c) isize {
    if (test_send_cap == 0) return 0;
    const n = @min(data_len, test_send_cap);
    if (test_send_len + n > test_send_buf.len) return -1;
    @memcpy(test_send_buf[test_send_len..][0..n], data[0..n]);
    test_send_len += n;
    test_send_cap -= n;
    return @intCast(n);
}

fn testSendErr(_: [*]const u8, _: usize) callconv(.c) isize {
    return -1;
}

test "tx ring rejects invalid record bounds" {
    testReset();

    try std.testing.expect(!enqueue("", "x"));
    try std.testing.expectEqual(@as(u32, 1), dropped);

    var subject_too_long: [SUBJ_MAX + 1]u8 = undefined;
    @memset(&subject_too_long, 's');
    try std.testing.expect(!enqueue(&subject_too_long, "x"));
    try std.testing.expectEqual(@as(u32, 2), dropped);

    var payload_too_long: [PAYLOAD_MAX + 1]u8 = undefined;
    @memset(&payload_too_long, 'p');
    try std.testing.expect(!enqueue("s", &payload_too_long));
    try std.testing.expectEqual(@as(u32, 3), dropped);

    var subject_max: [SUBJ_MAX]u8 = undefined;
    var payload_max: [PAYLOAD_MAX]u8 = undefined;
    @memset(&subject_max, 's');
    @memset(&payload_max, 'p');
    try std.testing.expect(enqueue(&subject_max, &payload_max));
    try std.testing.expectEqual(@as(usize, 1), count());
    try std.testing.expect(used() <= capacity());
}

test "tx ring evicts oldest records when full" {
    testReset();

    var payload: [PAYLOAD_MAX]u8 = undefined;
    @memset(&payload, 'x');
    var inserted: usize = 0;
    while (inserted < 200) : (inserted += 1) {
        payload[0] = @intCast('A' + (inserted % 26));
        try std.testing.expect(enqueue("s", &payload));
    }

    try std.testing.expect(dropped > 0);
    try std.testing.expect(count() < inserted);
    try std.testing.expect(used() <= capacity());

    var rec: DrainRecord = undefined;
    try std.testing.expect(copyHeadForDrain(&rec));
    drain_pinned = false;
    try std.testing.expect(rec.payload[0] != 'A');
}

test "tx ring resumes a partial NATS frame" {
    testReset();
    try std.testing.expect(enqueue("s", "abc"));

    testSendReset(8);
    drain(&testSendLimited);
    try std.testing.expectEqual(@as(usize, 1), count());
    try std.testing.expectEqual(@as(usize, 8), frame_bytes_sent);

    test_send_cap = 64;
    drain(&testSendLimited);
    try std.testing.expectEqual(@as(usize, 0), count());
    try std.testing.expectEqualStrings("PUB s 3\r\nabc\r\n", test_send_buf[0..test_send_len]);
}

test "tx ring can restart an in-flight record after reconnect" {
    testReset();
    try std.testing.expect(enqueue("s", "abc"));

    testSendReset(8);
    drain(&testSendLimited);
    tinyblok_tx_ring_reset_in_flight();

    testSendReset(64);
    drain(&testSendLimited);
    try std.testing.expectEqual(@as(usize, 0), count());
    try std.testing.expectEqualStrings("PUB s 3\r\nabc\r\n", test_send_buf[0..test_send_len]);
}

test "tx ring keeps a record queued after send error" {
    testReset();
    try std.testing.expect(enqueue("s", "abc"));

    drain(&testSendErr);
    try std.testing.expectEqual(@as(usize, 1), count());
    try std.testing.expectEqual(@as(usize, 0), frame_bytes_sent);
}

test "tx ring handles wraparound slack" {
    testReset();

    var payload: [PAYLOAD_MAX]u8 = undefined;
    @memset(&payload, 'x');
    for (0..80) |_| {
        try std.testing.expect(enqueue("s", &payload));
    }
    for (0..40) |_| testDropOne();

    for (0..80) |_| {
        try std.testing.expect(enqueue("s", &payload));
    }

    try std.testing.expect(slack_at_end > 0);
    try std.testing.expect(used() <= capacity());
    try std.testing.expect(count() > 0);

    testSendReset(100_000);
    drain(&testSendLimited);
    try std.testing.expectEqual(@as(usize, 0), count());
}
