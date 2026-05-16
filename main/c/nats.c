// Minimum-viable NATS client. Runtime config selects plaintext/TLS and
// auth mode; Kconfig only decides which support code is compiled in.
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
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

#ifndef CONFIG_TINYBLOK_NATS_CREDS_SUPPORT
#define CONFIG_TINYBLOK_NATS_CREDS_SUPPORT 0
#endif
#include "app_events.h"
#include "tinyblok_patchbay.h"
#include "tinyblok_tx_ring.h"
#ifdef ESP_PLATFORM
#include "tinyblok_config.h"
#endif

#if CONFIG_TINYBLOK_NATS_TLS
#include "mbedtls/ssl.h"
#include "mbedtls/error.h"
#include "psa/crypto.h"
#if CONFIG_TINYBLOK_NATS_TLS_BUNDLE
#include "esp_crt_bundle.h"
#endif
#endif

#if CONFIG_TINYBLOK_NATS_CREDS_SUPPORT
#include "creds.h"
#endif

static const char *TAG = "nats";

static int sock = -1;
static int nats_connected = 0;
#ifdef ESP_PLATFORM
static tinyblok_config_t nats_cfg;
#endif

#if CONFIG_TINYBLOK_NATS_TLS
static int tls_active = 0;
static mbedtls_ssl_context ssl;
static mbedtls_ssl_config ssl_conf;
static int tls_inited = 0;
static int tls_session_started = 0;
#endif

#define NET_OPEN() (sock >= 0)
#define NATS_SUBJECT_MAX 128
#define NATS_MSG_PAYLOAD_MAX 64
#define NATS_IO_TIMEOUT_MS 3000
#define NATS_CONNECT_TIMEOUT_MS 5000
#define NATS_HANDSHAKE_TIMEOUT_MS 5000
#define NATS_RX_READS_PER_DRAIN 8
#define NATS_CONTROL_TX_CAP (NATS_SUBJECT_MAX + NATS_MSG_PAYLOAD_MAX + 40)

static char rx_buf[768];
static size_t rx_len = 0;
static unsigned char control_tx[NATS_CONTROL_TX_CAP];
static size_t control_tx_len = 0;
static size_t control_tx_off = 0;
static int net_recv_want_write = 0;

static int64_t last_connect_attempt_us = 0;
#define RECONNECT_PERIOD_US (5LL * 1000 * 1000)
// `-ERR` from the broker gets a longer backoff: NGS keeps dead connections
// counted against the account cap for minutes, so 5s retries can pin us at
// the "maximum active connections" error indefinitely.
#define RECONNECT_AFTER_ERR_US (60LL * 1000 * 1000)
static int64_t backoff_until_us = 0;

#ifdef ESP_PLATFORM
static int load_runtime_config(void)
{
    esp_err_t err = tinyblok_config_load(&nats_cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "config load failed: %s", esp_err_to_name(err));
        return -1;
    }
    return 0;
}

static const char *runtime_host(void) { return nats_cfg.nats_host; }
static int runtime_port(void) { return nats_cfg.nats_port; }
static const char *runtime_name(void) { return nats_cfg.device_name[0] ? nats_cfg.device_name : "tinyblok"; }
static int runtime_tls_enabled(void) { return nats_cfg.nats_tls; }
#else
static int load_runtime_config(void) { return 0; }
static const char *runtime_host(void) { return CONFIG_TINYBLOK_NATS_HOST; }
static int runtime_port(void) { return CONFIG_TINYBLOK_NATS_PORT; }
static const char *runtime_name(void) { return CONFIG_TINYBLOK_NATS_CLIENT_NAME; }
static int runtime_tls_enabled(void) { return CONFIG_TINYBLOK_NATS_TLS; }
#endif

static int64_t deadline_from_now_ms(int timeout_ms)
{
    return esp_timer_get_time() + (int64_t)timeout_ms * 1000;
}

