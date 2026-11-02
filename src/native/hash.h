/* Message digests used by advapi32 (CryptCreateHash), bcrypt and crypt32.
 * Implemented in hash.c from the published algorithm specifications
 * (RFC 1321, FIPS 180-4).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef LSIW_HASH_H
#define LSIW_HASH_H

#include <stddef.h>
#include <stdint.h>

enum {
    LSIW_HASH_MD5 = 1,
    LSIW_HASH_SHA1,
    LSIW_HASH_SHA256,
    LSIW_HASH_SHA384,
    LSIW_HASH_SHA512
};

#define LSIW_HASH_MAX_DIGEST 64

struct lsiw_hash {
    int      alg;
    uint64_t total;                  /* bytes hashed so far             */
    union { uint32_t s32[8]; uint64_t s64[8]; } st;
    uint8_t  buf[128];               /* pending partial block           */
    size_t   blen;
};

/* Returns 0 on success, -1 for an unknown algorithm. */
int    lsiw_hash_init(struct lsiw_hash *h, int alg);
void   lsiw_hash_update(struct lsiw_hash *h, const void *data, size_t n);
/* Writes the digest (lsiw_hash_size(alg) bytes) and returns its size. The
 * context must be re-initialised before reuse. */
size_t lsiw_hash_final(struct lsiw_hash *h, uint8_t *out);
size_t lsiw_hash_size(int alg);          /* 0 for an unknown algorithm */
size_t lsiw_hash_block_size(int alg);    /* 64 or 128, for HMAC         */
/* One-shot helpers. */
size_t lsiw_hash_buf(int alg, const void *data, size_t n, uint8_t *out);
/* HMAC (RFC 2104) over any of the algorithms above. */
size_t lsiw_hmac(int alg, const void *key, size_t klen, const void *data, size_t n, uint8_t *out);

#endif
