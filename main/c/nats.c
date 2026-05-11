// Minimum-viable NATS client. Plaintext or TLS-via-INFO-upgrade per Kconfig;
// auth none / user+pass / .creds (JWT + Ed25519 nonce sig).
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#ifdef ESP_PLATFORM
#include "lwip/inet.h"
#else
#include <arpa/inet.h>
#endif
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

#include "app_events.h"

#if CONFIG_TINYBLOK_NATS_TLS
#include "mbedtls/ssl.h"
#include "mbedtls/error.h"
#include "psa/crypto.h"
#if CONFIG_TINYBLOK_NATS_TLS_BUNDLE
#include "esp_crt_bundle.h"
#endif
#endif

#if CONFIG_TINYBLOK_NATS_AUTH_CREDS
#include "creds.h"
#endif

static const char *TAG = "nats";

// Implemented in tx_ring.zig.
extern size_t tinyblok_tx_ring_used(void);
extern size_t tinyblok_tx_ring_count(void);
extern void tinyblok_tx_ring_reset_in_flight(void);

typedef struct
{
    const char *subject;
} tinyblok_request_sub_t;

extern const size_t tinyblok_request_sub_count;
extern const tinyblok_request_sub_t tinyblok_request_subs[];
extern void tinyblok_nats_handle_msg(const unsigned char *subject, size_t subject_len,
                                     const unsigned char *reply, size_t reply_len,
                                     const unsigned char *payload, size_t payload_len);

static int sock = -1;
static int nats_connected = 0;

#if CONFIG_TINYBLOK_NATS_TLS
static int tls_active = 0;
static mbedtls_ssl_context ssl;
static mbedtls_ssl_config ssl_conf;
static int tls_inited = 0;
#endif

#define NET_OPEN() (sock >= 0)
#define NATS_SUBJECT_MAX 128
#define NATS_MSG_PAYLOAD_MAX 64

static char rx_buf[768];
static size_t rx_len = 0;

static int64_t last_connect_attempt_us = 0;
#define RECONNECT_PERIOD_US (5LL * 1000 * 1000)
// `-ERR` from the broker gets a longer backoff: NGS keeps dead connections
// counted against the account cap for minutes, so 5s retries can pin us at
// the "maximum active connections" error indefinitely.
#define RECONNECT_AFTER_ERR_US (60LL * 1000 * 1000)
static int64_t backoff_until_us = 0;

#if CONFIG_TINYBLOK_NATS_TLS
static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    int fd = (int)(intptr_t)ctx;
    ssize_t n = send(fd, buf, len, 0);
    if (n >= 0)
        return (int)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    if (errno == EINTR)
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    int fd = (int)(intptr_t)ctx;
    ssize_t n = recv(fd, buf, len, 0);
    if (n > 0)
        return (int)n;
    if (n == 0)
        return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
        return MBEDTLS_ERR_SSL_WANT_READ;
    if (errno == EINTR)
        return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}
#endif