static int wait_fd_ready(int fd, int want_read, int want_write, int64_t deadline_us)
{
    if (!want_read && !want_write)
        return -1;

    for (;;)
    {
        int64_t now = esp_timer_get_time();
        if (now >= deadline_us)
            return 0;

        int64_t remaining_us = deadline_us - now;
        struct timeval tv = {
            .tv_sec = (long)(remaining_us / 1000000),
            .tv_usec = (long)(remaining_us % 1000000),
        };

        fd_set rfds;
        fd_set wfds;
        fd_set *read_set = NULL;
        fd_set *write_set = NULL;
        if (want_read)
        {
            FD_ZERO(&rfds);
            FD_SET(fd, &rfds);
            read_set = &rfds;
        }
        if (want_write)
        {
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            write_set = &wfds;
        }

        int rc = select(fd + 1, read_set, write_set, NULL, &tv);
        if (rc > 0)
            return 1;
        if (rc == 0)
            return 0;
        if (errno == EINTR)
            continue;
        return -1;
    }
}

static int set_fd_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    if ((flags & O_NONBLOCK) != 0)
        return 0;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int connect_with_deadline(int fd, const struct sockaddr *addr, socklen_t addrlen, int64_t deadline_us)
{
    if (connect(fd, addr, addrlen) == 0)
        return 0;

    if (errno != EINPROGRESS && errno != EALREADY && errno != EWOULDBLOCK)
        return -1;

    int ready = wait_fd_ready(fd, 0, 1, deadline_us);
    if (ready <= 0)
    {
        if (ready == 0)
            errno = ETIMEDOUT;
        return -1;
    }

    int so_error = 0;
    socklen_t so_error_len = sizeof(so_error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len) != 0)
        return -1;
    if (so_error != 0)
    {
        errno = so_error;
        return -1;
    }
    return 0;
}

int tinyblok_nats_subject_is_valid(const unsigned char *subject, size_t subject_len)
{
    if (subject == NULL || subject_len == 0 || subject_len > NATS_SUBJECT_MAX)
        return 0;

    int prev_dot = 1;
    for (size_t i = 0; i < subject_len; i++)
    {
        unsigned char c = subject[i];
        if (c <= ' ' || c == 0x7f || c == '*' || c == '>')
            return 0;
        if (c == '.')
        {
            if (prev_dot)
                return 0;
            prev_dot = 1;
            continue;
        }
        prev_dot = 0;
    }
    return !prev_dot;
}

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

static int net_send_all_until(const void *buf, size_t len, int64_t deadline_us)
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
            {
                int ready = wait_fd_ready(sock, n == MBEDTLS_ERR_SSL_WANT_READ,
                                          n == MBEDTLS_ERR_SSL_WANT_WRITE, deadline_us);
                if (ready == 1)
                    continue;
                return -1;
            }
            if (n == 0)
            {
                int ready = wait_fd_ready(sock, 0, 1, deadline_us);
                if (ready == 1)
                    continue;
                return -1;
            }
            return -1;
        }
        return 0;
    }
#endif
    while (off < len)
    {
        ssize_t n = send(sock, p + off, len - off, 0);
        if (n > 0)
        {
            off += (size_t)n;
            continue;
        }
        if (n == 0)
        {
            int ready = wait_fd_ready(sock, 0, 1, deadline_us);
            if (ready == 1)
                continue;
            return -1;
        }
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                int ready = wait_fd_ready(sock, 0, 1, deadline_us);
                if (ready == 1)
                    continue;
            }
            return -1;
        }
    }
    return 0;
}

