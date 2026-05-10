// User-owned Zig functions used by patchbay exports.
//
// Keep these dependency-free and bounded. Byte transforms receive an input
// slice as (ptr,len), write into the caller-provided output buffer, and return
// the number of bytes written.

pub fn helloZig(
    payload_ptr: [*]const u8,
    payload_len: usize,
    out_ptr: [*]u8,
    out_len: usize,
) usize {
    return copyPrefixPayload(
        "hello from zig: ",
        payload_ptr[0..payload_len],
        out_ptr[0..out_len],
    );
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