static int net_send_all(const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t off = 0;
#if CONFIG_TINYBLOK_NATS_TLS
    if (tls_active)
    {
        while (off < len)
        {
            int n = mbedtls_ssl_write(&ssl, (const unsigned char *)p + off, len - off);
            if (n > 0)
            {
                off += (size_t)n;
                continue;
            }
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
                continue;
            return -1;
        }
        return 0;
    }
#endif
    while (off < len)
    {
        ssize_t n = send(sock, p + off, len - off, 0);
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

// Returns >0 bytes read, 0 if peer closed, -1 on error, -2 on would-block.
static ssize_t net_recv(void *buf, size_t cap)
{
#if CONFIG_TINYBLOK_NATS_TLS
    if (tls_active)
    {
        int n = mbedtls_ssl_read(&ssl, (unsigned char *)buf, cap);
        if (n > 0)
            return n;
        if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
            return 0;
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
            return -2;
        return -1;
    }
#endif
    ssize_t n = recv(sock, buf, cap, 0);
    if (n > 0)
        return n;
    if (n == 0)
        return 0;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
        return -2;
    if (errno == EINTR)
        return -2;
    return -1;
}

// Returns bytes written (>=0), 0 on would-block, or -1 on hard error.
static ssize_t net_try_send(const void *buf, size_t len)
{
#if CONFIG_TINYBLOK_NATS_TLS
    if (tls_active)
    {
        int n = mbedtls_ssl_write(&ssl, (const unsigned char *)buf, len);
        if (n >= 0)
            return n;
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
            return 0;
        return -1;
    }
#endif
    ssize_t n = send(sock, buf, len, 0);
    if (n >= 0)
        return n;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return 0;
    return -1;
}

static void net_close(void)
{
#if CONFIG_TINYBLOK_NATS_TLS
    if (tls_active)
    {
        mbedtls_ssl_close_notify(&ssl);
        mbedtls_ssl_session_reset(&ssl);
        tls_active = 0;
    }
#endif
    if (sock >= 0)
    {
        close(sock);
        sock = -1;
    }
}

static void close_sock(int log_disconnect)
{
    if (NET_OPEN())
    {
        net_close();
        if (nats_connected)
        {
            nats_connected = 0;
            tinyblok_event_publish_nats_disconnected();
        }
        rx_len = 0;
        // Restart any partially-sent head record from byte 0 on next drain.
        tinyblok_tx_ring_reset_in_flight();
        if (log_disconnect)
            ESP_LOGW(TAG, "broker disconnected; will retry");
    }
}

#if CONFIG_TINYBLOK_NATS_TLS
// Long-lived mbedTLS state, reused across reconnects via session_reset.
static int tls_init_once(const char *host)
{
    if (tls_inited)
        return 0;

    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&ssl_conf);

    psa_status_t ps = psa_crypto_init();
    if (ps != PSA_SUCCESS && ps != PSA_ERROR_ALREADY_EXISTS)
    {
        ESP_LOGE(TAG, "psa_crypto_init failed: %d", (int)ps);
        return -1;
    }

    int rc = mbedtls_ssl_config_defaults(&ssl_conf, MBEDTLS_SSL_IS_CLIENT,
                                         MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "ssl_config_defaults failed: -0x%04x", -rc);
        return -1;
    }

#if CONFIG_TINYBLOK_NATS_TLS_BUNDLE
    mbedtls_ssl_conf_authmode(&ssl_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    rc = esp_crt_bundle_attach(&ssl_conf);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "esp_crt_bundle_attach failed: 0x%x", rc);
        return -1;
    }
#else
    mbedtls_ssl_conf_authmode(&ssl_conf, MBEDTLS_SSL_VERIFY_NONE);
#endif

    rc = mbedtls_ssl_setup(&ssl, &ssl_conf);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "ssl_setup failed: -0x%04x", -rc);
        return -1;
    }
    rc = mbedtls_ssl_set_hostname(&ssl, host);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "ssl_set_hostname failed: -0x%04x", -rc);
        return -1;
    }

    tls_inited = 1;
    return 0;
}

static int tls_handshake(void)
{
    mbedtls_ssl_set_bio(&ssl, (void *)(intptr_t)sock, bio_send, bio_recv, NULL);

    for (;;)
    {
        int rc = mbedtls_ssl_handshake(&ssl);
        if (rc == 0)
            return 0;
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        char err[128];
        mbedtls_strerror(rc, err, sizeof(err));
        ESP_LOGE(TAG, "ssl_handshake failed: -0x%04x (%s)", -rc, err);
        return -1;
    }
}
#endif

#if CONFIG_TINYBLOK_NATS_AUTH_CREDS
// Copies the JSON "nonce" value from INFO to `out` (NUL-terminated).
// Returns the length, or -1 if the field is missing.
static int extract_nonce(const char *info, size_t info_len, char *out, size_t out_cap)
{
    static const char key[] = "\"nonce\":\"";
    const char *p = memmem(info, info_len, key, sizeof(key) - 1);
    if (!p)
        return -1;
    p += sizeof(key) - 1;
    const char *end = memchr(p, '"', info_len - (size_t)(p - info));
    if (!end)
        return -1;
    size_t n = (size_t)(end - p);
    if (n + 1 > out_cap)
        return -1;
    memcpy(out, p, n);
    out[n] = '\0';
    return (int)n;
}
#endif