// Returns >0 bytes read, 0 if peer closed, -1 on error, -2 on would-block.
static ssize_t net_recv(void *buf, size_t cap)
{
    net_recv_want_write = 0;
#if CONFIG_TINYBLOK_NATS_TLS
    if (tls_active)
    {
        int n = mbedtls_ssl_read(&ssl, (unsigned char *)buf, cap);
        if (n > 0)
            return n;
        if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
            return 0;
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            net_recv_want_write = n == MBEDTLS_ERR_SSL_WANT_WRITE;
            return -2;
        }
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

static void reset_control_tx(void)
{
    control_tx_len = 0;
    control_tx_off = 0;
}

static int control_tx_pending(void)
{
    return control_tx_off < control_tx_len;
}

static int drain_control_tx(void)
{
    while (control_tx_off < control_tx_len)
    {
        ssize_t n = net_try_send(control_tx + control_tx_off, control_tx_len - control_tx_off);
        if (n < 0)
            return -1;
        if (n == 0)
            return 0;
        control_tx_off += (size_t)n;
    }
    reset_control_tx();
    return 1;
}

static int queue_control_frame(const unsigned char *data, size_t len)
{
    if (len == 0 || len > sizeof(control_tx))
        return -1;
    if (control_tx_pending())
        return -1;
    memcpy(control_tx, data, len);
    control_tx_len = len;
    control_tx_off = 0;
    return drain_control_tx() < 0 ? -1 : 0;
}

static void net_close(void)
{
#if CONFIG_TINYBLOK_NATS_TLS
    if (tls_active)
    {
        mbedtls_ssl_close_notify(&ssl);
    }
    if (tls_session_started)
    {
        mbedtls_ssl_session_reset(&ssl);
        tls_active = 0;
        tls_session_started = 0;
    }
#endif
    reset_control_tx();
    if (sock >= 0)
    {
        close(sock);
        sock = -1;
    }
}

static void set_socket_timeouts(int fd)
{
    struct timeval tv = {
        .tv_sec = NATS_IO_TIMEOUT_MS / 1000,
        .tv_usec = (NATS_IO_TIMEOUT_MS % 1000) * 1000,
    };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0)
        ESP_LOGW(TAG, "setsockopt(SO_RCVTIMEO) failed: errno=%d", errno);
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0)
        ESP_LOGW(TAG, "setsockopt(SO_SNDTIMEO) failed: errno=%d", errno);
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
    tls_session_started = 1;
    int64_t deadline_us = deadline_from_now_ms(NATS_HANDSHAKE_TIMEOUT_MS);

    for (;;)
    {
        int rc = mbedtls_ssl_handshake(&ssl);
        if (rc == 0)
            return 0;
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            int ready = wait_fd_ready(sock, rc == MBEDTLS_ERR_SSL_WANT_READ,
                                      rc == MBEDTLS_ERR_SSL_WANT_WRITE, deadline_us);
            if (ready == 1)
                continue;
            ESP_LOGE(TAG, "ssl_handshake timed out");
            return -1;
        }
        char err[128];
        mbedtls_strerror(rc, err, sizeof(err));
        ESP_LOGE(TAG, "ssl_handshake failed: -0x%04x (%s)", -rc, err);
        return -1;
    }
}
#endif

#if CONFIG_TINYBLOK_NATS_CREDS_SUPPORT
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

static int append_raw(char *out, size_t cap, size_t *off, const char *src, size_t len)
{
    if (*off > cap || len > cap - *off)
        return -1;
    memcpy(out + *off, src, len);
    *off += len;
    return 0;
}

static int append_lit(char *out, size_t cap, size_t *off, const char *src)
{
    return append_raw(out, cap, off, src, strlen(src));
}

static int append_fmt(char *out, size_t cap, size_t *off, const char *fmt, ...)
{
    if (*off >= cap)
        return -1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + *off, cap - *off, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *off)
        return -1;
    *off += (size_t)n;
    return 0;
}

static int append_json_cstr(char *out, size_t cap, size_t *off, const char *src)
{
    if (append_lit(out, cap, off, "\"") != 0)
        return -1;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++)
    {
        if (*p == '"' || *p == '\\')
        {
            char esc[2] = {'\\', (char)*p};
            if (append_raw(out, cap, off, esc, sizeof(esc)) != 0)
                return -1;
        }
        else if (*p < 0x20)
        {
            if (append_fmt(out, cap, off, "\\u%04x", (unsigned)*p) != 0)
                return -1;
        }
        else
        {
            char c = (char)*p;
            if (append_raw(out, cap, off, &c, 1) != 0)
                return -1;
        }
    }
    return append_lit(out, cap, off, "\"");
}

