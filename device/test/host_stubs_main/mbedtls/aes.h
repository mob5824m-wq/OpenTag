#pragma once
#include <cstddef>
#include <cstdint>
typedef struct { int dummy; } mbedtls_aes_context;
inline void mbedtls_aes_init(mbedtls_aes_context *) {}
inline void mbedtls_aes_free(mbedtls_aes_context *) {}
inline int mbedtls_aes_setkey_enc(mbedtls_aes_context *, const uint8_t *, unsigned) { return 0; }
inline int mbedtls_aes_crypt_ctr(mbedtls_aes_context *, size_t, size_t *, unsigned char *,
                                 const unsigned char *, const uint8_t *, uint8_t *) { return 0; }
