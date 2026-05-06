#pragma once

#include <stddef.h>

// Result of parsing an embedded .creds file. Lifetime is process-long;
// pointers reference static buffers populated once at boot.
typedef struct
{
    const char *jwt;       // base64url JWT string, NUL-terminated
    size_t jwt_len;        // strlen(jwt)
    unsigned char seed[32]; // raw Ed25519 private key seed
} tinyblok_creds_t;

// Parse the .creds file embedded in the firmware. Returns 0 on success.
// Logs and returns -1 on any structural / checksum / decode failure.
// Idempotent; cached after the first successful call.
int tinyblok_creds_load(void);

// Accessor for parsed creds. Returns NULL until tinyblok_creds_load() succeeds.
const tinyblok_creds_t *tinyblok_creds_get(void);

// Sign `msg` with the loaded user seed (Ed25519). Writes 64 raw signature
// bytes to `sig_out`. Returns 0 on success.
int tinyblok_creds_sign(const unsigned char *msg, size_t msg_len,
                        unsigned char sig_out[64]);

// base64url-encode (no padding) `len` bytes of `in` into `out`. `out` must
// have room for at least 4 * ceil(len/3) bytes plus a trailing NUL.
// Returns the number of bytes written (excluding NUL).
size_t tinyblok_b64url_encode(const unsigned char *in, size_t len, char *out);