// Returns the byte length of the line written to `out`, or -1 on failure.
static int build_connect_line(const char *info, size_t info_len, char *out, size_t out_cap)
{
#if !defined(ESP_PLATFORM) || !CONFIG_TINYBLOK_NATS_CREDS_SUPPORT
    (void)info;
    (void)info_len;
#endif

    size_t off = 0;
    if (append_lit(out, out_cap, &off,
                   "CONNECT {\"verbose\":false,\"pedantic\":false,\"name\":") != 0 ||
        append_json_cstr(out, out_cap, &off, runtime_name()) != 0 ||
        append_lit(out, out_cap, &off, ",\"lang\":\"c\",\"version\":\"0.1\",\"tls_required\":") != 0 ||
        append_lit(out, out_cap, &off, runtime_tls_enabled() ? "true" : "false") != 0)
        return -1;

#ifdef ESP_PLATFORM
    if (strcmp(nats_cfg.nats_auth, "userpass") == 0)
    {
        if (append_lit(out, out_cap, &off, ",\"user\":") != 0 ||
            append_json_cstr(out, out_cap, &off, nats_cfg.nats_user) != 0 ||
            append_lit(out, out_cap, &off, ",\"pass\":") != 0 ||
            append_json_cstr(out, out_cap, &off, nats_cfg.nats_password) != 0)
            return -1;
    }
    else if (strcmp(nats_cfg.nats_auth, "token") == 0)
    {
        if (append_lit(out, out_cap, &off, ",\"auth_token\":") != 0 ||
            append_json_cstr(out, out_cap, &off, nats_cfg.nats_token) != 0)
            return -1;
    }
    else if (strcmp(nats_cfg.nats_auth, "creds") == 0)
    {
#if CONFIG_TINYBLOK_NATS_CREDS_SUPPORT
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

        if (append_lit(out, out_cap, &off, ",\"jwt\":") != 0 ||
            append_json_cstr(out, out_cap, &off, c->jwt) != 0 ||
            append_lit(out, out_cap, &off, ",\"sig\":") != 0 ||
            append_json_cstr(out, out_cap, &off, sig_b64) != 0)
            return -1;
#else
        ESP_LOGE(TAG, ".creds auth selected but creds support is not compiled in");
        return -1;
#endif
    }
#endif

    if (append_lit(out, out_cap, &off, "}\r\n") != 0)
        return -1;
    if (off >= out_cap)
        return -1;
    out[off] = '\0';
    return (int)off;
}

static int pop_protocol_line(char *out, size_t out_cap, size_t *out_len)
{
    char *crlf = memmem(rx_buf, rx_len, "\r\n", 2);
    if (crlf == NULL)
        return 0;

    size_t line_len = (size_t)(crlf - rx_buf);
    if (line_len + 1 > out_cap)
        return -2;
    memcpy(out, rx_buf, line_len);
    out[line_len] = '\0';

    size_t consumed = line_len + 2;
    rx_len -= consumed;
    if (rx_len > 0)
        memmove(rx_buf, rx_buf + consumed, rx_len);
    *out_len = line_len;
    return 1;
}

static int read_protocol_line_until(char *out, size_t out_cap, size_t *out_len, int64_t deadline_us)
{
    for (;;)
    {
        int popped = pop_protocol_line(out, out_cap, out_len);
        if (popped != 0)
            return popped;
        if (rx_len >= sizeof(rx_buf))
            return -2;

        ssize_t n = net_recv(rx_buf + rx_len, sizeof(rx_buf) - rx_len);
        if (n > 0)
        {
            rx_len += (size_t)n;
            continue;
        }
        if (n == 0)
            return 0;
        if (n == -2)
        {
            int ready = wait_fd_ready(sock, !net_recv_want_write, net_recv_want_write, deadline_us);
            if (ready == 1)
                continue;
            return ready == 0 ? -2 : -1;
        }
        return -1;
    }
}

static int wait_for_connect_pong(int64_t deadline_us)
{
    char line[256];
    size_t line_len = 0;
    for (;;)
    {
        int rc = read_protocol_line_until(line, sizeof(line), &line_len, deadline_us);
        if (rc != 1)
        {
            ESP_LOGE(TAG, "wait(PONG) failed: rc=%d", rc);
            return -1;
        }

        if (line_len == 4 && memcmp(line, "PONG", 4) == 0)
            return 0;
        if (line_len == 4 && memcmp(line, "PING", 4) == 0)
        {
            if (net_send_all_until("PONG\r\n", 6, deadline_us) != 0)
                return -1;
            continue;
        }
        if (line_len >= 4 && memcmp(line, "-ERR", 4) == 0)
        {
            ESP_LOGW(TAG, "<- %.*s (backing off %llds)",
                     (int)(line_len > 120 ? 120 : line_len), line,
                     RECONNECT_AFTER_ERR_US / 1000000);
            backoff_until_us = esp_timer_get_time() + RECONNECT_AFTER_ERR_US;
            return -1;
        }
        if (line_len > 0)
            ESP_LOGI(TAG, "<- %.*s", (int)(line_len > 80 ? 80 : line_len), line);
    }
}

