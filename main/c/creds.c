// Parser for NATS .creds files (JWT block + NKey-seed block) and the
// Ed25519 nonce-signing primitive that goes with them.
#include "creds.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "sdkconfig.h"

#ifndef CONFIG_TINYBLOK_NATS_CREDS_SUPPORT
#define CONFIG_TINYBLOK_NATS_CREDS_SUPPORT 0
#endif

#if CONFIG_TINYBLOK_NATS_CREDS_SUPPORT

#ifdef ESP_PLATFORM
#include "tinyblok_config.h"
#endif

extern int tinyblok_ed25519_sign(const unsigned char seed[32],
                                 const unsigned char *msg, size_t msg_len,
                                 unsigned char sig_out[64]);

static const char *TAG = "creds";

#ifndef ESP_PLATFORM
// Linker symbols populated by EMBED_TXTFILES in host/legacy builds.
extern const char nats_creds_start[] asm("_binary_nats_creds_start");
extern const char nats_creds_end[] asm("_binary_nats_creds_end");
#endif

static tinyblok_creds_t g_creds;
static char g_jwt_buf[2048];
static int g_loaded = 0;

static int is_ws(char c) { return c == '\r' || c == '\n' || c == ' ' || c == '\t'; }

// Find the body between "-----BEGIN <label>-----" and the next "-----END"
// marker, whitespace-trimmed. Returns -1 if either marker is missing.
static int extract_block(const char *haystack, size_t haystack_len,
                         const char *label, const char **out_start,
                         size_t *out_len)
{
    char begin_marker[64];
    int n = snprintf(begin_marker, sizeof(begin_marker), "-----BEGIN %s-----", label);
    if (n <= 0 || (size_t)n >= sizeof(begin_marker))
        return -1;

    const char *begin = memmem(haystack, haystack_len, begin_marker, (size_t)n);
    if (!begin)
        return -1;
    const char *body = begin + n;
    size_t remaining = haystack_len - (size_t)(body - haystack);

    // nsc emits 6 dashes, the spec example uses 5; match the longer form
    // first so we don't stop one byte inside a 6-dash sentinel.
    const char *end = memmem(body, remaining, "------END", 9);
    if (!end)
    {
        end = memmem(body, remaining, "-----END", 8);
        if (!end)
            return -1;
    }

    while (body < end && is_ws(*body))
        body++;
    const char *tail = end;
    while (tail > body && is_ws(tail[-1]))
        tail--;

    *out_start = body;
    *out_len = (size_t)(tail - body);
    return 0;
}

// RFC 4648 base32, no padding. Whitespace and '=' are skipped.
// Returns number of decoded bytes, or -1 on bad character / overflow.
static int base32_decode(const char *in, size_t in_len, unsigned char *out, size_t out_cap)
{
    uint32_t buf = 0;
    int bits = 0;
    size_t out_len = 0;
    for (size_t i = 0; i < in_len; i++)
    {
        char c = in[i];
        if (is_ws(c) || c == '=')
            continue;
        int v;
        if (c >= 'A' && c <= 'Z')
            v = c - 'A';
        else if (c >= 'a' && c <= 'z')
            v = c - 'a';
        else if (c >= '2' && c <= '7')
            v = 26 + (c - '2');
        else
            return -1;
        buf = (buf << 5) | (uint32_t)v;
        bits += 5;
        if (bits >= 8)
        {
            bits -= 8;
            if (out_len >= out_cap)
                return -1;
            out[out_len++] = (unsigned char)((buf >> bits) & 0xFF);
        }
    }
    return (int)out_len;
}

