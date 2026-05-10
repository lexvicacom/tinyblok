pub fn copyPrefixPayload(prefix: []const u8, payload: []const u8, out: []u8) usize {
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
