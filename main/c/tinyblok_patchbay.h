#ifndef TINYBLOK_PATCHBAY_H
#define TINYBLOK_PATCHBAY_H

#include <stddef.h>
#include <stdint.h>

// Timer source loaded from a top-level `(pump ...)` form. The subject slice is
// owned by the patchbay parse arena and remains valid after init.
typedef struct tinyblok_pump
{
    const char *subject;
    uint64_t period_us;
} tinyblok_pump;

int tinyblok_patchbay_init(void);
void tinyblok_patchbay_main(void);

size_t tinyblok_patchbay_pump_count(void);
const tinyblok_pump *tinyblok_patchbay_pump(size_t index);
void tinyblok_patchbay_fire_pump(size_t index);

size_t tinyblok_patchbay_request_count(void);
const char *tinyblok_patchbay_request_subject(size_t index);
void tinyblok_patchbay_handle_msg(const unsigned char *subject, size_t subject_len,
                                  const unsigned char *reply, size_t reply_len,
                                  const unsigned char *payload, size_t payload_len);

void tinyblok_patchbay_clock_fired(void);
void tinyblok_patchbay_arm_next_clock(void);

#endif
