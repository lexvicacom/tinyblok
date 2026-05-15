#include "tinyblok_tx_ring.h"

#include <stdio.h>
#include <string.h>

#include "app_events.h"

#define HEADER_BYTES 4

// Bounded outbound publish queue. Records are stored as borrowed-wire copies:
// u16 subject length, u16 payload length, then subject and payload bytes.
static uint8_t s_buf[TINYBLOK_TX_RING_CAP];
static size_t s_head;
static size_t s_tail;
static size_t s_used;
static size_t s_slack_at_end;
static size_t s_frame_bytes_sent;
static bool s_drain_pinned;
static uint32_t s_dropped;

extern void tinyblok_tx_ring_lock(void);
extern void tinyblok_tx_ring_unlock(void);

typedef struct drain_record
{
    uint8_t subject[TINYBLOK_SUBJECT_MAX];
    uint8_t payload[TINYBLOK_PAYLOAD_MAX];
    size_t subject_len;
    size_t payload_len;
    size_t rec_size;
    size_t sent;
} drain_record;

typedef enum send_status
{
    SEND_COMPLETE,
    SEND_BLOCKED,
    SEND_ERR,
} send_status;

static uint16_t read_u16(size_t off)
{
    return (uint16_t)s_buf[off] | (uint16_t)((uint16_t)s_buf[off + 1] << 8);
}

static void write_u16(size_t off, uint16_t v)
{
    s_buf[off] = (uint8_t)(v & 0xff);
    s_buf[off + 1] = (uint8_t)(v >> 8);
}

static void write_record(size_t off, const uint8_t *subject, size_t subject_len, const uint8_t *payload, size_t payload_len)
{
    size_t p = off;
    write_u16(p, (uint16_t)subject_len);
    p += 2;
    write_u16(p, (uint16_t)payload_len);
    p += 2;
    memcpy(s_buf + p, subject, subject_len);
    p += subject_len;
    memcpy(s_buf + p, payload, payload_len);
}

static void advance(size_t n)
{
    s_tail += n;
    s_used -= n;
    if (s_slack_at_end > 0 && s_tail == TINYBLOK_TX_RING_CAP - s_slack_at_end)
    {
        s_tail = 0;
        s_slack_at_end = 0;
    }
    else if (s_tail >= TINYBLOK_TX_RING_CAP)
    {
        s_tail -= TINYBLOK_TX_RING_CAP;
    }
}

static void normalize_tail_slack(void)
{
    if (s_slack_at_end > 0 && s_tail == TINYBLOK_TX_RING_CAP - s_slack_at_end)
    {
        s_tail = 0;
        s_slack_at_end = 0;
    }
}

static size_t count_locked(void)
{
    if (s_used == 0)
        return 0;

    size_t t = s_tail;
    size_t remaining = s_used;
    size_t count = 0;
    while (remaining > 0)
    {
        if (s_slack_at_end > 0 && t == TINYBLOK_TX_RING_CAP - s_slack_at_end)
            t = 0;
        const size_t subject_len = read_u16(t);
        const size_t payload_len = read_u16(t + 2);
        const size_t rec_size = HEADER_BYTES + subject_len + payload_len;
        t += rec_size;
        if (t >= TINYBLOK_TX_RING_CAP)
            t -= TINYBLOK_TX_RING_CAP;
        remaining -= rec_size;
        count += 1;
    }
    return count;
}

static bool try_write(const uint8_t *subject, size_t subject_len, const uint8_t *payload, size_t payload_len, size_t need)
{
    if (s_used == 0)
    {
        s_head = 0;
        s_tail = 0;
        s_slack_at_end = 0;
        write_record(0, subject, subject_len, payload, payload_len);
        s_head = need == TINYBLOK_TX_RING_CAP ? 0 : need;
        s_used = need;
        return true;
    }

    if (s_head > s_tail)
    {
        const size_t tail_to_end = TINYBLOK_TX_RING_CAP - s_head;
        if (need <= tail_to_end)
        {
            write_record(s_head, subject, subject_len, payload, payload_len);
            s_head += need;
            if (s_head == TINYBLOK_TX_RING_CAP)
                s_head = 0;
            s_used += need;
            return true;
        }
        if (need < s_tail)
        {
            s_slack_at_end = tail_to_end;
            write_record(0, subject, subject_len, payload, payload_len);
            s_head = need;
            s_used += need;
            return true;
        }
        return false;
    }

    const size_t gap = s_tail > s_head ? s_tail - s_head : 0;
    if (need < gap)
    {
        write_record(s_head, subject, subject_len, payload, payload_len);
        s_head += need;
        s_used += need;
        return true;
    }
    return false;
}

static bool evict_one(void)
{
    if (s_used == 0)
        return false;
    normalize_tail_slack();
    if (s_drain_pinned || s_frame_bytes_sent > 0)
        return false;

    const size_t subject_len = read_u16(s_tail);
    const size_t payload_len = read_u16(s_tail + 2);
    advance(HEADER_BYTES + subject_len + payload_len);
    s_dropped += 1;
    return true;
}

