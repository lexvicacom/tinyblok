// User-owned Zig functions registered from patchbay.edn.
//
// Keep these dependency-free and bounded. Byte transforms receive an input
// slice as (ptr,len), write into the caller-provided output buffer, and return
// the number of bytes written.

const bytes = @import("bytes.zig");

export fn tinyblok_hello_zig(
    payload_ptr: [*]const u8,
    payload_len: usize,
    out_ptr: [*]u8,
    out_len: usize,
) callconv(.c) usize {
    return helloZig(payload_ptr, payload_len, out_ptr, out_len);
}

fn helloZig(
    payload_ptr: [*]const u8,
    payload_len: usize,
    out_ptr: [*]u8,
    out_len: usize,
) usize {
    return bytes.copyPrefixPayload(
        "hello from zig: ",
        payload_ptr[0..payload_len],
        out_ptr[0..out_len],
    );
}