// Returns the byte length of the line written to `out`, or -1 on failure.
static int build_connect_line(const char *info, size_t info_len, char *out, size_t out_cap)
{
    (void)info;
    (void)info_len;

    static const char common[] =
        "\"verbose\":false,\"pedantic\":false,"
        "\"name\":\"" CONFIG_TINYBLOK_NATS_CLIENT_NAME "\","
        "\"lang\":\"c\",\"version\":\"0.1\","
#if CONFIG_TINYBLOK_NATS_TLS
        "\"tls_required\":true";
#else
        "\"tls_required\":false";
#endif

#if CONFIG_TINYBLOK_NATS_AUTH_USERPASS
    int n = snprintf(out, out_cap,
                     "CONNECT {%s,\"user\":\"%s\",\"pass\":\"%s\"}\r\n",
                     common, CONFIG_TINYBLOK_NATS_USER, CONFIG_TINYBLOK_NATS_PASS);
    if (n <= 0 || (size_t)n >= out_cap)
        return -1;
    return n;

#elif CONFIG_TINYBLOK_NATS_AUTH_CREDS
    const tinyblok_creds_t *c = tinyblok_creds_get();
    if (!c)
    {
        ESP_LOGE(TAG, "creds not loaded");
        return -1;
    }

    char nonce[128];
    int nonce_len = extract_nonce(info, info_len, nonce, sizeof(nonce));
    if (nonce_len <= 0)
    {
        ESP_LOGE(TAG, "no nonce in INFO; broker may not require auth");
        return -1;
    }

    unsigned char sig[64];
    if (tinyblok_creds_sign((const unsigned char *)nonce, (size_t)nonce_len, sig) != 0)
        return -1;

    char sig_b64[96];
    tinyblok_b64url_encode(sig, 64, sig_b64);

    int n = snprintf(out, out_cap,
                     "CONNECT {%s,\"jwt\":\"%s\",\"sig\":\"%s\"}\r\n",
                     common, c->jwt, sig_b64);
    if (n <= 0 || (size_t)n >= out_cap)
        return -1;
    return n;

#else
    int n = snprintf(out, out_cap, "CONNECT {%s}\r\n", common);
    if (n <= 0 || (size_t)n >= out_cap)
        return -1;
    return n;
#endif
}

static int send_request_subs(void)
{
    for (size_t i = 0; i < tinyblok_request_sub_count; i++)
    {
        const char *subject = tinyblok_request_subs[i].subject;
        char line[NATS_SUBJECT_MAX + 32];
        int n = snprintf(line, sizeof(line), "SUB %s %u\r\n", subject, (unsigned)(i + 1));
        if (n <= 0 || (size_t)n >= sizeof(line))
        {
            ESP_LOGE(TAG, "request subject too long: %s", subject);
            return -1;
        }
        if (net_send_all(line, (size_t)n) != 0)
            return -1;
        ESP_LOGI(TAG, "request '%s' subscribed", subject);
    }
    return 0;
}

int tinyblok_nats_reply(const unsigned char *subject, size_t subject_len,
                        const unsigned char *payload, size_t payload_len)
{
    if (!NET_OPEN() || subject_len == 0 || subject_len > NATS_SUBJECT_MAX)
        return -1;

    char hdr[NATS_SUBJECT_MAX + 32];
    int n = snprintf(hdr, sizeof(hdr), "PUB %.*s %u\r\n",
                     (int)subject_len, (const char *)subject, (unsigned)payload_len);
    if (n <= 0 || (size_t)n >= sizeof(hdr))
        return -1;
    if (net_send_all(hdr, (size_t)n) != 0)
    {
        close_sock(1);
        return -1;
    }
    if (payload_len > 0 && net_send_all(payload, payload_len) != 0)
    {
        close_sock(1);
        return -1;
    }
    if (net_send_all("\r\n", 2) != 0)
    {
        close_sock(1);
        return -1;
    }
    return 0;
}

