/* Message digests: MD5 (RFC 1321), SHA-1 / SHA-256 / SHA-384 / SHA-512
 * (FIPS 180-4) and HMAC (RFC 2104). Written from the published algorithm
 * specifications; used by advapi32 (CryptCreateHash), bcrypt and crypt32.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "hash.h"
#include <string.h>

/* ---- small endian helpers --------------------------------------------- */

static uint32_t rd32be(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}
static uint64_t rd64be(const uint8_t *p)
{
    return (uint64_t)rd32be(p) << 32 | rd32be(p + 4);
}
static void wr32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void wr32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void wr64be(uint8_t *p, uint64_t v)
{
    wr32be(p, (uint32_t)(v >> 32)); wr32be(p + 4, (uint32_t)v);
}

static uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static uint64_t rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

/* ---- MD5 (RFC 1321) ---------------------------------------------------- */

static void md5_block(uint32_t s[4], const uint8_t *block)
{
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
        0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
        0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
        0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
        0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
        0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
        0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
        0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
    static const int R[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
    uint32_t m[16];
    for (int i = 0; i < 16; i++) m[i] = rd32le(block + i * 4);
    uint32_t a = s[0], b = s[1], c = s[2], d = s[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16)      { f = (b & c) | (~b & d);           g = i; }
        else if (i < 32) { f = (d & b) | (~d & c);           g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d;                    g = (3 * i + 5) & 15; }
        else             { f = c ^ (b | ~d);                 g = (7 * i) & 15; }
        f += a + K[i] + m[g];
        a = d; d = c; c = b;
        b += rotl32(f, R[i]);
    }
    s[0] += a; s[1] += b; s[2] += c; s[3] += d;
}

/* ---- SHA-1 (FIPS 180-4) ------------------------------------------------ */

static void sha1_block(uint32_t s[5], const uint8_t *block)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = rd32be(block + i * 4);
    for (int i = 16; i < 80; i++) w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = s[0], b = s[1], c = s[2], d = s[3], e = s[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5a827999; }
        else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8f1bbcdc; }
        else             { f = b ^ c ^ d;                    k = 0xca62c1d6; }
        uint32_t t = rotl32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rotl32(b, 30); b = a; a = t;
    }
    s[0] += a; s[1] += b; s[2] += c; s[3] += d; s[4] += e;
}

/* ---- SHA-256 (FIPS 180-4) ---------------------------------------------- */

