#ifndef TINYBLOK_TX_RING_H
#define TINYBLOK_TX_RING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define TINYBLOK_TX_RING_CAP 8192
#define TINYBLOK_SUBJECT_MAX 64
#define TINYBLOK_PAYLOAD_MAX 64

typedef ssize_t (*tinyblok_try_send_fn)(const unsigned char *data, size_t len);

bool tinyblok_tx_ring_enqueue(const uint8_t *subject, size_t subject_len, const uint8_t *payload, size_t payload_len);
void tinyblok_tx_ring_drain(tinyblok_try_send_fn try_send);

size_t tinyblok_tx_ring_used(void);
size_t tinyblok_tx_ring_count(void);
void tinyblok_tx_ring_reset_in_flight(void);

#endif
