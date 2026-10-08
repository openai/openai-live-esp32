/* CI's DTLS fuzzer reached comparison of empty X.509 Names. Exercise that
 * boundary deterministically through verification of an initialized empty
 * certificate chain, with no trust anchors. This must fail without UB. */
#include <mbedtls/x509_crt.h>
#include <stdint.h>
#include <stdio.h>
int main(void) {
  mbedtls_x509_crt crt;
  mbedtls_x509_crt_init(&crt);
  uint32_t flags = 0;
  int result =
      mbedtls_x509_crt_verify(&crt, NULL, NULL, NULL, &flags, NULL, NULL);
  mbedtls_x509_crt_free(&crt);
  if (!result || !(flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED))
    return 1;
  puts("Empty X.509 name regression passed");
  return 0;
}
