// Single-producer, single-consumer ring of NATS PUB records, drained into the
// non-blocking socket once per tick. Producer = rule eval on the loop thread
// (via patchbay.emit). Consumer = same thread. No locks, no atomics —
// single-threaded by construction.
//
// We store records, not framed bytes. Each record is:
//
//   [u16 subject_len][u16 payload_len][subject_len bytes][payload_len bytes]
//
// The PUB header ("PUB <subject> <payload_len>\r\n") and trailing "\r\n" are
// built on the stack at drain time. Subject bytes are copied into the ring at
// enqueue time so callers don't have to manage subject lifetime — the ring is
// fully self-contained.
//
// Drop-oldest on full: if a record won't fit, evict records starting from the
// tail until it does. Right policy for live telemetry — broker comes back to a
// catch-up of recent samples rather than 30 sec of stale data and nothing
// current. The one in-progress record (record_bytes_sent > 0) is pinned and
// never evicted, otherwise the broker would receive a truncated PUB followed
// by a fresh one and close on the protocol violation.
//
// A record is never split across the wrap boundary: if it doesn't fit in the
// contiguous tail-to-end region, head jumps to 0 and the tail bytes become
// "slack" — accounted for separately so used_bytes only counts real records.

extern fn snprintf(buf: [*]u8, len: usize, fmt: [*:0]const u8, ...) c_int;

pub const RING_CAP: usize = 8192;
pub const SUBJ_MAX: usize = 64;
pub const PAYLOAD_MAX: usize = 64;

const HEADER_BYTES: usize = @sizeOf(u16) + @sizeOf(u16);

var buf: [RING_CAP]u8 = undefined;
var head: usize = 0;
var tail: usize = 0;
var used_bytes: usize = 0; // record bytes only (excludes slack)
var slack_at_end: usize = 0; // unused tail region created by wrap-around

// Bytes of the current head record already pushed to the socket. Non-zero
// means the head record is pinned (eviction would corrupt the wire stream).
var record_bytes_sent: usize = 0;

pub var dropped: u32 = 0;

pub fn used() usize {
    return used_bytes;
}

pub fn capacity() usize {
    return RING_CAP;
}

export fn tinyblok_tx_ring_used() callconv(.c) usize {
    return used_bytes;
}

export fn tinyblok_tx_ring_count() callconv(.c) usize {
    return count();
}

/// Caller (the NATS layer) invokes this after a socket teardown so the next
/// drain restarts the head record from byte 0 instead of resuming mid-record
/// into a fresh connection (which the broker would parse as garbage).
export fn tinyblok_tx_ring_reset_in_flight() callconv(.c) void {
    record_bytes_sent = 0;
}

/// Number of records currently buffered. Walks the ring; cheap (cap is 8 KiB).
pub fn count() usize {
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

/// Free space available without eviction. Equals contiguous space at head.
fn freeContiguous() usize {
    if (used_bytes == 0) {
        return RING_CAP;
    }
    if (head > tail) {
        return RING_CAP - head; // tail-to-end region; remaining `tail` bytes
        //                         require wrapping head to 0
    }
    // head <= tail and we have data → wrapped layout
    return tail - head;
}

/// Append a record. Evicts oldest records to make room if necessary.
/// Only fails if the record itself is malformed or larger than RING_CAP.
pub fn enqueue(subject: []const u8, payload: []const u8) bool {
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
        // Linear layout: data is [tail .. head), free is [head .. RING_CAP) ∪ [0 .. tail).
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

    // The tail record is the head of the FIFO; if it's mid-send, it's pinned.
    if (record_bytes_sent > 0) return false;

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

/// Drain records into the socket. Stops on EAGAIN (try_send returns 0) or
/// error (negative). Resumes mid-record via record_bytes_sent.
pub fn drain(try_send: TrySendFn) void {
    while (used_bytes > 0) {
        if (slack_at_end > 0 and tail == RING_CAP - slack_at_end) {
            tail = 0;
            slack_at_end = 0;
            if (used_bytes == 0) return;
        }

        const subj_len = readU16(tail);
        const pl_len = readU16(tail + 2);
        const subj_off = tail + HEADER_BYTES;
        const subject = buf[subj_off..][0..subj_len];
        const payload_off = subj_off + subj_len;
        const payload = buf[payload_off..][0..pl_len];
        const rec_size = HEADER_BYTES + subj_len + pl_len;

        var hdr: [SUBJ_MAX + 32]u8 = undefined;
        const hdr_n = snprintf(&hdr, hdr.len, "PUB %.*s %u\r\n", @as(c_int, @intCast(subj_len)), subject.ptr, @as(c_uint, @intCast(pl_len)));
        if (hdr_n <= 0) {
            advance(rec_size);
            record_bytes_sent = 0;
            continue;
        }
        const hdr_len: usize = @min(@as(usize, @intCast(hdr_n)), hdr.len);

        var sent = record_bytes_sent;
        const ok = sendSegment(try_send, hdr[0..hdr_len], &sent, 0) and
            sendSegment(try_send, payload, &sent, hdr_len) and
            sendSegment(try_send, "\r\n", &sent, hdr_len + pl_len);

        if (!ok) {
            record_bytes_sent = sent;
            return;
        }

        advance(rec_size);
        record_bytes_sent = 0;
    }
}

/// Send `slice` accounting for `sent.*` bytes already pushed across the whole
/// record's wire form. Returns false on EAGAIN/error so caller can persist `sent`.
fn sendSegment(try_send: TrySendFn, slice: []const u8, sent: *usize, slice_start: usize) bool {
    if (slice.len == 0) return true;
    if (sent.* >= slice_start + slice.len) return true;
    const skip = if (sent.* > slice_start) sent.* - slice_start else 0;
    var off: usize = skip;
    while (off < slice.len) {
        const n = try_send(slice.ptr + off, slice.len - off);
        if (n <= 0) {
            sent.* = slice_start + off;
            return false;
        }
        off += @intCast(n);
    }
    sent.* = slice_start + slice.len;
    return true;
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
