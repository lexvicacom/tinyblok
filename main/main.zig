const pb = @import("patchbay.zig");
const rules = @import("rules.zig");

extern fn vTaskDelay(ticks: u32) void;
extern fn tinyblok_nats_publish(subject: [*:0]const u8, payload: [*]const u8, payload_len: usize) callconv(.c) c_int;

export fn zig_main() callconv(.c) void {
    pb.publisher = tinyblok_nats_publish;
    const ticks_per_iter: u32 = rules.tick_ms / 10; // IDF default 100 Hz tick = 10 ms

    while (true) {
        rules.collect();
        vTaskDelay(ticks_per_iter);
    }
}
