#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>

#include "app_events.h"

static size_t handle_msg_count = 0;
static size_t reset_in_flight_count = 0;

extern int tinyblok_nats_connect(void);
extern void tinyblok_nats_drain_rx(void);
extern ssize_t tinyblok_nats_try_send(const unsigned char *data, size_t len);
extern int tinyblok_nats_reply(const unsigned char *subject, size_t subject_len,
                               const unsigned char *payload, size_t payload_len);

int64_t esp_timer_get_time(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (int64_t)ts.tv_sec * 1000000LL + (int64_t)(ts.tv_nsec / 1000);
}

size_t tinyblok_tx_ring_used(void)
{
    return 0;
}

size_t tinyblok_tx_ring_count(void)
{
    return 0;
}

void tinyblok_tx_ring_reset_in_flight(void)
{
    reset_in_flight_count++;
}

size_t tinyblok_patchbay_request_count(void)
{
    return 1;
}

const char *tinyblok_patchbay_request_subject(size_t index)
{
    return index == 0 ? "tinyblok.req.echo" : NULL;
}

void tinyblok_patchbay_handle_msg(const unsigned char *subject, size_t subject_len,
                                  const unsigned char *reply, size_t reply_len,
                                  const unsigned char *payload, size_t payload_len)
{
    handle_msg_count++;
    if (subject_len == strlen("tinyblok.req.echo") &&
        memcmp(subject, "tinyblok.req.echo", subject_len) == 0)
    {
        (void)tinyblok_nats_reply(reply, reply_len, payload, payload_len);
    }
}

static int run_serve(void)
{
    if (tinyblok_nats_connect() != 0)
    {
        fprintf(stderr, "tinyblok_nats_connect failed\n");
        return 1;
    }

    const int64_t deadline = esp_timer_get_time() + 5000 * 1000;
    while (esp_timer_get_time() < deadline)
    {
        tinyblok_nats_drain_rx();
        if (handle_msg_count > 0)
        {
            if (tinyblok_message_count() == 0)
            {
                fprintf(stderr, "message event counter did not increment\n");
                return 1;
            }
            printf("ok nats host smoke: handled=%zu reset_in_flight=%zu\n",
                   handle_msg_count, reset_in_flight_count);
            return 0;
        }

        struct timespec ts = {.tv_sec = 0, .tv_nsec = 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }

    fprintf(stderr, "timed out waiting for nats request\n");
    return 1;
}

static int run_pub(void)
{
    if (tinyblok_nats_connect() != 0)
    {
        fprintf(stderr, "tinyblok_nats_connect failed\n");
        return 1;
    }

    static const unsigned char frames[] =
        "PUB tinyblok.host.pub 5\r\nbeep0\r\n"
        "PUB tinyblok.host.pub 5\r\nbeep1\r\n"
        "PUB tinyblok.host.pub 5\r\nbeep2\r\n"
        "PUB tinyblok.host.pub 5\r\nbeep3\r\n"
        "PUB tinyblok.host.pub 5\r\nbeep4\r\n";
    size_t off = 0;
    const int64_t deadline = esp_timer_get_time() + 2000 * 1000;
    while (off < sizeof(frames) - 1 && esp_timer_get_time() < deadline)
    {
        ssize_t n = tinyblok_nats_try_send(frames + off, sizeof(frames) - 1 - off);
        if (n < 0)
        {
            fprintf(stderr, "tinyblok_nats_try_send failed\n");
            return 1;
        }
        if (n == 0)
        {
            struct timespec ts = {.tv_sec = 0, .tv_nsec = 10 * 1000 * 1000};
            nanosleep(&ts, NULL);
            continue;
        }
        off += (size_t)n;
    }

    if (off != sizeof(frames) - 1)
    {
        fprintf(stderr, "timed out publishing host frames\n");
        return 1;
    }
    printf("ok nats host pub batch: bytes=%zu messages=5\n", off);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "serve") == 0)
        return run_serve();
    if (argc == 2 && strcmp(argv[1], "pub") == 0)
        return run_pub();

    fprintf(stderr, "usage: %s serve|pub\n", argv[0]);
    return 2;
}
