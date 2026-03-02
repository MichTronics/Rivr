/*
 * crypto/hmac_sha256.c — Self-contained SHA-256 + HMAC-SHA-256 for RIVR.
 *
 * SHA-256 core adapted from Brad Conte's public-domain implementation.
 * (https://github.com/B-Con/crypto-algorithms, CC0 / public domain)
 * HMAC layer follows RFC 2104.
 *
 * No external dependencies — no mbedTLS, no ESP-IDF crypto component.
 * No heap allocation — all state is on the caller's stack.
 */

#include "hmac_sha256.h"
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * SHA-256 core
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Portable rotate-right for 32-bit words. */
#define ROR32(x, n) (((x) >> (n)) | ((x) << (32u - (n))))

/* SHA-256 auxiliary functions. */
#define CH(x,y,z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x)     (ROR32(x, 2u)  ^ ROR32(x, 13u) ^ ROR32(x, 22u))
#define EP1(x)     (ROR32(x, 6u)  ^ ROR32(x, 11u) ^ ROR32(x, 25u))
#define SIG0(x)    (ROR32(x, 7u)  ^ ROR32(x, 18u) ^ ((x) >> 3u))
#define SIG1(x)    (ROR32(x, 17u) ^ ROR32(x, 19u) ^ ((x) >> 10u))

/* SHA-256 round constants (first 32 bits of cube roots of first 64 primes). */
static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

/* SHA-256 working-state context (stack-allocated by callers). */
typedef struct {
    uint8_t  block[64];   /* Current partial block                          */
    uint32_t state[8];    /* Running hash state h0..h7                      */
    uint64_t bitlen;      /* Total message bit-length processed so far      */
    uint32_t datalen;     /* Bytes in current partial block (0..63)         */
} rivr_sha256_ctx_t;

