// tinyblok hello-world (milestone 1).
// Loops printf every second; verifies Zig static lib + IDF link + USB-Serial-JTAG.

extern fn printf(fmt: [*:0]const u8, ...) c_int;
extern fn vTaskDelay(ticks: u32) void;
extern fn tinyblok_read_temp_c() f32;

// C6 internal temp sensor quantizes to 1 °C, so polling faster than 1 Hz just
// gives duplicates. Default IDF tick rate is 100 Hz (10 ms/tick) → 100 ticks = 1 s.
const ticks_per_sample: u32 = 100;

export fn zig_main() callconv(.c) void {
    var counter: u32 = 0;
    while (true) {
        const temp_c: f64 = tinyblok_read_temp_c();
        _ = printf("hello from zig: tick=%lu temp=%.7fC\n", counter, temp_c);
        counter +%= 1;
        vTaskDelay(ticks_per_sample);
    }
}