static int try_connect_once(int verbose_failure)
{
    const char *host = CONFIG_TINYBLOK_NATS_HOST;
    int port = CONFIG_TINYBLOK_NATS_PORT;
    char peer_ip[16] = "";

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

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
    if (res->ai_family == AF_INET)
    {
        const struct sockaddr_in *addr = (const struct sockaddr_in *)res->ai_addr;
        snprintf(peer_ip, sizeof(peer_ip), "%s", inet_ntoa(addr->sin_addr));
    }
    freeaddrinfo(res);
    sock = fd;

    // INFO arrives in plaintext on accept, before any TLS upgrade.
    static char info_buf[768];
    ssize_t n = net_recv(info_buf, sizeof(info_buf) - 1);
    if (n <= 0)
    {
        if (verbose_failure)
            ESP_LOGE(TAG, "recv(INFO) failed: n=%d", (int)n);
        net_close();
        return -1;
    }
    info_buf[n] = '\0';
    size_t info_len = (size_t)n;
    while (info_len > 0 && (info_buf[info_len - 1] == '\n' || info_buf[info_len - 1] == '\r'))
        info_buf[--info_len] = '\0';

#if CONFIG_TINYBLOK_NATS_TLS
    // We don't inspect INFO's "tls_required"; the handshake will fail
    // loudly if the broker isn't expecting TLS.
    if (tls_init_once(host) != 0)
    {
        net_close();
        return -1;
    }
    if (tls_handshake() != 0)
    {
        net_close();
        return -1;
    }
    tls_active = 1;
#endif

    static char connect_line[2560];
    int connect_len = build_connect_line(info_buf, info_len, connect_line, sizeof(connect_line));
    if (connect_len < 0)
    {
        if (verbose_failure)
            ESP_LOGE(TAG, "could not build CONNECT line");
        net_close();
        return -1;
    }
    if (net_send_all(connect_line, (size_t)connect_len) != 0)
    {
        if (verbose_failure)
            ESP_LOGE(TAG, "send(CONNECT) failed");
        net_close();
        return -1;
    }
    if (send_request_subs() != 0)
    {
        if (verbose_failure)
            ESP_LOGE(TAG, "send(SUB) failed");
        net_close();
        return -1;
    }

    // Steady-state I/O is non-blocking; handshake and CONNECT are done.
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        if (verbose_failure)
            ESP_LOGE(TAG, "fcntl(O_NONBLOCK) failed: errno=%d", errno);
        net_close();
        return -1;
    }

#if CONFIG_TINYBLOK_NATS_TLS
    ESP_LOGI(TAG, "connected %s:%d (tls)", host, port);
#else
    ESP_LOGI(TAG, "connected %s:%d", host, port);
#endif
    nats_connected = 1;
    tinyblok_event_publish_nats_connected(host, peer_ip, (uint16_t)port);
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
#if CONFIG_TINYBLOK_NATS_AUTH_CREDS
    if (tinyblok_creds_load() != 0)
    {
        ESP_LOGE(TAG, "creds load failed; aborting connect");
        return -1;
    }
#endif
    last_connect_attempt_us = esp_timer_get_time();
    return try_connect_once(1);
}

void tinyblok_nats_maintain(void)
{
    if (NET_OPEN())
        return;
    int64_t now = esp_timer_get_time();
    if (now < backoff_until_us)
        return;
    if (now - last_connect_attempt_us < RECONNECT_PERIOD_US)
        return;
    last_connect_attempt_us = now;
    (void)try_connect_once(0);
}

typedef struct
{
    const unsigned char *subject;
    size_t subject_len;
    const unsigned char *reply;
    size_t reply_len;
    size_t payload_len;
} nats_msg_t;

// Returns 0 on success, -1 on malformed input, -2 on overflow or cap exceed.
static int parse_size(const char *p, size_t len, size_t max, size_t *out)
{
    if (len == 0)
        return -1;
    size_t v = 0;
    for (size_t i = 0; i < len; i++)
    {
        if (p[i] < '0' || p[i] > '9')
            return -1;
        size_t digit = (size_t)(p[i] - '0');
        if (v > (SIZE_MAX - digit) / 10)
            return -2;
        v = v * 10 + digit;
        if (v > max)
            return -2;
    }
    *out = v;
    return 0;
}

