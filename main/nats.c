// Minimum-viable NATS client: TCP connect + PUB + lazy PING/PONG. No SUB, no reconnect.

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

static const char *TAG = "nats";

static int sock = -1;

static char rx_buf[256];
static size_t rx_len = 0;

const char *const tinyblok_nats_host = CONFIG_TINYBLOK_NATS_HOST;

int tinyblok_nats_port(void)
{
    return CONFIG_TINYBLOK_NATS_PORT;
}

static int send_all(int fd, const char *buf, size_t len)
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

int tinyblok_nats_connect(void)
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
        ESP_LOGE(TAG, "getaddrinfo(%s:%s) failed: %d", host, port_str, err);
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0)
    {
        ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
        freeaddrinfo(res);
        return -1;
    }

    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0)
    {
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
        ESP_LOGE(TAG, "recv(INFO) failed: n=%d errno=%d", (int)n, errno);
        close(fd);
        return -1;
    }
    info_buf[n] = '\0';
    while (n > 0 && (info_buf[n - 1] == '\n' || info_buf[n - 1] == '\r'))
    {
        info_buf[--n] = '\0';
    }
    ESP_LOGI(TAG, "connected %s:%s", host, port_str);
    ESP_LOGI(TAG, "<- %s", info_buf);

    static const char connect_line[] = "CONNECT {\"verbose\":false,\"pedantic\":false}\r\n";
    if (send_all(fd, connect_line, sizeof(connect_line) - 1) != 0)
    {
        ESP_LOGE(TAG, "send(CONNECT) failed: errno=%d", errno);
        close(fd);
        return -1;
    }

    // Non-blocking after handshake so drain_inbox gets EAGAIN when idle.
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        ESP_LOGE(TAG, "fcntl(O_NONBLOCK) failed: errno=%d", errno);
        close(fd);
        return -1;
    }

    sock = fd;
    return 0;
}

// Drain pending bytes, reply PONG to any PING, log+drop the rest. Called once per publish.
static void drain_inbox(void)
{
    if (sock < 0)
        return;

    for (;;)
    {
        if (rx_len >= sizeof(rx_buf))
        {
            // Single line longer than rx_buf — drop rather than wedge the publish path.
            ESP_LOGW(TAG, "rx_buf overflow, dropping %u bytes", (unsigned)rx_len);
            rx_len = 0;
        }
        ssize_t n = recv(sock, rx_buf + rx_len, sizeof(rx_buf) - rx_len, 0);
        if (n > 0)
        {
            rx_len += (size_t)n;
        }
        else if (n == 0)
        {
            ESP_LOGW(TAG, "broker closed connection");
            close(sock);
            sock = -1;
            rx_len = 0;
            return;
        }
        else
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EINTR)
                continue;
            ESP_LOGE(TAG, "recv failed: errno=%d", errno);
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
            if (send_all(sock, "PONG\r\n", 6) != 0)
            {
                ESP_LOGE(TAG, "send(PONG) failed: errno=%d", errno);
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

int tinyblok_nats_publish(const char *subject, const char *payload, size_t payload_len)
{
    if (sock < 0)
        return -1;

    drain_inbox();
    if (sock < 0)
        return -1;

    char header[128];
    int hn = snprintf(header, sizeof(header), "PUB %s %u\r\n", subject, (unsigned)payload_len);
    if (hn < 0 || hn >= (int)sizeof(header))
        return -1;

    if (send_all(sock, header, (size_t)hn) != 0)
        return -1;
    if (payload_len > 0 && send_all(sock, payload, payload_len) != 0)
        return -1;
    if (send_all(sock, "\r\n", 2) != 0)
        return -1;
    return 0;
}
