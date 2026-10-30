#pragma once
#include <cstddef>
#include <cstdint>
typedef struct { int dummy; } mbedtls_md_info_t;
typedef enum { MBEDTLS_MD_SHA256 } mbedtls_md_type_t;
inline const mbedtls_md_info_t *mbedtls_md_info_from_type(mbedtls_md_type_t) { static mbedtls_md_info_t i; return &i; }
inline int mbedtls_md_hmac(const mbedtls_md_info_t *, const uint8_t *, size_t, const uint8_t *, size_t, uint8_t *) { return 0; }