bool tinyblok_tx_ring_enqueue(const uint8_t *subject, size_t subject_len, const uint8_t *payload, size_t payload_len)
{
    tinyblok_tx_ring_lock();
    if (subject_len == 0 || subject_len > TINYBLOK_SUBJECT_MAX || payload_len > TINYBLOK_PAYLOAD_MAX)
    {
        s_dropped += 1;
        tinyblok_tx_ring_unlock();
        return false;
    }

    const size_t need = HEADER_BYTES + subject_len + payload_len;
    while (!try_write(subject, subject_len, payload, payload_len, need))
    {
        if (!evict_one())
        {
            s_dropped += 1;
            tinyblok_tx_ring_unlock();
            return false;
        }
    }
    tinyblok_tx_ring_unlock();
    return true;
}

size_t tinyblok_tx_ring_used(void)
{
    tinyblok_tx_ring_lock();
    const size_t used = s_used;
    tinyblok_tx_ring_unlock();
    return used;
}

size_t tinyblok_tx_ring_count(void)
{
    tinyblok_tx_ring_lock();
    const size_t count = count_locked();
    tinyblok_tx_ring_unlock();
    return count;
}

void tinyblok_tx_ring_reset_in_flight(void)
{
    tinyblok_tx_ring_lock();
    s_frame_bytes_sent = 0;
    tinyblok_tx_ring_unlock();
}

static bool copy_head_for_drain(drain_record *out)
{
    if (s_used == 0)
        return false;
    normalize_tail_slack();
    if (s_used == 0)
        return false;

    const size_t subject_len = read_u16(s_tail);
    const size_t payload_len = read_u16(s_tail + 2);
    const size_t rec_size = HEADER_BYTES + subject_len + payload_len;
    if (subject_len > TINYBLOK_SUBJECT_MAX || payload_len > TINYBLOK_PAYLOAD_MAX || rec_size > s_used)
    {
        s_dropped += 1;
        advance(rec_size < s_used ? rec_size : s_used);
        s_frame_bytes_sent = 0;
        s_drain_pinned = false;
        return false;
    }

    memcpy(out->subject, s_buf + s_tail + HEADER_BYTES, subject_len);
    memcpy(out->payload, s_buf + s_tail + HEADER_BYTES + subject_len, payload_len);
    out->subject_len = subject_len;
    out->payload_len = payload_len;
    out->rec_size = rec_size;
    out->sent = s_frame_bytes_sent;
    s_drain_pinned = true;
    return true;
}

static send_status send_segment(tinyblok_try_send_fn try_send, const uint8_t *buf, size_t len, size_t *sent, size_t slice_start)
{
    if (*sent >= slice_start + len)
        return SEND_COMPLETE;
    size_t off = *sent > slice_start ? *sent - slice_start : 0;
    while (off < len)
    {
        const ssize_t n = try_send(buf + off, len - off);
        if (n < 0)
        {
            *sent = slice_start + off;
            return SEND_ERR;
        }
        if (n == 0)
        {
            *sent = slice_start + off;
            return SEND_BLOCKED;
        }
        off += (size_t)n;
    }
    *sent = slice_start + len;
    return SEND_COMPLETE;
}

void tinyblok_tx_ring_drain(tinyblok_try_send_fn try_send)
{
    for (;;)
    {
        drain_record rec = {0};
        tinyblok_tx_ring_lock();
        const bool copied = copy_head_for_drain(&rec);
        tinyblok_tx_ring_unlock();
        if (!copied)
            return;

        char hdr[TINYBLOK_SUBJECT_MAX + 32];
        const int n = snprintf(hdr, sizeof(hdr), "PUB %.*s %u\r\n", (int)rec.subject_len, rec.subject, (unsigned)rec.payload_len);
        if (n <= 0 || (size_t)n >= sizeof(hdr))
        {
            tinyblok_tx_ring_lock();
            advance(rec.rec_size);
            s_frame_bytes_sent = 0;
            s_drain_pinned = false;
            s_dropped += 1;
            tinyblok_tx_ring_unlock();
            continue;
        }

        size_t sent = rec.sent;
        send_status status = send_segment(try_send, (const uint8_t *)hdr, (size_t)n, &sent, 0);
        if (status == SEND_COMPLETE)
            status = send_segment(try_send, rec.payload, rec.payload_len, &sent, (size_t)n);
        if (status == SEND_COMPLETE)
            status = send_segment(try_send, (const uint8_t *)"\r\n", 2, &sent, (size_t)n + rec.payload_len);

        tinyblok_tx_ring_lock();
        s_drain_pinned = false;
        if (status == SEND_COMPLETE)
        {
            advance(rec.rec_size);
            s_frame_bytes_sent = 0;
            tinyblok_event_publish_pub_sent();
        }
        else
        {
            s_frame_bytes_sent = status == SEND_BLOCKED ? sent : 0;
        }
        tinyblok_tx_ring_unlock();
        if (status != SEND_COMPLETE)
            return;
    }
}
