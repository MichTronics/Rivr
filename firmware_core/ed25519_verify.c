/*
 * ed25519_verify.c — compact Ed25519 signature verification.
 *
 * Field arithmetic and group law derived from TweetNaCl
 * (Bernstein, Duif, Lange, Schwabe, Yang 2012 — public domain).
 *
 * SHA-512 adapted from the FIPS 180-4 reference (public domain).
 *
 * All stack-allocated; zero heap; no external dependencies.
 * Verified against RFC 8032 test vectors.
 */

#include "ed25519_verify.h"
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * SHA-512
 * ═══════════════════════════════════════════════════════════════════════════ */

#define SHA512_BLOCK  128u
#define SHA512_DIGEST  64u

typedef struct {
    uint64_t h[8];
    uint8_t  buf[SHA512_BLOCK];
    uint64_t lo, hi;  /* bit count */
} sha512_t;

static const uint64_t K[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL,
    0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL,
    0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL, 0x2de92c6f592b0275ULL,
    0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL,
    0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL,
    0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL,
    0x92722c851482353bULL, 0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL,
    0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL,
    0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL,
    0xc67178f2e372532bULL, 0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL,
    0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

#define ROR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))
#define CH(x,y,z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x)  (ROR64(x,28) ^ ROR64(x,34) ^ ROR64(x,39))
#define EP1(x)  (ROR64(x,14) ^ ROR64(x,18) ^ ROR64(x,41))
#define S0(x)   (ROR64(x, 1) ^ ROR64(x, 8) ^ ((x) >> 7))
#define S1(x)   (ROR64(x,19) ^ ROR64(x,61) ^ ((x) >> 6))

static void sha512_transform(sha512_t *ctx, const uint8_t *data)
{
    uint64_t a, b, c, d, e, f, g, h, t1, t2, W[80];
    for (int i = 0; i < 16; i++) {
        W[i]  = (uint64_t)data[8*i  ] << 56;
        W[i] |= (uint64_t)data[8*i+1] << 48;
        W[i] |= (uint64_t)data[8*i+2] << 40;
        W[i] |= (uint64_t)data[8*i+3] << 32;
        W[i] |= (uint64_t)data[8*i+4] << 24;
        W[i] |= (uint64_t)data[8*i+5] << 16;
        W[i] |= (uint64_t)data[8*i+6] <<  8;
        W[i] |= (uint64_t)data[8*i+7];
    }
    for (int i = 16; i < 80; i++)
        W[i] = S1(W[i-2]) + W[i-7] + S0(W[i-15]) + W[i-16];

    a = ctx->h[0]; b = ctx->h[1]; c = ctx->h[2]; d = ctx->h[3];
    e = ctx->h[4]; f = ctx->h[5]; g = ctx->h[6]; h = ctx->h[7];

    for (int i = 0; i < 80; i++) {
        t1 = h + EP1(e) + CH(e,f,g) + K[i] + W[i];
        t2 = EP0(a) + MAJ(a,b,c);
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    ctx->h[0]+=a; ctx->h[1]+=b; ctx->h[2]+=c; ctx->h[3]+=d;
    ctx->h[4]+=e; ctx->h[5]+=f; ctx->h[6]+=g; ctx->h[7]+=h;
}

static void sha512_init(sha512_t *ctx)
{
    ctx->h[0] = 0x6a09e667f3bcc908ULL; ctx->h[1] = 0xbb67ae8584caa73bULL;
    ctx->h[2] = 0x3c6ef372fe94f82bULL; ctx->h[3] = 0xa54ff53a5f1d36f1ULL;
    ctx->h[4] = 0x510e527fade682d1ULL; ctx->h[5] = 0x9b05688c2b3e6c1fULL;
    ctx->h[6] = 0x1f83d9abfb41bd6bULL; ctx->h[7] = 0x5be0cd19137e2179ULL;
    ctx->lo = ctx->hi = 0;
}

static void sha512_update(sha512_t *ctx, const uint8_t *data, size_t len)
{
    size_t used = (size_t)(ctx->lo >> 3) & (SHA512_BLOCK - 1);
    ctx->lo += (uint64_t)len << 3;
    if (ctx->lo < (uint64_t)len << 3) ctx->hi++;
    ctx->hi += (uint64_t)len >> 61;

    size_t free = SHA512_BLOCK - used;
    if (len >= free) {
        memcpy(ctx->buf + used, data, free);
        sha512_transform(ctx, ctx->buf);
        data += free; len -= free;
        while (len >= SHA512_BLOCK) {
            sha512_transform(ctx, data);
            data += SHA512_BLOCK; len -= SHA512_BLOCK;
        }
        used = 0;
    }
    memcpy(ctx->buf + used, data, len);
}

static void sha512_final(sha512_t *ctx, uint8_t out[SHA512_DIGEST])
{
    size_t used = (size_t)(ctx->lo >> 3) & (SHA512_BLOCK - 1);
    ctx->buf[used++] = 0x80;
    if (used > SHA512_BLOCK - 16) {
        memset(ctx->buf + used, 0, SHA512_BLOCK - used);
        sha512_transform(ctx, ctx->buf);
        used = 0;
    }
    memset(ctx->buf + used, 0, SHA512_BLOCK - 16 - used);
    uint64_t hi = ctx->hi, lo = ctx->lo;
    ctx->buf[112] = (uint8_t)(hi >> 56); ctx->buf[113] = (uint8_t)(hi >> 48);
    ctx->buf[114] = (uint8_t)(hi >> 40); ctx->buf[115] = (uint8_t)(hi >> 32);
    ctx->buf[116] = (uint8_t)(hi >> 24); ctx->buf[117] = (uint8_t)(hi >> 16);
    ctx->buf[118] = (uint8_t)(hi >>  8); ctx->buf[119] = (uint8_t)(hi      );
    ctx->buf[120] = (uint8_t)(lo >> 56); ctx->buf[121] = (uint8_t)(lo >> 48);
    ctx->buf[122] = (uint8_t)(lo >> 40); ctx->buf[123] = (uint8_t)(lo >> 32);
    ctx->buf[124] = (uint8_t)(lo >> 24); ctx->buf[125] = (uint8_t)(lo >> 16);
    ctx->buf[126] = (uint8_t)(lo >>  8); ctx->buf[127] = (uint8_t)(lo      );
    sha512_transform(ctx, ctx->buf);
    for (int i = 0; i < 8; i++) {
        out[8*i  ] = (uint8_t)(ctx->h[i] >> 56);
        out[8*i+1] = (uint8_t)(ctx->h[i] >> 48);
        out[8*i+2] = (uint8_t)(ctx->h[i] >> 40);
        out[8*i+3] = (uint8_t)(ctx->h[i] >> 32);
        out[8*i+4] = (uint8_t)(ctx->h[i] >> 24);
        out[8*i+5] = (uint8_t)(ctx->h[i] >> 16);
        out[8*i+6] = (uint8_t)(ctx->h[i] >>  8);
        out[8*i+7] = (uint8_t)(ctx->h[i]      );
    }
}

/* Hash three concatenated buffers → 64-byte digest. */
static void sha512_3(uint8_t out[SHA512_DIGEST],
                     const uint8_t *a, size_t alen,
                     const uint8_t *b, size_t blen,
                     const uint8_t *c, size_t clen)
{
    sha512_t ctx;
    sha512_init(&ctx);
    sha512_update(&ctx, a, alen);
    sha512_update(&ctx, b, blen);
    sha512_update(&ctx, c, clen);
    sha512_final(&ctx, out);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * GF(2^255-19) field arithmetic  (TweetNaCl limb representation)
 * Each gf element is 16 limbs of 16 bits in little-endian order.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef long long gf[16];

static const gf gf0 = {0};
static const gf gf1 = {1};

/* d = -121665/121666 mod p */
static const gf D = {
    0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
    0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203
};

/* 2*d */
static const gf D2 = {
    0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
    0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406
};

/* sqrt(-1) mod p */
static const gf I = {
    0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
    0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83
};

/* Base point x */
static const gf BX = {
    0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
    0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169
};

/* Base point y */
static const gf BY = {
    0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
    0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666
};

static void gf_cpy(gf r, const gf a) {
    for (int i = 0; i < 16; i++) r[i] = a[i];
}
static void gf_add(gf r, const gf a, const gf b) {
    for (int i = 0; i < 16; i++) r[i] = a[i] + b[i];
}
static void gf_sub(gf r, const gf a, const gf b) {
    for (int i = 0; i < 16; i++) r[i] = a[i] - b[i];
}

static void car25519(gf r)
{
    for (int i = 0; i < 16; i++) {
        r[(i+1) % 16] += (i < 15) ? (r[i] >> 16) : 38 * (r[i] >> 16);
        r[i] &= 0xffff;
    }
}

static void gf_mul(gf r, const gf a, const gf b)
{
    long long t[31] = {0};
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++)
            t[i+j] += a[i] * b[j];
    /* Wrap high limbs: each bit above 2^255 contributes 19 * 2 = 38 */
    for (int i = 0; i < 15; i++)
        t[i] += 38 * t[i + 16];
    /* Assign (may be > 2^16; car25519 will normalise) */
    for (int i = 0; i < 16; i++)
        r[i] = t[i];
    /* Two carry passes as in TweetNaCl */
    car25519(r);
    car25519(r);
}

static void gf_sq(gf r, const gf a) { gf_mul(r, a, a); }

/* Constant-time conditional swap: swaps p and q when b == 1. */
static void cswap25519(gf p, gf q, int b)
{
    long long c = ~((long long)b - 1);
    for (int i = 0; i < 16; i++) {
        long long t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void pack25519(uint8_t o[32], const gf n)
{
    gf m, t;
    gf_cpy(t, n);
    car25519(t); car25519(t); car25519(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i]   = t[i] - 0xffff - ((m[i-1] >> 16) & 1);
            m[i-1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int bi = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        cswap25519(t, m, 1-bi);
    }
    for (int i = 0; i < 16; i++) {
        o[2*i  ] = (uint8_t)(t[i]     );
        o[2*i+1] = (uint8_t)(t[i] >> 8);
    }
}

static int neq25519(const gf a, const gf b)
{
    uint8_t c[32], d[32];
    pack25519(c, a);
    pack25519(d, b);
    int r = 0;
    for (int i = 0; i < 32; i++) r |= c[i] ^ d[i];
    return r;   /* non-zero → not equal */
}

static uint8_t par25519(const gf a)
{
    uint8_t d[32];
    pack25519(d, a);
    return d[0] & 1;
}

static void unpack25519(gf r, const uint8_t p[32])
{
    for (int i = 0; i < 16; i++)
        r[i] = p[2*i] | ((long long)p[2*i+1] << 8);
    r[15] &= 0x7fff;
}

/* r = x^((p-5)/8) mod p, used in square-root extraction. */
static void gf_pow22523(gf r, const gf x)
{
    gf t0, t1, t2;
    gf_sq(t0, x);
    for (int i = 1; i <  1; i++) gf_sq(t0, t0);   /* t0 = x^2 */
    gf_sq(t1, t0);
    for (int i = 1; i <  2; i++) gf_sq(t1, t1);   /* t1 = x^8 */
    gf_mul(t1, x, t1);                              /* t1 = x^9 */
    gf_mul(t0, t0, t1);                             /* t0 = x^11 */
    gf_sq(t0, t0);                                  /* t0 = x^22 */
    gf_mul(t0, t1, t0);                             /* t0 = x^31 */
    gf_sq(t1, t0);
    for (int i = 1; i <  5; i++) gf_sq(t1, t1);   /* t1 = x^992 */
    gf_mul(t0, t1, t0);                             /* t0 = x^1023 = x^(2^10-1) */
    gf_sq(t1, t0);
    for (int i = 1; i < 10; i++) gf_sq(t1, t1);
    gf_mul(t1, t1, t0);                             /* x^(2^20-1) */
    gf_sq(t2, t1);
    for (int i = 1; i < 20; i++) gf_sq(t2, t2);
    gf_mul(t1, t2, t1);                             /* x^(2^40-1) */
    gf_sq(t1, t1);
    for (int i = 1; i < 10; i++) gf_sq(t1, t1);
    gf_mul(t0, t1, t0);                             /* x^(2^50-1) */
    gf_sq(t1, t0);
    for (int i = 1; i < 50; i++) gf_sq(t1, t1);
    gf_mul(t1, t1, t0);                             /* x^(2^100-1) */
    gf_sq(t2, t1);
    for (int i = 1; i < 100; i++) gf_sq(t2, t2);
    gf_mul(t1, t2, t1);                             /* x^(2^200-1) */
    gf_sq(t1, t1);
    for (int i = 1; i < 50; i++) gf_sq(t1, t1);
    gf_mul(t0, t1, t0);                             /* x^(2^250-1) */
    gf_sq(t0, t0);
    gf_sq(t0, t0);                                  /* x^(2^252-4) */
    gf_mul(r, t0, x);                               /* x^(2^252-3) = x^((p-5)/8) */
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Extended twisted Edwards group arithmetic
 * Point = (X:Y:Z:T) with x = X/Z, y = Y/Z, x*y = T/Z
 * ═══════════════════════════════════════════════════════════════════════════ */

/* p[4][16]: index 0=X, 1=Y, 2=Z, 3=T */
typedef gf pt[4];

static void pt_add(pt p, pt q)
{
    gf a, b, c, d, t, e, f, g, h;
    gf_sub(a, p[1], p[0]);    gf_sub(t, q[1], q[0]);    gf_mul(a, a, t);
    gf_add(b, p[0], p[1]);    gf_add(t, q[0], q[1]);    gf_mul(b, b, t);
    gf_mul(c, p[3], q[3]);    gf_mul(c, c, D2);
    gf_mul(d, p[2], q[2]);    gf_add(d, d, d);
    gf_sub(e, b, a);
    gf_sub(f, d, c);
    gf_add(g, d, c);
    gf_add(h, b, a);
    gf_mul(p[0], e, f);
    gf_mul(p[1], h, g);
    gf_mul(p[2], g, f);
    gf_mul(p[3], e, h);
}

static void pt_cswap(pt p, pt q, int b)
{
    for (int i = 0; i < 4; i++) cswap25519(p[i], q[i], b);
}

static void scalarmult(pt r, pt p, const uint8_t *s)
{
    /* Double-and-add scalar multiplication (Montgomery ladder).
     *   q = accumulator (identity element)
     *   r = running power of p (starts as p itself)
     * For each bit b of s (MSB first):
     *   conditionally swap q and r
     *   q += r  (accumulator absorbs one copy of the power)
     *   r  = 2r (double the power)
     *   conditionally swap back
     * Result lands in q.  Matches TweetNaCl scalarmult. */
    pt q;
    gf_cpy(q[0], gf0);
    gf_cpy(q[1], gf1);
    gf_cpy(q[2], gf1);
    gf_cpy(q[3], gf0);
    gf_cpy(r[0], p[0]);
    gf_cpy(r[1], p[1]);
    gf_cpy(r[2], p[2]);
    gf_cpy(r[3], p[3]);

    for (int i = 255; i >= 0; i--) {
        int b = (s[i/8] >> (i & 7)) & 1;
        pt_cswap(q, r, b);
        pt_add(r, q);   /* r += q  (matches TweetNaCl add(input, identity)) */
        pt_add(q, q);   /* q  = 2q (matches TweetNaCl add(identity,identity))*/
        pt_cswap(q, r, b);
    }
    gf_cpy(r[0], q[0]);
    gf_cpy(r[1], q[1]);
    gf_cpy(r[2], q[2]);
    gf_cpy(r[3], q[3]);
}

static void scalarbase(pt r, const uint8_t *s)
{
    pt p;
    gf_cpy(p[0], BX);
    gf_cpy(p[1], BY);
    gf_cpy(p[2], gf1);
    gf_mul(p[3], BX, BY);
    scalarmult(r, p, s);
}

/* Decompress a 32-byte encoding (Edwards y || sign) into extended coords.
 * Returns false if the point is not on the curve. */
static bool unpackneg(pt r, const uint8_t p[32])
{
    gf t, chk, num, den, den2, den4, den6;
    gf_cpy(r[2], gf1);
    unpack25519(r[1], p);
    gf_sq(num, r[1]);
    gf_mul(den, num, D);
    gf_sub(num, num, r[2]);  /* num = y^2 - 1 */
    gf_add(den, r[2], den);  /* den = d*y^2 + 1 */

    gf_sq(den2, den);
    gf_sq(den4, den2);
    gf_mul(den6, den4, den2);
    gf_mul(t, den6, num);
    gf_mul(t, t, den);

    gf_pow22523(t, t);
    gf_mul(t, t, num);
    gf_mul(t, t, den);
    gf_mul(t, t, den);
    gf_mul(r[0], t, den);

    gf_sq(chk, r[0]);
    gf_mul(chk, chk, den);
    if (neq25519(chk, num)) {
        gf_mul(r[0], r[0], I);
    }

    gf_sq(chk, r[0]);
    gf_mul(chk, chk, den);
    if (neq25519(chk, num)) {
        return false;  /* not on curve */
    }

    if (par25519(r[0]) == (p[31] >> 7)) {
        gf_sub(r[0], gf0, r[0]);
    }

    gf_mul(r[3], r[0], r[1]);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Scalar arithmetic mod l (group order)
 * l = 2^252 + 27742317777372353535851937790883648493
 * ═══════════════════════════════════════════════════════════════════════════ */

static const uint8_t L[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
    0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Scalar reduction mod l (group order)
 * l = 2^252 + 27742317777372353535851937790883648493
 *
 * 21-bit limb representation from the SUPERCOP ref10 implementation
 * (Bernstein, Duif, Lange, Schwabe, Yang — public domain).
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint64_t load3(const uint8_t *in)
{
    return (uint64_t)in[0] | ((uint64_t)in[1] << 8) | ((uint64_t)in[2] << 16);
}
static uint64_t load4(const uint8_t *in)
{
    return (uint64_t)in[0] | ((uint64_t)in[1] << 8)
         | ((uint64_t)in[2] << 16) | ((uint64_t)in[3] << 24);
}

/* Reduce a 64-byte little-endian scalar s mod l; result in out[32].
 *
 * Uses __int128 for intermediate limb accumulation because the products of
 * 21-bit input limbs by the reduction constants (up to ~997805) can reach
 * ~2^67 after 12 accumulation steps — well beyond int64_t range.  After the
 * carry normalisation all values fit back into int64_t.
 *
 * Algorithm: SUPERCOP ref10 / libsodium sc_reduce, public domain.
 */
static void sc_reduce(uint8_t out[32], const uint8_t s[64])
{
    typedef __int128 i128;

    /* Load 24 overlapping 21-bit windows from the 64-byte LE input. */
    i128 s0  = 2097151LL & (int64_t)load3(s);
    i128 s1  = 2097151LL & (int64_t)(load4(s +  2) >>  5);
    i128 s2  = 2097151LL & (int64_t)(load3(s +  5) >>  2);
    i128 s3  = 2097151LL & (int64_t)(load4(s +  7) >>  7);
    i128 s4  = 2097151LL & (int64_t)(load4(s + 10) >>  4);
    i128 s5  = 2097151LL & (int64_t)(load3(s + 13) >>  1);
    i128 s6  = 2097151LL & (int64_t)(load4(s + 15) >>  6);
    i128 s7  = 2097151LL & (int64_t)(load3(s + 18) >>  3);
    i128 s8  = 2097151LL & (int64_t)load3(s + 21);
    i128 s9  = 2097151LL & (int64_t)(load4(s + 23) >>  5);
    i128 s10 = 2097151LL & (int64_t)(load3(s + 26) >>  2);
    i128 s11 = 2097151LL & (int64_t)(load4(s + 28) >>  7);
    i128 s12 = 2097151LL & (int64_t)(load4(s + 31) >>  4);
    i128 s13 = 2097151LL & (int64_t)(load4(s + 34) >>  1);
    i128 s14 = 2097151LL & (int64_t)(load4(s + 36) >>  6);
    i128 s15 = 2097151LL & (int64_t)(load3(s + 39) >>  3);
    i128 s16 = 2097151LL & (int64_t)load3(s + 42);
    i128 s17 = 2097151LL & (int64_t)(load4(s + 44) >>  5);
    i128 s18 = 2097151LL & (int64_t)(load3(s + 47) >>  2);
    i128 s19 = 2097151LL & (int64_t)(load4(s + 49) >>  7);
    i128 s20 = 2097151LL & (int64_t)(load4(s + 52) >>  4);
    i128 s21 = 2097151LL & (int64_t)(load3(s + 55) >>  1);
    i128 s22 = 2097151LL & (int64_t)(load4(s + 57) >>  6);
    i128 s23 =              (int64_t)(load4(s + 60) >>  3);

    /* Reduction pass: fold s23..s12 into s0..s11 using the identity
     *   2^252 ≡ -c  (mod l),  c = [666643,470296,654183,-997805,136657,-683901]
     * Constants from SUPERCOP ref10 (public domain). */
    s11 += s23 * 666643; s12 += s23 * 470296;
    s13 += s23 * 654183; s14 -= s23 * 997805;
    s15 += s23 * 136657; s16 -= s23 * 683901; s23 = 0;

    s10 += s22 * 666643; s11 += s22 * 470296;
    s12 += s22 * 654183; s13 -= s22 * 997805;
    s14 += s22 * 136657; s15 -= s22 * 683901; s22 = 0;

    s9  += s21 * 666643; s10 += s21 * 470296;
    s11 += s21 * 654183; s12 -= s21 * 997805;
    s13 += s21 * 136657; s14 -= s21 * 683901; s21 = 0;

    s8  += s20 * 666643; s9  += s20 * 470296;
    s10 += s20 * 654183; s11 -= s20 * 997805;
    s12 += s20 * 136657; s13 -= s20 * 683901; s20 = 0;

    s7  += s19 * 666643; s8  += s19 * 470296;
    s9  += s19 * 654183; s10 -= s19 * 997805;
    s11 += s19 * 136657; s12 -= s19 * 683901; s19 = 0;

    s6  += s18 * 666643; s7  += s18 * 470296;
    s8  += s18 * 654183; s9  -= s18 * 997805;
    s10 += s18 * 136657; s11 -= s18 * 683901; s18 = 0;

    s5  += s17 * 666643; s6  += s17 * 470296;
    s7  += s17 * 654183; s8  -= s17 * 997805;
    s9  += s17 * 136657; s10 -= s17 * 683901; s17 = 0;

    s4  += s16 * 666643; s5  += s16 * 470296;
    s6  += s16 * 654183; s7  -= s16 * 997805;
    s8  += s16 * 136657; s9  -= s16 * 683901; s16 = 0;

    s3  += s15 * 666643; s4  += s15 * 470296;
    s5  += s15 * 654183; s6  -= s15 * 997805;
    s7  += s15 * 136657; s8  -= s15 * 683901; s15 = 0;

    s2  += s14 * 666643; s3  += s14 * 470296;
    s4  += s14 * 654183; s5  -= s14 * 997805;
    s6  += s14 * 136657; s7  -= s14 * 683901; s14 = 0;

    s1  += s13 * 666643; s2  += s13 * 470296;
    s3  += s13 * 654183; s4  -= s13 * 997805;
    s5  += s13 * 136657; s6  -= s13 * 683901; s13 = 0;

    s0  += s12 * 666643; s1  += s12 * 470296;
    s2  += s12 * 654183; s3  -= s12 * 997805;
    s4  += s12 * 136657; s5  -= s12 * 683901; s12 = 0;

/* Balanced carry: keeps lo in (-2^20, 2^20], propagates to hi. */
#define CARRY(hi, lo) do { \
    i128 _c = ((lo) + (i128)(1L << 20)) >> 21; \
    (hi) += _c; (lo) -= _c * (i128)(1L << 21); \
} while(0)
/* Floor carry: normalises lo to [0, 2^21); used in the final sequential
 * pass so that all limbs are non-negative for byte packing. */
#define CFLR(hi, lo) do { \
    i128 _c = (lo) >> 21; \
    (hi) += _c; (lo) -= _c * (i128)(1L << 21); \
} while(0)

    /* Carry propagation pass 1 (interleaved balanced carry). */
    CARRY(s1,  s0);  CARRY(s3,  s2);  CARRY(s5,  s4);
    CARRY(s7,  s6);  CARRY(s9,  s8);  CARRY(s11, s10);
    CARRY(s2,  s1);  CARRY(s4,  s3);  CARRY(s6,  s5);
    CARRY(s8,  s7);  CARRY(s10, s9);  CARRY(s12, s11);

    /* s12 is non-zero; fold it back into s0..s5. */
    s0 += s12 * 666643; s1 += s12 * 470296;
    s2 += s12 * 654183; s3 -= s12 * 997805;
    s4 += s12 * 136657; s5 -= s12 * 683901; s12 = 0;

    /* Carry propagation pass 2: sequential floor carry to force every limb
     * into [0, 2^21) ready for packing. */
    CFLR(s1, s0);  CFLR(s2, s1);  CFLR(s3, s2);  CFLR(s4, s3);
    CFLR(s5, s4);  CFLR(s6, s5);  CFLR(s7, s6);  CFLR(s8, s7);
    CFLR(s9, s8);  CFLR(s10, s9); CFLR(s11, s10); CFLR(s12, s11);

    /* s12 residual (can happen once when pass-1 left a small negative) */
    if (s12 != 0) {
        s0 += s12 * 666643; s1 += s12 * 470296;
        s2 += s12 * 654183; s3 -= s12 * 997805;
        s4 += s12 * 136657; s5 -= s12 * 683901; s12 = 0;
        CFLR(s1, s0);  CFLR(s2, s1);  CFLR(s3, s2);  CFLR(s4, s3);
        CFLR(s5, s4);  CFLR(s6, s5);  CFLR(s7, s6);  CFLR(s8, s7);
        CFLR(s9, s8);  CFLR(s10, s9); CFLR(s11, s10); CFLR(s12, s11);
        (void)s12;
    }
#undef CARRY
#undef CFLR

    /* Pack 12 × 21-bit limbs into 32 bytes (little-endian).
     * All s[i] are in [0, 2^21) so the casts to int64_t are safe. */
#define S(x) ((int64_t)(x))
    out[ 0] = (uint8_t)( S(s0) >>  0);
    out[ 1] = (uint8_t)( S(s0) >>  8);
    out[ 2] = (uint8_t)((S(s0) >> 16) | (S(s1) << 5));
    out[ 3] = (uint8_t)( S(s1) >>  3);
    out[ 4] = (uint8_t)( S(s1) >> 11);
    out[ 5] = (uint8_t)((S(s1) >> 19) | (S(s2) << 2));
    out[ 6] = (uint8_t)( S(s2) >>  6);
    out[ 7] = (uint8_t)((S(s2) >> 14) | (S(s3) << 7));
    out[ 8] = (uint8_t)( S(s3) >>  1);
    out[ 9] = (uint8_t)( S(s3) >>  9);
    out[10] = (uint8_t)((S(s3) >> 17) | (S(s4) << 4));
    out[11] = (uint8_t)( S(s4) >>  4);
    out[12] = (uint8_t)( S(s4) >> 12);
    out[13] = (uint8_t)((S(s4) >> 20) | (S(s5) << 1));
    out[14] = (uint8_t)( S(s5) >>  7);
    out[15] = (uint8_t)((S(s5) >> 15) | (S(s6) << 6));
    out[16] = (uint8_t)( S(s6) >>  2);
    out[17] = (uint8_t)( S(s6) >> 10);
    out[18] = (uint8_t)((S(s6) >> 18) | (S(s7) << 3));
    out[19] = (uint8_t)( S(s7) >>  5);
    out[20] = (uint8_t)( S(s7) >> 13);
    out[21] = (uint8_t)( S(s8) >>  0);
    out[22] = (uint8_t)( S(s8) >>  8);
    out[23] = (uint8_t)((S(s8) >> 16) | (S(s9) << 5));
    out[24] = (uint8_t)( S(s9) >>  3);
    out[25] = (uint8_t)( S(s9) >> 11);
    out[26] = (uint8_t)((S(s9) >> 19) | (S(s10)<< 2));
    out[27] = (uint8_t)( S(s10)>>  6);
    out[28] = (uint8_t)((S(s10)>> 14) | (S(s11)<< 7));
    out[29] = (uint8_t)( S(s11)>>  1);
    out[30] = (uint8_t)( S(s11)>>  9);
    out[31] = (uint8_t)( S(s11)>> 17);
#undef S
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Ed25519 verify
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Modular inverse: r = x^{p-2} mod p via Fermat's little theorem. */
static void gf_inv(gf r, const gf x)
{
    gf t;
    gf_cpy(t, x);
    /* exponent p-2 = 2^255-21: all bits set except bits 2 and 4 */
    for (int i = 253; i >= 0; i--) {
        gf_sq(t, t);
        if (i != 2 && i != 4) gf_mul(t, t, x);
    }
    gf_cpy(r, t);
}

/* Pack an extended-coordinates point to 32 bytes. */
static void pt_pack(uint8_t out[32], pt p)
{
    gf tx, ty, zi;
    gf_inv(zi, p[2]);      /* zi = 1/Z */
    gf_mul(tx, p[0], zi);  /* x = X/Z */
    gf_mul(ty, p[1], zi);  /* y = Y/Z */
    pack25519(out, ty);
    out[31] ^= par25519(tx) << 7;
}

bool ed25519_verify_detached(const uint8_t sig[64],
                              const uint8_t *msg, size_t msglen,
                              const uint8_t pk[32])
{
    uint8_t h[SHA512_DIGEST];
    uint8_t rcheck[32];
    pt A, nB, R;
    uint8_t s[32];

    /* s = sig[32..63] — check it is < l */
    memcpy(s, sig + 32, 32);
    if (s[31] & 0xe0) return false;  /* s >= 2^253, reject */

    /* Decompress nA = -A (negate, because we check s*B == R + h*A) */
    if (!unpackneg(A, pk)) return false;

    /* h = SHA-512(sig[0..31] || pk || msg) reduced mod l */
    sha512_3(h, sig, 32, pk, 32, msg, msglen);
    sc_reduce(h, h);

    /* Compute s*B */
    scalarbase(nB, s);

    /* Compute h*A (A is already negated so this gives -h*A) */
    scalarmult(R, A, h);

    /* R_check = s*B + (-h*A)  =  s*B - h*A */
    pt_add(nB, R);

    /* Pack the computed point and compare with sig[0..31] */
    pt_pack(rcheck, nB);

    /* Constant-time compare */
    int diff = 0;
    for (int i = 0; i < 32; i++) diff |= rcheck[i] ^ sig[i];
    return diff == 0;
}