static int send_request_subs(int64_t deadline_us)
{
    const size_t request_count = tinyblok_patchbay_request_count();
    for (size_t i = 0; i < request_count; i++)
    {
        const char *subject = tinyblok_patchbay_request_subject(i);
        size_t subject_len = subject ? strlen(subject) : 0;
        if (!tinyblok_nats_subject_is_valid((const unsigned char *)subject, subject_len))
        {
            ESP_LOGE(TAG, "invalid request subject: %s", subject ? subject : "(null)");
            return -1;
        }
        char line[NATS_SUBJECT_MAX + 32];
        int n = snprintf(line, sizeof(line), "SUB %s %u\r\n", subject, (unsigned)(i + 1));
        if (n <= 0 || (size_t)n >= sizeof(line))
        {
            ESP_LOGE(TAG, "request subject too long: %s", subject);
            return -1;
        }
        if (net_send_all_until(line, (size_t)n, deadline_us) != 0)
            return -1;
        ESP_LOGI(TAG, "request '%s' subscribed", subject);
    }
    return 0;
}

int tinyblok_nats_reply(const unsigned char *subject, size_t subject_len,
                        const unsigned char *payload, size_t payload_len)
{
    if (!NET_OPEN() || !tinyblok_nats_subject_is_valid(subject, subject_len) ||
        payload_len > NATS_MSG_PAYLOAD_MAX)
        return -1;

    unsigned char frame[NATS_CONTROL_TX_CAP];
    int n = snprintf((char *)frame, sizeof(frame), "PUB %.*s %u\r\n",
                     (int)subject_len, (const char *)subject, (unsigned)payload_len);
    if (n <= 0 || (size_t)n >= sizeof(frame))
        return -1;
    size_t off = (size_t)n;
    if (payload_len > sizeof(frame) - off - 2)
        return -1;
    if (payload_len > 0)
    {
        memcpy(frame + off, payload, payload_len);
        off += payload_len;
    }
    frame[off++] = '\r';
    frame[off++] = '\n';
    if (queue_control_frame(frame, off) != 0)
    {
        close_sock(1);
        return -1;
    }
    return 0;
}

static int try_connect_once(int verbose_failure)
{
    if (load_runtime_config() != 0)
        return -1;
    const char *host = runtime_host();
    int port = runtime_port();
    char peer_ip[16] = "";

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);
    rx_len = 0;
    reset_control_tx();

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

    int fd = -1;
    int last_errno = 0;
    int64_t connect_deadline_us = deadline_from_now_ms(NATS_CONNECT_TIMEOUT_MS);
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next)
    {
        if (esp_timer_get_time() >= connect_deadline_us)
        {
            last_errno = ETIMEDOUT;
            break;
        }

        int candidate = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (candidate < 0)
        {
            last_errno = errno;
            continue;
        }
        set_socket_timeouts(candidate);
        if (set_fd_nonblocking(candidate) != 0)
        {
            last_errno = errno;
            close(candidate);
            continue;
        }

        if (connect_with_deadline(candidate, ai->ai_addr, ai->ai_addrlen, connect_deadline_us) != 0)
        {
            last_errno = errno;
            close(candidate);
            continue;
        }

        if (ai->ai_family == AF_INET)
        {
            const struct sockaddr_in *addr = (const struct sockaddr_in *)ai->ai_addr;
            snprintf(peer_ip, sizeof(peer_ip), "%s", inet_ntoa(addr->sin_addr));
        }
        fd = candidate;
        break;
    }
    freeaddrinfo(res);
    if (fd < 0)
    {
        if (verbose_failure)
            ESP_LOGE(TAG, "connect(%s:%s) failed: errno=%d", host, port_str, last_errno);
        return -1;
    }
    sock = fd;

    // INFO arrives in plaintext on accept, before any TLS upgrade.
    static char info_buf[768];
    size_t info_len = 0;
    int line_rc = read_protocol_line_until(info_buf, sizeof(info_buf), &info_len,
                                           deadline_from_now_ms(NATS_IO_TIMEOUT_MS));
    if (line_rc != 1 || info_len < 5 || memcmp(info_buf, "INFO ", 5) != 0)
    {
        if (verbose_failure)
            ESP_LOGE(TAG, "recv(INFO) failed: rc=%d len=%u", line_rc, (unsigned)info_len);
        net_close();
        return -1;
    }

