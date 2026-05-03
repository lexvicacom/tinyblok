const rules = @import("rules.zig");
const tx_ring = @import("tx_ring.zig");

extern fn vTaskDelay(ticks: u32) void;
extern fn tinyblok_nats_try_send(data: [*]const u8, data_len: usize) callconv(.c) isize;
extern fn tinyblok_nats_drain_rx() callconv(.c) void;
extern fn tinyblok_nats_maintain() callconv(.c) void;

export fn zig_main() callconv(.c) void {
    const ticks_per_iter: u32 = rules.tick_ms / 10;

    while (true) {
        rules.collect();
        tinyblok_nats_maintain();
        tx_ring.drain(tinyblok_nats_try_send);
        tinyblok_nats_drain_rx();
        vTaskDelay(ticks_per_iter);
    }
}
