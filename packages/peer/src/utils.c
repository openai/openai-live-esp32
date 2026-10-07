#include "utils.h"
#include <mbedtls/entropy.h>
#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>
#include <string.h>

int utils_random_bytes(void *out, size_t size) {
  mbedtls_entropy_context entropy;
  mbedtls_entropy_init(&entropy);
  size_t offset = 0;
  int ret = 0;
  while (offset < size) {
    size_t n = size - offset;
    if (n > MBEDTLS_ENTROPY_BLOCK_SIZE)
      n = MBEDTLS_ENTROPY_BLOCK_SIZE;
    ret = mbedtls_entropy_func(&entropy, (uint8_t *)out + offset, n);
    if (ret)
      break;
    offset += n;
  }
  mbedtls_entropy_free(&entropy);
  if (ret)
    mbedtls_platform_zeroize(out, size);
  return ret;
}

int utils_random_string(char *s, int len) {
  static const char alphabet[] =
      "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz+/";
  if (!s || len < 0 || len > 256)
    return -1;
  uint8_t random[256];
  if (utils_random_bytes(random, (size_t)len)) {
    s[0] = 0;
    return -1;
  }
  for (int i = 0; i < len; ++i)
    s[i] = alphabet[random[i] & 63];
  s[len] = 0;
  mbedtls_platform_zeroize(random, sizeof(random));
  return 0;
}

int utils_get_hmac_sha1(const char *input, size_t input_len, const char *key,
                        size_t key_len, unsigned char *output) {
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
  if (!md || !key || !key_len)
    return -1;
  return mbedtls_md_hmac(md, (const uint8_t *)key, key_len,
                         (const uint8_t *)input, input_len, output);
}