#if CONFIG_TINYBLOK_NATS_TLS
    // We don't inspect INFO's "tls_required"; the handshake will fail
    // loudly if the broker isn't expecting TLS.
    if (runtime_tls_enabled() && rx_len != 0)
    {
        ESP_LOGE(TAG, "unexpected plaintext after INFO before TLS upgrade");
        net_close();
        return -1;
    }
    if (runtime_tls_enabled() && tls_init_once(host) != 0)
    {
        net_close();
        return -1;
    }
    if (runtime_tls_enabled() && tls_handshake() != 0)
    {
        net_close();
        return -1;
    }
    tls_active = runtime_tls_enabled();
#endif

    int64_t setup_deadline_us = deadline_from_now_ms(NATS_HANDSHAKE_TIMEOUT_MS);
    static char connect_line[2560];
    int connect_len = build_connect_line(info_buf, info_len, connect_line, sizeof(connect_line));
    if (connect_len < 0)
    {
        if (verbose_failure)
            ESP_LOGE(TAG, "could not build CONNECT line");
        net_close();
        return -1;
    }
    if (net_send_all_until(connect_line, (size_t)connect_len, setup_deadline_us) != 0)
    {
        if (verbose_failure)
            ESP_LOGE(TAG, "send(CONNECT) failed");
        net_close();
        return -1;
    }
    if (net_send_all_until("PING\r\n", 6, setup_deadline_us) != 0 ||
        wait_for_connect_pong(setup_deadline_us) != 0)
    {
        if (verbose_failure)
            ESP_LOGE(TAG, "connect PING/PONG failed");
        net_close();
        return -1;
    }
    if (send_request_subs(setup_deadline_us) != 0)
    {
        if (verbose_failure)
            ESP_LOGE(TAG, "send(SUB) failed");
        net_close();
        return -1;
    }

    ESP_LOGI(TAG, "connected %s:%d%s", host, port, runtime_tls_enabled() ? " (tls)" : "");
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
#if CONFIG_TINYBLOK_NATS_CREDS_SUPPORT
    if (load_runtime_config() != 0)
        return -1;
#ifdef ESP_PLATFORM
    if (strcmp(nats_cfg.nats_auth, "creds") == 0)
    {
#endif
        if (tinyblok_creds_load() != 0)
        {
            ESP_LOGE(TAG, "creds load failed; aborting connect");
            return -1;
        }
#ifdef ESP_PLATFORM
    }
#endif
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
    if (!tinyblok_nats_subject_is_valid(msg->subject, msg->subject_len))
        return -2;
    if (ntok == 3)
    {
        msg->reply = (const unsigned char *)"";
        msg->reply_len = 0;
        return parse_size(tok[2], tok_len[2], NATS_MSG_PAYLOAD_MAX, &msg->payload_len);
    }

    msg->reply = (const unsigned char *)tok[2];
    msg->reply_len = tok_len[2];
    if (!tinyblok_nats_subject_is_valid(msg->reply, msg->reply_len))
        return -2;
    return parse_size(tok[3], tok_len[3], NATS_MSG_PAYLOAD_MAX, &msg->payload_len);
}

void tinyblok_nats_drain_rx(void)
{
    if (!NET_OPEN())
        return;

    if (control_tx_pending())
    {
        int control_rc = drain_control_tx();
        if (control_rc < 0)
        {
            close_sock(1);
            return;
        }
    }

    int peer_closed = 0;
    for (size_t reads = 0; reads < NATS_RX_READS_PER_DRAIN; reads++)
    {
        if (rx_len >= sizeof(rx_buf))
        {
            ESP_LOGW(TAG, "rx buffer overflow");
            close_sock(1);
            return;
        }
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
                tinyblok_patchbay_handle_msg(msg.subject, msg.subject_len,
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
            static const unsigned char pong[] = "PONG\r\n";
            if (queue_control_frame(pong, sizeof(pong) - 1) != 0)
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
            close_sock(1);
            return;
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
    if (control_tx_pending())
    {
        int control_rc = drain_control_tx();
        if (control_rc < 0)
        {
            close_sock(1);
            return -1;
        }
        if (control_rc == 0)
            return 0;
    }
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
