// Minimum-viable NATS client: TCP connect + non-blocking PUB + lazy PING/PONG.
// Reconnect is throttled and silent — when the broker dies the rule loop keeps
// running, the ring keeps buffering up to cap, and a fresh connect is attempted
// every RECONNECT_PERIOD_US until it succeeds.

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

static const char *TAG = "nats";

// Implemented in tx_ring.zig.
extern size_t tinyblok_tx_ring_used(void);
extern size_t tinyblok_tx_ring_count(void);
extern void tinyblok_tx_ring_reset_in_flight(void);

static int sock = -1;

static char rx_buf[256];
static size_t rx_len = 0;

static int64_t last_connect_attempt_us = 0;
#define RECONNECT_PERIOD_US (5LL * 1000 * 1000)

static int send_all_blocking(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len)
    {
        ssize_t n = send(fd, buf + off, len - off, 0);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static void close_sock(int log_disconnect)
{
    if (sock >= 0)
    {
        close(sock);
        sock = -1;
        rx_len = 0;
        // The head record may be partway through a send. The new connection
        // hasn't seen those bytes, so restart the record from byte 0 on next drain.
        tinyblok_tx_ring_reset_in_flight();
        if (log_disconnect)
            ESP_LOGW(TAG, "broker disconnected; will retry");
    }
}

// Try once to establish the connection. Quiet on failure (caller is the
// reconnect loop, which would spam otherwise). The first successful connect of
// each session logs INFO.
static int try_connect_once(int verbose_failure)
{
    const char *host = CONFIG_TINYBLOK_NATS_HOST;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", CONFIG_TINYBLOK_NATS_PORT);

    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    int err = getaddrinfo(host, port_str, &hints, &res);
    if (err != 0 || res == NULL)
    {
        if (verbose_failure)
            ESP_LOGE(TAG, "getaddrinfo(%s:%s) failed: %d", host, port_str, err);
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0)
    {
        if (verbose_failure)
            ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
        freeaddrinfo(res);
        return -1;
    }

    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0)
    {
        if (verbose_failure)
            ESP_LOGE(TAG, "connect(%s:%s) failed: errno=%d", host, port_str, errno);
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);

    char info_buf[512];
    ssize_t n = recv(fd, info_buf, sizeof(info_buf) - 1, 0);
    if (n <= 0)
    {
        if (verbose_failure)
            ESP_LOGE(TAG, "recv(INFO) failed: n=%d errno=%d", (int)n, errno);
        close(fd);
        return -1;
    }
    info_buf[n] = '\0';
    while (n > 0 && (info_buf[n - 1] == '\n' || info_buf[n - 1] == '\r'))
    {
        info_buf[--n] = '\0';
    }

    static const char connect_line[] = "CONNECT {\"verbose\":false,\"pedantic\":false}\r\n";
    if (send_all_blocking(fd, connect_line, sizeof(connect_line) - 1) != 0)
    {
        if (verbose_failure)
            ESP_LOGE(TAG, "send(CONNECT) failed: errno=%d", errno);
        close(fd);
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        if (verbose_failure)
            ESP_LOGE(TAG, "fcntl(O_NONBLOCK) failed: errno=%d", errno);
        close(fd);
        return -1;
    }

    sock = fd;
    ESP_LOGI(TAG, "connected %s:%s", host, port_str);
    ESP_LOGI(TAG, "<- %s", info_buf);
    size_t backlog_bytes = tinyblok_tx_ring_used();
    if (backlog_bytes > 0)
    {
        ESP_LOGI(TAG, "flushing %u buffered bytes, %u messages",
                 (unsigned)backlog_bytes, (unsigned)tinyblok_tx_ring_count());
    }
    return 0;
}

int tinyblok_nats_connect(void)
{
    last_connect_attempt_us = esp_timer_get_time();
    return try_connect_once(1);
}

// Called every tick. If disconnected, attempt a reconnect at most once per
// RECONNECT_PERIOD_US. Silent on failure.
void tinyblok_nats_maintain(void)
{
    if (sock >= 0)
        return;
    int64_t now = esp_timer_get_time();
    if (now - last_connect_attempt_us < RECONNECT_PERIOD_US)
        return;
    last_connect_attempt_us = now;
    (void)try_connect_once(0);
}

// Drain pending inbound bytes, reply PONG to any PING, log+drop the rest.
void tinyblok_nats_drain_rx(void)
{
    if (sock < 0)
        return;

    for (;;)
    {
        if (rx_len >= sizeof(rx_buf))
        {
            rx_len = 0; // line longer than rx_buf; drop silently rather than wedge
        }
        ssize_t n = recv(sock, rx_buf + rx_len, sizeof(rx_buf) - rx_len, 0);
        if (n > 0)
        {
            rx_len += (size_t)n;
        }
        else if (n == 0)
        {
            close_sock(1);
            return;
        }
        else
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EINTR)
                continue;
            close_sock(1);
            return;
        }
    }

    size_t scan = 0;
    while (scan < rx_len)
    {
        char *line = rx_buf + scan;
        size_t remaining = rx_len - scan;
        char *crlf = memmem(line, remaining, "\r\n", 2);
        if (crlf == NULL)
            break;
        size_t line_len = (size_t)(crlf - line);

        if (line_len == 4 && memcmp(line, "PING", 4) == 0)
        {
            if (send_all_blocking(sock, "PONG\r\n", 6) != 0)
            {
                close_sock(1);
                return;
            }
        }
        else if (line_len > 0)
        {
            ESP_LOGI(TAG, "<- %.*s", (int)(line_len > 80 ? 80 : line_len), line);
        }

        scan += line_len + 2;
    }

    if (scan > 0)
    {
        rx_len -= scan;
        if (rx_len > 0)
            memmove(rx_buf, rx_buf + scan, rx_len);
    }
}

// One non-blocking send. Returns bytes written (>=0), or -1 on real error
// (socket torn down so subsequent calls fail fast).
ssize_t tinyblok_nats_try_send(const unsigned char *data, size_t len)
{
    if (sock < 0)
        return -1;
    if (len == 0)
        return 0;

    ssize_t n = send(sock, data, len, 0);
    if (n >= 0)
        return n;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return 0;
    close_sock(1);
    return -1;
}