static int parse_msg_line(char *line, size_t line_len, nats_msg_t *msg)
{
    if (line_len < 4 || memcmp(line, "MSG ", 4) != 0)
        return -1;

    char *tok[4] = {0};
    size_t tok_len[4] = {0};
    size_t ntok = 0;
    size_t i = 4;
    while (i < line_len && ntok < 4)
    {
        while (i < line_len && line[i] == ' ')
            i++;
        if (i >= line_len)
            break;
        size_t start = i;
        while (i < line_len && line[i] != ' ')
            i++;
        tok[ntok] = line + start;
        tok_len[ntok] = i - start;
        ntok++;
    }
    while (i < line_len && line[i] == ' ')
        i++;
    if (i < line_len)
        return -1;

    if (ntok != 3 && ntok != 4)
        return -1;

    msg->subject = (const unsigned char *)tok[0];
    msg->subject_len = tok_len[0];
    if (msg->subject_len == 0 || msg->subject_len > NATS_SUBJECT_MAX)
        return -2;
    if (ntok == 3)
    {
        msg->reply = (const unsigned char *)"";
        msg->reply_len = 0;
        return parse_size(tok[2], tok_len[2], NATS_MSG_PAYLOAD_MAX, &msg->payload_len);
    }

    msg->reply = (const unsigned char *)tok[2];
    msg->reply_len = tok_len[2];
    if (msg->reply_len == 0 || msg->reply_len > NATS_SUBJECT_MAX)
        return -2;
    return parse_size(tok[3], tok_len[3], NATS_MSG_PAYLOAD_MAX, &msg->payload_len);
}

void tinyblok_nats_drain_rx(void)
{
    if (!NET_OPEN())
        return;

    int peer_closed = 0;
    for (;;)
    {
        if (rx_len >= sizeof(rx_buf))
            rx_len = 0; // overflow: drop the in-flight line rather than wedge.
        ssize_t n = net_recv(rx_buf + rx_len, sizeof(rx_buf) - rx_len);
        if (n > 0)
        {
            rx_len += (size_t)n;
            continue;
        }
        if (n == -2)
            break;
        // EOF or error: defer teardown so any trailing `-ERR ...\r\n` from
        // the broker still gets parsed and logged below.
        peer_closed = 1;
        break;
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

        if (line_len >= 4 && memcmp(line, "MSG ", 4) == 0)
        {
            nats_msg_t msg;
            int parse_rc = parse_msg_line(line, line_len, &msg);
            if (parse_rc != 0)
            {
                ESP_LOGW(TAG, "%s", parse_rc == -2 ? "oversized MSG line" : "malformed MSG line");
                close_sock(1);
                return;
            }

            size_t payload_off = line_len + 2;
            if (payload_off > SIZE_MAX - msg.payload_len - 2)
            {
                ESP_LOGW(TAG, "MSG frame length overflow");
                close_sock(1);
                return;
            }
            size_t frame_len = payload_off + msg.payload_len + 2;
            if (remaining < frame_len)
                break;
            unsigned char *payload = (unsigned char *)line + payload_off;
            if (line[frame_len - 2] != '\r' || line[frame_len - 1] != '\n')
            {
                ESP_LOGW(TAG, "malformed MSG payload terminator");
                close_sock(1);
                return;
            }

            tinyblok_event_publish_message_processed();
            if (msg.reply_len > 0)
            {
                tinyblok_nats_handle_msg(msg.subject, msg.subject_len,
                                         msg.reply, msg.reply_len,
                                         payload, msg.payload_len);
                if (!NET_OPEN())
                    return;
            }
            scan += frame_len;
            continue;
        }

        if (line_len == 4 && memcmp(line, "PING", 4) == 0)
        {
            if (net_send_all("PONG\r\n", 6) != 0)
            {
                close_sock(1);
                return;
            }
        }
        else if (line_len >= 4 && memcmp(line, "-ERR", 4) == 0)
        {
            ESP_LOGW(TAG, "<- %.*s (backing off %llds)",
                     (int)(line_len > 120 ? 120 : line_len), line,
                     RECONNECT_AFTER_ERR_US / 1000000);
            backoff_until_us = esp_timer_get_time() + RECONNECT_AFTER_ERR_US;
        }
        else if (line_len > 0)
        {
            ESP_LOGI(TAG, "<- %.*s", (int)(line_len > 80 ? 80 : line_len), line);
        }

        scan += line_len + 2;
    }

    if (peer_closed)
    {
        close_sock(1);
        return;
    }

    if (scan > 0)
    {
        rx_len -= scan;
        if (rx_len > 0)
            memmove(rx_buf, rx_buf + scan, rx_len);
    }
}

ssize_t tinyblok_nats_try_send(const unsigned char *data, size_t len)
{
    if (!NET_OPEN())
        return -1;
    if (len == 0)
        return 0;
    ssize_t n = net_try_send(data, len);
    if (n < 0)
    {
        close_sock(1);
        return -1;
    }
    return n;
}