static uint16_t crc16_xmodem(const unsigned char *data, size_t len)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < len; i++)
    {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

static const char b64url_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

size_t tinyblok_b64url_encode(const unsigned char *in, size_t len, char *out)
{
    size_t i = 0, o = 0;
    while (i + 3 <= len)
    {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | (uint32_t)in[i + 2];
        out[o++] = b64url_alphabet[(v >> 18) & 0x3F];
        out[o++] = b64url_alphabet[(v >> 12) & 0x3F];
        out[o++] = b64url_alphabet[(v >> 6) & 0x3F];
        out[o++] = b64url_alphabet[v & 0x3F];
        i += 3;
    }
    if (i + 1 == len)
    {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = b64url_alphabet[(v >> 18) & 0x3F];
        out[o++] = b64url_alphabet[(v >> 12) & 0x3F];
    }
    else if (i + 2 == len)
    {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[o++] = b64url_alphabet[(v >> 18) & 0x3F];
        out[o++] = b64url_alphabet[(v >> 12) & 0x3F];
        out[o++] = b64url_alphabet[(v >> 6) & 0x3F];
    }
    out[o] = '\0';
    return o;
}

int tinyblok_creds_load(void)
{
    if (g_loaded)
        return 0;

#ifdef ESP_PLATFORM
    tinyblok_config_t cfg;
    esp_err_t err = tinyblok_config_load(&cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "config load failed: %s", esp_err_to_name(err));
        return -1;
    }
    const char *blob = cfg.nats_creds;
    size_t blob_len = strnlen(cfg.nats_creds, sizeof(cfg.nats_creds));
    if (blob_len == 0)
    {
        ESP_LOGE(TAG, "no NATS .creds file stored");
        return -1;
    }
#else
    const char *blob = nats_creds_start;
    size_t blob_len = (size_t)(nats_creds_end - nats_creds_start);
    if (blob_len > 0 && blob[blob_len - 1] == '\0')
        blob_len--;
#endif

    const char *jwt_start;
    size_t jwt_len;
    if (extract_block(blob, blob_len, "NATS USER JWT", &jwt_start, &jwt_len) != 0)
    {
        ESP_LOGE(TAG, "could not find NATS USER JWT block");
        return -1;
    }

    if (jwt_len + 1 > sizeof(g_jwt_buf))
    {
        ESP_LOGE(TAG, "JWT (%u bytes) exceeds buffer (%u)", (unsigned)jwt_len,
                 (unsigned)sizeof(g_jwt_buf));
        return -1;
    }
    size_t out = 0;
    for (size_t i = 0; i < jwt_len; i++)
    {
        char c = jwt_start[i];
        if (is_ws(c))
            continue;
        g_jwt_buf[out++] = c;
    }
    g_jwt_buf[out] = '\0';
    g_creds.jwt = g_jwt_buf;
    g_creds.jwt_len = out;

    const char *seed_start;
    size_t seed_len;
    if (extract_block(blob, blob_len, "USER NKEY SEED", &seed_start, &seed_len) != 0)
    {
        ESP_LOGE(TAG, "could not find USER NKEY SEED block");
        return -1;
    }

    // nats-io/nkeys decodeSeed layout: 2-byte prefix, 32-byte seed,
    // 2-byte CRC16/XMODEM little-endian over the first 34 bytes.
    unsigned char raw[40];
    int decoded = base32_decode(seed_start, seed_len, raw, sizeof(raw));
    if (decoded != 36)
    {
        ESP_LOGE(TAG, "seed base32 decode wrong length: %d (expected 36)", decoded);
        return -1;
    }

    uint16_t want = crc16_xmodem(raw, 34);
    uint16_t have = (uint16_t)raw[34] | ((uint16_t)raw[35] << 8);
    if (want != have)
    {
        ESP_LOGE(TAG, "seed CRC mismatch: want=%04x have=%04x", want, have);
        return -1;
    }

    memcpy(g_creds.seed, &raw[2], 32);

    g_loaded = 1;
    ESP_LOGI(TAG, "loaded creds: jwt=%u bytes, seed ok", (unsigned)g_creds.jwt_len);
    return 0;
}

const tinyblok_creds_t *tinyblok_creds_get(void)
{
    return g_loaded ? &g_creds : NULL;
}

int tinyblok_creds_sign(const unsigned char *msg, size_t msg_len, unsigned char sig_out[64])
{
    if (!g_loaded)
        return -1;

    return tinyblok_ed25519_sign(g_creds.seed, msg, msg_len, sig_out);
}

#else // CONFIG_TINYBLOK_NATS_CREDS_SUPPORT

// Link-clean stubs for builds without creds auth.
int tinyblok_creds_load(void) { return -1; }
const tinyblok_creds_t *tinyblok_creds_get(void) { return NULL; }
int tinyblok_creds_sign(const unsigned char *msg, size_t msg_len, unsigned char sig_out[64])
{
    (void)msg;
    (void)msg_len;
    (void)sig_out;
    return -1;
}
size_t tinyblok_b64url_encode(const unsigned char *in, size_t len, char *out)
{
    (void)in;
    (void)len;
    if (out)
        *out = '\0';
    return 0;
}

#endif // CONFIG_TINYBLOK_NATS_CREDS_SUPPORT