static void sha256_block(uint32_t s[8], const uint8_t *block)
{
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = rd32be(block + i * 4);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s[0], b = s[1], c = s[2], d = s[3], e = s[4], f = s[5], g = s[6], h = s[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s[0] += a; s[1] += b; s[2] += c; s[3] += d;
    s[4] += e; s[5] += f; s[6] += g; s[7] += h;
}

/* ---- SHA-512 / SHA-384 (FIPS 180-4) ------------------------------------ */

static void sha512_block(uint64_t s[8], const uint8_t *block)
{
    static const uint64_t K[80] = {
        0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
        0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
        0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
        0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
        0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
        0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
        0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
        0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
        0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
        0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
        0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
        0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
        0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
        0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
        0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
        0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
        0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
        0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
        0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
        0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL };
    uint64_t w[80];
    for (int i = 0; i < 16; i++) w[i] = rd64be(block + i * 8);
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint64_t a = s[0], b = s[1], c = s[2], d = s[3], e = s[4], f = s[5], g = s[6], h = s[7];
    for (int i = 0; i < 80; i++) {
        uint64_t S1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
        uint64_t ch = (e & f) ^ (~e & g);
        uint64_t t1 = h + S1 + ch + K[i] + w[i];
        uint64_t S0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
        uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint64_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s[0] += a; s[1] += b; s[2] += c; s[3] += d;
    s[4] += e; s[5] += f; s[6] += g; s[7] += h;
}

/* ---- public interface -------------------------------------------------- */

size_t lsiw_hash_size(int alg)
{
    switch (alg) {
    case LSIW_HASH_MD5:    return 16;
    case LSIW_HASH_SHA1:   return 20;
    case LSIW_HASH_SHA256: return 32;
    case LSIW_HASH_SHA384: return 48;
    case LSIW_HASH_SHA512: return 64;
    default:               return 0;
    }
}

size_t lsiw_hash_block_size(int alg)
{
    switch (alg) {
    case LSIW_HASH_MD5:
    case LSIW_HASH_SHA1:
    case LSIW_HASH_SHA256: return 64;
    case LSIW_HASH_SHA384:
    case LSIW_HASH_SHA512: return 128;
    default:               return 0;
    }
}

int lsiw_hash_init(struct lsiw_hash *h, int alg)
{
    if (!lsiw_hash_size(alg)) return -1;
    h->alg   = alg;
    h->total = 0;
    h->blen  = 0;
    switch (alg) {
    case LSIW_HASH_MD5:
        h->st.s32[0] = 0x67452301; h->st.s32[1] = 0xefcdab89;
        h->st.s32[2] = 0x98badcfe; h->st.s32[3] = 0x10325476;
        break;
    case LSIW_HASH_SHA1:
        h->st.s32[0] = 0x67452301; h->st.s32[1] = 0xefcdab89;
        h->st.s32[2] = 0x98badcfe; h->st.s32[3] = 0x10325476;
        h->st.s32[4] = 0xc3d2e1f0;
        break;
    case LSIW_HASH_SHA256:
        h->st.s32[0] = 0x6a09e667; h->st.s32[1] = 0xbb67ae85;
        h->st.s32[2] = 0x3c6ef372; h->st.s32[3] = 0xa54ff53a;
        h->st.s32[4] = 0x510e527f; h->st.s32[5] = 0x9b05688c;
        h->st.s32[6] = 0x1f83d9ab; h->st.s32[7] = 0x5be0cd19;
        break;
    case LSIW_HASH_SHA384:
        h->st.s64[0] = 0xcbbb9d5dc1059ed8ULL; h->st.s64[1] = 0x629a292a367cd507ULL;
        h->st.s64[2] = 0x9159015a3070dd17ULL; h->st.s64[3] = 0x152fecd8f70e5939ULL;
        h->st.s64[4] = 0x67332667ffc00b31ULL; h->st.s64[5] = 0x8eb44a8768581511ULL;
        h->st.s64[6] = 0xdb0c2e0d64f98fa7ULL; h->st.s64[7] = 0x47b5481dbefa4fa4ULL;
        break;
    case LSIW_HASH_SHA512:
        h->st.s64[0] = 0x6a09e667f3bcc908ULL; h->st.s64[1] = 0xbb67ae8584caa73bULL;
        h->st.s64[2] = 0x3c6ef372fe94f82bULL; h->st.s64[3] = 0xa54ff53a5f1d36f1ULL;
        h->st.s64[4] = 0x510e527fade682d1ULL; h->st.s64[5] = 0x9b05688c2b3e6c1fULL;
        h->st.s64[6] = 0x1f83d9abfb41bd6bULL; h->st.s64[7] = 0x5be0cd19137e2179ULL;
        break;
    }
    return 0;
}

static void hash_block(struct lsiw_hash *h, const uint8_t *block)
{
    switch (h->alg) {
    case LSIW_HASH_MD5:    md5_block(h->st.s32, block);    break;
    case LSIW_HASH_SHA1:   sha1_block(h->st.s32, block);   break;
    case LSIW_HASH_SHA256: sha256_block(h->st.s32, block); break;
    case LSIW_HASH_SHA384:
    case LSIW_HASH_SHA512: sha512_block(h->st.s64, block); break;
    }
}

void lsiw_hash_update(struct lsiw_hash *h, const void *data, size_t n)
{
    const uint8_t *p = data;
    size_t bs = lsiw_hash_block_size(h->alg);
    h->total += n;
    /* Finish any partial block first. */
    if (h->blen) {
        size_t need = bs - h->blen;
        size_t take = n < need ? n : need;
        memcpy(h->buf + h->blen, p, take);
        h->blen += take;
        p += take; n -= take;
        if (h->blen == bs) { hash_block(h, h->buf); h->blen = 0; }
    }
    while (n >= bs) { hash_block(h, p); p += bs; n -= bs; }
    if (n) { memcpy(h->buf, p, n); h->blen = n; }
}

size_t lsiw_hash_final(struct lsiw_hash *h, uint8_t *out)
{
    size_t bs    = lsiw_hash_block_size(h->alg);
    int    be    = (h->alg != LSIW_HASH_MD5);   /* MD5 is little-endian */
    uint64_t bits = h->total << 3;
    uint64_t hi   = h->total >> 61;             /* high bits of the bit count */

    /* Append 0x80 then zero padding up to the length field. */
    uint8_t one = 0x80;
    lsiw_hash_update(h, &one, 1);
    h->total--;                                  /* padding is not message data */
    while (h->blen != bs - (bs == 128 ? 16 : 8)) {
        uint8_t z = 0;
        lsiw_hash_update(h, &z, 1);
        h->total--;
    }
    /* Length field: 64-bit for the 64-byte block algorithms, 128-bit for the
     * 128-byte ones. */
    uint8_t lenbuf[16];
    if (bs == 128) {
        wr64be(lenbuf, hi);
        wr64be(lenbuf + 8, bits);
        memcpy(h->buf + h->blen, lenbuf, 16);
    } else if (be) {
        wr64be(lenbuf, bits);
        memcpy(h->buf + h->blen, lenbuf, 8);
    } else {
        /* MD5: 64-bit little-endian bit length. */
        wr32le(lenbuf, (uint32_t)bits);
        wr32le(lenbuf + 4, (uint32_t)(bits >> 32));
        memcpy(h->buf + h->blen, lenbuf, 8);
    }
    hash_block(h, h->buf);

    size_t dsz = lsiw_hash_size(h->alg);
    switch (h->alg) {
    case LSIW_HASH_MD5:
        for (int i = 0; i < 4; i++) wr32le(out + i * 4, h->st.s32[i]);
        break;
    case LSIW_HASH_SHA1:
        for (int i = 0; i < 5; i++) wr32be(out + i * 4, h->st.s32[i]);
        break;
    case LSIW_HASH_SHA256:
        for (int i = 0; i < 8; i++) wr32be(out + i * 4, h->st.s32[i]);
        break;
    case LSIW_HASH_SHA384:
    case LSIW_HASH_SHA512:
        for (size_t i = 0; i < dsz / 8; i++) wr64be(out + i * 8, h->st.s64[i]);
        break;
    }
    return dsz;
}

size_t lsiw_hash_buf(int alg, const void *data, size_t n, uint8_t *out)
{
    struct lsiw_hash h;
    if (lsiw_hash_init(&h, alg) != 0) return 0;
    lsiw_hash_update(&h, data, n);
    return lsiw_hash_final(&h, out);
}

size_t lsiw_hmac(int alg, const void *key, size_t klen,
                 const void *data, size_t n, uint8_t *out)
{
    size_t bs  = lsiw_hash_block_size(alg);
    size_t dsz = lsiw_hash_size(alg);
    if (!bs) return 0;

    uint8_t k[128];
    memset(k, 0, bs);
    if (klen > bs) {
        /* A key longer than the block size is replaced by its own hash. */
        lsiw_hash_buf(alg, key, klen, k);
    } else {
        memcpy(k, key, klen);
    }

    uint8_t ipad[128], opad[128];
    for (size_t i = 0; i < bs; i++) {
        ipad[i] = (uint8_t)(k[i] ^ 0x36);
        opad[i] = (uint8_t)(k[i] ^ 0x5c);
    }

    /* inner = H((K ^ ipad) || message) */
    uint8_t inner[LSIW_HASH_MAX_DIGEST];
    struct lsiw_hash h;
    lsiw_hash_init(&h, alg);
    lsiw_hash_update(&h, ipad, bs);
    lsiw_hash_update(&h, data, n);
    lsiw_hash_final(&h, inner);

    /* out = H((K ^ opad) || inner) */
    lsiw_hash_init(&h, alg);
    lsiw_hash_update(&h, opad, bs);
    lsiw_hash_update(&h, inner, dsz);
    return lsiw_hash_final(&h, out);
}