/* Process one 64-byte block. */
static void sha256_transform(rivr_sha256_ctx_t *ctx, const uint8_t data[64])
{
    uint32_t a, b, c, d, e, f, g, h, t1, t2, m[64];
    uint32_t i, j;

    for (i = 0, j = 0; i < 16u; i++, j += 4u) {
        m[i] = ((uint32_t)data[j    ] << 24u)
             | ((uint32_t)data[j + 1] << 16u)
             | ((uint32_t)data[j + 2] <<  8u)
             | ((uint32_t)data[j + 3]);
    }
    for (; i < 64u; i++) {
        m[i] = SIG1(m[i - 2u]) + m[i - 7u] + SIG0(m[i - 15u]) + m[i - 16u];
    }

    a = ctx->state[0]; b = ctx->state[1];
    c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5];
    g = ctx->state[6]; h = ctx->state[7];

    for (i = 0; i < 64u; i++) {
        t1 = h + EP1(e) + CH(e, f, g) + K[i] + m[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b;
    ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f;
    ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(rivr_sha256_ctx_t *ctx)
{
    ctx->datalen = 0u;
    ctx->bitlen  = 0u;
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
}

static void sha256_update(rivr_sha256_ctx_t *ctx, const uint8_t *data, size_t len)
{
    for (size_t i = 0u; i < len; i++) {
        ctx->block[ctx->datalen] = data[i];
        ctx->datalen++;
        if (ctx->datalen == 64u) {
            sha256_transform(ctx, ctx->block);
            ctx->bitlen += 512u;
            ctx->datalen = 0u;
        }
    }
}

static void sha256_final(rivr_sha256_ctx_t *ctx, uint8_t out[32])
{
    uint32_t i = ctx->datalen;

    /* Append 0x80 padding byte, then zero bytes up to the length field. */
    ctx->block[i++] = 0x80u;
    if (ctx->datalen < 56u) {
        while (i < 56u) { ctx->block[i++] = 0x00u; }
    } else {
        while (i < 64u) { ctx->block[i++] = 0x00u; }
        sha256_transform(ctx, ctx->block);
        memset(ctx->block, 0, 56u);
    }

    /* Append total message bit-length as big-endian 64-bit value. */
    ctx->bitlen += (uint64_t)ctx->datalen * 8u;
    ctx->block[63] = (uint8_t) ctx->bitlen;
    ctx->block[62] = (uint8_t)(ctx->bitlen >>  8u);
    ctx->block[61] = (uint8_t)(ctx->bitlen >> 16u);
    ctx->block[60] = (uint8_t)(ctx->bitlen >> 24u);
    ctx->block[59] = (uint8_t)(ctx->bitlen >> 32u);
    ctx->block[58] = (uint8_t)(ctx->bitlen >> 40u);
    ctx->block[57] = (uint8_t)(ctx->bitlen >> 48u);
    ctx->block[56] = (uint8_t)(ctx->bitlen >> 56u);
    sha256_transform(ctx, ctx->block);

    /* Produce big-endian digest. */
    for (i = 0u; i < 4u; i++) {
        out[i     ] = (uint8_t)(ctx->state[0] >> (24u - i * 8u));
        out[i +  4] = (uint8_t)(ctx->state[1] >> (24u - i * 8u));
        out[i +  8] = (uint8_t)(ctx->state[2] >> (24u - i * 8u));
        out[i + 12] = (uint8_t)(ctx->state[3] >> (24u - i * 8u));
        out[i + 16] = (uint8_t)(ctx->state[4] >> (24u - i * 8u));
        out[i + 20] = (uint8_t)(ctx->state[5] >> (24u - i * 8u));
        out[i + 24] = (uint8_t)(ctx->state[6] >> (24u - i * 8u));
        out[i + 28] = (uint8_t)(ctx->state[7] >> (24u - i * 8u));
    }
}

/* ── Public: SHA-256 ──────────────────────────────────────────────────────── */

void rivr_sha256(const uint8_t *data, size_t len, uint8_t out[RIVR_SHA256_DIGEST_LEN])
{
    rivr_sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * HMAC-SHA-256  (RFC 2104)
 * ═══════════════════════════════════════════════════════════════════════════ */

void rivr_hmac_sha256(const uint8_t *key, size_t key_len,
                      const uint8_t *msg, size_t msg_len,
                      uint8_t out[RIVR_HMAC_SHA256_LEN])
{
    /* All temporaries stack-allocated. */
    uint8_t           k_norm[64]; /* key normalised to ≤ 64 bytes            */
    uint8_t           ikey[64];   /* k_norm XOR ipad (0x36)                  */
    uint8_t           okey[64];   /* k_norm XOR opad (0x5c)                  */
    uint8_t           inner[32];  /* inner hash: H((k XOR ipad) ∥ msg)       */
    rivr_sha256_ctx_t ctx;
    uint32_t          i;

    /* Step 1: derive k_norm.
     * If key is longer than the block size (64 B), hash it down to 32 B.
     * Otherwise pad with zeros to 64 bytes. */
    memset(k_norm, 0, sizeof(k_norm));
    if (key_len > 64u) {
        rivr_sha256(key, key_len, k_norm);   /* result is 32 B, remaining 32 stay 0 */
    } else {
        memcpy(k_norm, key, key_len);
    }

    /* Step 2: construct ikey and okey. */
    for (i = 0u; i < 64u; i++) {
        ikey[i] = k_norm[i] ^ 0x36u;
        okey[i] = k_norm[i] ^ 0x5cu;
    }

    /* Step 3: inner hash = H((k XOR ipad) ∥ msg). */
    sha256_init(&ctx);
    sha256_update(&ctx, ikey, 64u);
    sha256_update(&ctx, msg,  msg_len);
    sha256_final(&ctx, inner);

    /* Step 4: outer hash = H((k XOR opad) ∥ inner). */
    sha256_init(&ctx);
    sha256_update(&ctx, okey,  64u);
    sha256_update(&ctx, inner, 32u);
    sha256_final(&ctx, out);
}
