#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "app_events.h"

#ifndef CONFIG_TINYBLOK_NATS_PORT
#define CONFIG_TINYBLOK_NATS_PORT 4224
#endif

extern int tinyblok_nats_connect(void);
extern void tinyblok_nats_drain_rx(void);
extern ssize_t tinyblok_nats_try_send(const unsigned char *data, size_t len);
extern int tinyblok_nats_subject_is_valid(const unsigned char *subject, size_t subject_len);
extern int tinyblok_nats_reply(const unsigned char *subject, size_t subject_len,
                               const unsigned char *payload, size_t payload_len);

typedef enum server_case
{
    CASE_SPLIT_INFO,
    CASE_SERVER_PING,
    CASE_SPLIT_MSG_REPLY,
    CASE_OVERSIZED_MSG,
    CASE_ERR_AFTER_CONNECT,
    CASE_ERR_AFTER_READY,
} server_case;

typedef struct server_ctx
{
    server_case which;
    int ok;
    char error[160];
} server_ctx;

static size_t handle_msg_count;
static size_t reset_in_flight_count;
static bool reply_enabled;

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
    return index == 0 ? "tinyblok.req.test" : NULL;
}

void tinyblok_patchbay_handle_msg(const unsigned char *subject, size_t subject_len,
                                  const unsigned char *reply, size_t reply_len,
                                  const unsigned char *payload, size_t payload_len)
{
    handle_msg_count++;
    if (reply_enabled &&
        subject_len == strlen("tinyblok.req.test") &&
        memcmp(subject, "tinyblok.req.test", subject_len) == 0 &&
        payload_len == 0)
    {
        (void)payload;
        (void)tinyblok_nats_reply(reply, reply_len, (const unsigned char *)"ok", 2);
    }
}

static void sleep_ms(long ms)
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static void set_error(server_ctx *ctx, const char *msg)
{
    snprintf(ctx->error, sizeof(ctx->error), "%s", msg);
    ctx->ok = 0;
}

