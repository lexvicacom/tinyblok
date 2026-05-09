// User-owned Zig sources for patchbay functions.
//
// Add small dependency-free pump reads or request helpers here, then register
// them from patchbay.edn with:
//
// - (pump SUBJECT :from symbol ...)
// - (fn NAME :from symbol ...)
// - (on-req SUBJECT ...)

export fn tinyblok_user_echo(
    payload_ptr: [*]const u8,
    payload_len: usize,
    out_ptr: [*]u8,
    out_len: usize,
) callconv(.c) usize {
    const n = @min(payload_len, out_len);
    @memcpy(out_ptr[0..n], payload_ptr[0..n]);
    return n;
}
//
// patchbay.edn:
//
// (fn user-echo :from tinyblok_user_echo :input bytes :type bytes)
//
// (on-req "tinyblok.req.user-echo"
//   (reply! (user-echo payload)))
