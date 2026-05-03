// tinyblok hello-world (milestone 1).
// Loops printf every second; verifies Zig static lib + IDF link + USB-Serial-JTAG.

extern fn printf(fmt: [*:0]const u8, ...) c_int;
extern fn vTaskDelay(ticks: u32) void;

// Default IDF tick rate is 100 Hz; 1000 ms = 100 ticks. Revisit if CONFIG_FREERTOS_HZ changes.
const ticks_per_second: u32 = 100;

export fn zig_main() callconv(.c) void {
    var counter: u32 = 0;
    while (true) {
        _ = printf("hello from zig: tick=%lu\n", counter);
        counter +%= 1;
        vTaskDelay(ticks_per_second);
    }
}