static int send_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len)
    {
        ssize_t n = send(fd, buf + off, len - off, 0);
        if (n > 0)
        {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

static int recv_until(int fd, char *buf, size_t cap, const char *needle)
{
    size_t len = 0;
    int64_t deadline = esp_timer_get_time() + 3000 * 1000;
    while (esp_timer_get_time() < deadline && len + 1 < cap)
    {
        ssize_t n = recv(fd, buf + len, cap - 1 - len, 0);
        if (n > 0)
        {
            len += (size_t)n;
            buf[len] = '\0';
            if (strstr(buf, needle) != NULL)
                return 0;
            continue;
        }
        if (n == 0)
            return -1;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            sleep_ms(10);
            continue;
        }
        return -1;
    }
    return -1;
}

static int send_info(int fd, bool split)
{
    static const char info[] = "INFO {\"server_id\":\"TEST\",\"version\":\"host\"}\r\n";
    if (!split)
        return send_all(fd, info, sizeof(info) - 1);
    if (send_all(fd, info, 7) != 0)
        return -1;
    sleep_ms(30);
    return send_all(fd, info + 7, sizeof(info) - 1 - 7);
}

static void *server_thread(void *arg)
{
    server_ctx *ctx = (server_ctx *)arg;
    ctx->ok = 1;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
    {
        set_error(ctx, "socket failed");
        return NULL;
    }
    int one = 1;
    (void)setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(CONFIG_TINYBLOK_NATS_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listen_fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(listen_fd, 1) != 0)
    {
        set_error(ctx, "bind/listen failed");
        close(listen_fd);
        return NULL;
    }

    int fd = accept(listen_fd, NULL, NULL);
    close(listen_fd);
    if (fd < 0)
    {
        set_error(ctx, "accept failed");
        return NULL;
    }

    struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (send_info(fd, ctx->which == CASE_SPLIT_INFO) != 0)
    {
        set_error(ctx, "send INFO failed");
        close(fd);
        return NULL;
    }

    char got[768] = {0};
    if (ctx->which == CASE_ERR_AFTER_CONNECT)
    {
        static const char err[] = "-ERR 'auth failed'\r\n";
        if (recv_until(fd, got, sizeof(got), "CONNECT ") != 0 ||
            send_all(fd, err, sizeof(err) - 1) != 0)
            set_error(ctx, "ERR-after-connect flow failed");
        close(fd);
        return NULL;
    }

    if (recv_until(fd, got, sizeof(got), "PING\r\n") != 0)
    {
        set_error(ctx, "did not receive setup PING");
        close(fd);
        return NULL;
    }
    if (strstr(got, "CONNECT ") == NULL)
    {
        set_error(ctx, "missing CONNECT");
        close(fd);
        return NULL;
    }
    if (send_all(fd, "PONG\r\n", 6) != 0)
    {
        set_error(ctx, "send setup PONG failed");
        close(fd);
        return NULL;
    }

    memset(got, 0, sizeof(got));
    if (recv_until(fd, got, sizeof(got), "SUB tinyblok.req.test 1\r\n") != 0)
    {
        set_error(ctx, "missing request SUB");
        close(fd);
        return NULL;
    }

    if (ctx->which == CASE_SPLIT_MSG_REPLY)
    {
        static const char msg[] = "MSG tinyblok.req.test 1 _INBOX.reply 0\r\n\r\n";
        if (send_all(fd, msg, 13) != 0)
            set_error(ctx, "send split MSG head failed");
        sleep_ms(30);
        if (ctx->ok && send_all(fd, msg + 13, sizeof(msg) - 1 - 13) != 0)
            set_error(ctx, "send split MSG tail failed");
        if (ctx->ok && recv_until(fd, got, sizeof(got), "PUB _INBOX.reply 2\r\nok\r\n") != 0)
            set_error(ctx, "did not receive reply PUB");
    }
    else if (ctx->which == CASE_OVERSIZED_MSG)
    {
        static const char msg[] =
            "MSG tinyblok.req.test 1 65\r\n"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n";
        if (send_all(fd, msg, sizeof(msg) - 1) != 0)
            set_error(ctx, "send oversized MSG failed");
    }
    else if (ctx->which == CASE_ERR_AFTER_READY)
    {
        static const char err[] = "-ERR 'maximum connections exceeded'\r\n";
        if (send_all(fd, err, sizeof(err) - 1) != 0)
            set_error(ctx, "send ready ERR failed");
    }
    else if (ctx->which == CASE_SERVER_PING)
    {
        memset(got, 0, sizeof(got));
        if (send_all(fd, "PING\r\n", 6) != 0 ||
            recv_until(fd, got, sizeof(got), "PONG\r\n") != 0)
            set_error(ctx, "server PING did not receive PONG");
    }

    sleep_ms(80);
    close(fd);
    return NULL;
}

static int run_server_case(server_case which)
{
    handle_msg_count = 0;
    reply_enabled = which == CASE_SPLIT_MSG_REPLY;

    server_ctx ctx = {.which = which, .ok = 0};
    pthread_t thread;
    if (pthread_create(&thread, NULL, server_thread, &ctx) != 0)
    {
        fprintf(stderr, "pthread_create failed\n");
        return 1;
    }
    sleep_ms(80);

    int connect_rc = tinyblok_nats_connect();
    if (which == CASE_ERR_AFTER_CONNECT)
    {
        pthread_join(thread, NULL);
        if (connect_rc == 0)
        {
            fprintf(stderr, "connect unexpectedly succeeded after -ERR\n");
            return 1;
        }
        return ctx.ok ? 0 : (fprintf(stderr, "%s\n", ctx.error), 1);
    }
    if (connect_rc != 0)
    {
        pthread_join(thread, NULL);
        fprintf(stderr, "tinyblok_nats_connect failed: %s\n", ctx.error);
        return 1;
    }

    int64_t deadline = esp_timer_get_time() + 3000 * 1000;
    while (esp_timer_get_time() < deadline)
    {
        tinyblok_nats_drain_rx();
        if (which == CASE_SPLIT_MSG_REPLY && handle_msg_count > 0)
            break;
        if ((which == CASE_SPLIT_INFO || which == CASE_SERVER_PING) &&
            tinyblok_nats_try_send((const unsigned char *)"", 0) < 0)
            break;
        if ((which == CASE_OVERSIZED_MSG || which == CASE_ERR_AFTER_READY) &&
            tinyblok_nats_try_send((const unsigned char *)"", 0) < 0)
            break;
        sleep_ms(10);
    }

    pthread_join(thread, NULL);
    if (!ctx.ok)
    {
        fprintf(stderr, "%s\n", ctx.error);
        return 1;
    }

    if (which == CASE_SPLIT_MSG_REPLY && handle_msg_count != 1)
    {
        fprintf(stderr, "expected one handled message, got %zu\n", handle_msg_count);
        return 1;
    }
    if (which == CASE_OVERSIZED_MSG && handle_msg_count != 0)
    {
        fprintf(stderr, "oversized message was handled\n");
        return 1;
    }

    while (tinyblok_nats_try_send((const unsigned char *)"", 0) >= 0 &&
           esp_timer_get_time() < deadline)
    {
        tinyblok_nats_drain_rx();
        sleep_ms(10);
    }
    return 0;
}

static int run_subject_validation(void)
{
    static const unsigned char good[] = "tinyblok.req.test";
    static const unsigned char bad_space[] = "tinyblok.req test";
    static const unsigned char bad_wild[] = "tinyblok.*.test";
    static const unsigned char bad_empty_token[] = "tinyblok..test";
    if (!tinyblok_nats_subject_is_valid(good, sizeof(good) - 1))
        return 1;
    if (tinyblok_nats_subject_is_valid(bad_space, sizeof(bad_space) - 1))
        return 1;
    if (tinyblok_nats_subject_is_valid(bad_wild, sizeof(bad_wild) - 1))
        return 1;
    if (tinyblok_nats_subject_is_valid(bad_empty_token, sizeof(bad_empty_token) - 1))
        return 1;
    if (tinyblok_nats_subject_is_valid(NULL, 0))
        return 1;
    return 0;
}

int main(void)
{
    if (run_subject_validation() != 0)
    {
        fprintf(stderr, "subject validation failed\n");
        return 1;
    }
    if (run_server_case(CASE_SPLIT_INFO) != 0)
        return 1;
    if (run_server_case(CASE_SERVER_PING) != 0)
        return 1;
    if (run_server_case(CASE_SPLIT_MSG_REPLY) != 0)
        return 1;
    if (run_server_case(CASE_OVERSIZED_MSG) != 0)
        return 1;
    if (run_server_case(CASE_ERR_AFTER_CONNECT) != 0)
        return 1;
    if (run_server_case(CASE_ERR_AFTER_READY) != 0)
        return 1;

    printf("ok nats protocol host tests: handled=%zu reset_in_flight=%zu\n",
           handle_msg_count, reset_in_flight_count);
    return 0;
}
