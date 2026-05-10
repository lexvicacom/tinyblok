const tx_ring = @import("tx_ring.zig");

comptime {
    _ = @import("rules.zig");
    _ = @import("user.zig");
}

extern fn vTaskDelay(ticks: u32) void;
extern fn tinyblok_nats_try_send(data: [*]const u8, data_len: usize) callconv(.c) isize;
extern fn tinyblok_nats_drain_rx() callconv(.c) void;
extern fn tinyblok_nats_maintain() callconv(.c) void;

export fn zig_main() callconv(.c) void {
    while (true) {
        tinyblok_nats_maintain(); // Keep NATS and wifi up
        tx_ring.drain(tinyblok_nats_try_send); // transmit anything buffered
        tinyblok_nats_drain_rx(); // recv

        vTaskDelay(10); // 100ms tick
    }
}
