// Deterministic Ed25519 sign-from-seed wrapper around vendored TweetNaCl.
// NATS .creds auth needs RFC 8032 Ed25519 (SHA-512). TweetNaCl's
// crypto_sign_ed25519 implements that exactly.
//
// We don't need crypto_sign_keypair (which would require randombytes), only
// signing with a known seed. The keypair-from-seed step here mirrors what
// TweetNaCl's crypto_sign_keypair does internally, minus the RNG read.
//
// randombytes() must still be defined for the linker. It's never called on
// our paths, but if some unused TweetNaCl entrypoint references it the
// linker would otherwise drop the whole TU. Stub returns nothing useful.
#include "tweetnacl.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_random.h"

void randombytes(unsigned char *x, unsigned long long len)
{
    // Routed to the hardware RNG so this is a real CSPRNG if anything
    // ever does call it. Keygen is not on our path today.
    while (len > 0)
    {
        size_t chunk = len > 1024 ? 1024 : (size_t)len;
        esp_fill_random(x, chunk);
        x += chunk;
        len -= chunk;
    }
}

// Internal TweetNaCl primitives we need to reuse. They're declared `static`
// in tweetnacl.c as `sv` (`static void`), so we can't call them directly —
// instead we do the work ourselves, computing the public key via crypto_hash
// + the basepoint scalarmult primitive. But since those are also static, we
// take the easier path: feed crypto_sign with a 64-byte secret key
// `(seed || pk)`, with pk computed by running a one-shot keypair derivation.
//
// crypto_sign expects the 64-byte secret key in TweetNaCl's format:
//   bytes 0..31 = seed
//   bytes 32..63 = public key (32 bytes, little-endian compressed point)
//
// We derive pk by calling crypto_sign with a *known answer test* setup:
// give it a placeholder pk slot, sign a dummy message, and read pk out from
// the resulting signed message. This is wasteful but only happens once at
// boot, and avoids ripping internals out of tweetnacl.c.
//
// Actually simpler: re-call crypto_sign each time. The 32-byte public key
// derivation cost is one SHA-512 + one scalar-mult, both folded inside
// crypto_sign's hashing pass. The signing function recomputes pk from sk
// implicitly via the secret-key layout, so as long as we feed it the
// (seed, pk) pair we computed once, it works. We compute that pair once
// at startup via TweetNaCl's keypair function, but seeded.

// crypto_hash_sha512 — exposed via tweetnacl.h
extern int crypto_hash(unsigned char *out, const unsigned char *m, unsigned long long n);

// To derive pk from seed without using randombytes, we need the same
// scalar/clamping/scalarbase steps that crypto_sign_keypair uses. Those
// helpers are file-static in tweetnacl.c. The cleanest path: monkeypatch.
// We instead temporarily override randombytes via a thread-local pointer,
// but that's ugly too.
//
// Cleanest: add ONE small public function to tweetnacl.c (see the patch in
// this same commit). The function takes a pre-existing seed in sk[0..31],
// runs the same hash+clamp+scalarbase+pack as crypto_sign_keypair, and
// writes pk to both sk[32..63] and the pk argument. We declare it here.
extern int crypto_sign_seed_keypair(unsigned char *pk, unsigned char *sk,
                                    const unsigned char *seed);

int tinyblok_ed25519_sign(const unsigned char seed[32],
                          const unsigned char *msg, size_t msg_len,
                          unsigned char sig_out[64])
{
    unsigned char pk[32];
    unsigned char sk[64];
    if (crypto_sign_seed_keypair(pk, sk, seed) != 0)
        return -1;

    // crypto_sign produces a signed message: sig (64) || msg. We only want
    // the sig. Static (not stack) because TweetNaCl's signing path already
    // uses several KB of stack via nested gf[16] arrays — adding 320 more
    // bytes here was enough to overflow the main task stack on C6.
    // NATS nonces are ~16-22 chars, well under the 256-byte cap.
    if (msg_len > 256)
        return -1;
    static unsigned char sm[64 + 256];
    unsigned long long smlen = 0;
    int rc = crypto_sign(sm, &smlen, msg, (unsigned long long)msg_len, sk);

    // Wipe sk; pk is public.
    volatile unsigned char *p = sk;
    for (size_t i = 0; i < sizeof(sk); i++)
        p[i] = 0;

    if (rc != 0 || smlen != 64 + msg_len)
        return -1;
    memcpy(sig_out, sm, 64);
    return 0;
}
