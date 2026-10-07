/* Audio-only RTP framing. Video depacketizers are deliberately not included. */
#include "rtp.h"
#include "utils.h"
#include "wire.h"
#include <string.h>

int rtp_packet_validate(uint8_t *p, size_t n) {
  return p && n >= 12 && (p[0] >> 6) == 2 &&
         ((p[1] & 127) < 64 || (p[1] & 127) >= 96);
}
uint32_t rtp_get_ssrc(uint8_t *p) { return wire_u32(p + 8); }
int rtp_payload(const uint8_t *p, size_t n, size_t *offset, size_t *length) {
  if (!rtp_packet_validate((uint8_t *)p, n))
    return -1;
  size_t start = 12 + 4 * (size_t)(p[0] & 15);
  if (start > n)
    return -1;
  if (p[0] & 16) {
    if (n - start < 4)
      return -1;
    size_t ext = 4 + 4 * (size_t)wire_u16(p + start + 2);
    if (ext > n - start)
      return -1;
    start += ext;
  }
  size_t end = n;
  if (p[0] & 32) {
    size_t pad = p[n - 1];
    if (!pad || pad > n - start)
      return -1;
    end -= pad;
  }
  *offset = start;
  *length = end - start;
  return 0;
}
static int encode(RtpEncoder *e, uint8_t *data, size_t size) {
  if (!e || !e->on_packet || !e->ssrc || !data || !size ||
      size > CONFIG_MTU - 12 - 32)
    return -1;
  uint8_t *p = e->buf;
  p[0] = 0x80;
  p[1] = (uint8_t)e->type;
  wire_put16(p + 2, e->seq_number++);
  wire_put32(p + 4, e->timestamp);
  wire_put32(p + 8, e->ssrc);
  e->timestamp += e->timestamp_increment;
  memcpy(p + 12, data, size);
  e->on_packet(p, size + 12, e->user_data);
  return 0;
}
void rtp_encoder_init(RtpEncoder *e, MediaCodec codec, RtpOnPacket cb,
                      void *ctx) {
  memset(e, 0, sizeof(*e));
  if (codec != CODEC_OPUS)
    return;
  e->on_packet = cb;
  e->user_data = ctx;
  e->type = PT_OPUS;
  e->encode_func = encode;
  uint8_t random[10];
  if (utils_random_bytes(random, sizeof(random)))
    return;
  e->ssrc = wire_u32(random);
  if (!e->ssrc)
    e->ssrc = 1;
  e->seq_number = wire_u16(random + 4);
  e->timestamp = wire_u32(random + 6);
  e->timestamp_increment = 960;
}
int rtp_encoder_encode(RtpEncoder *e, const uint8_t *data, size_t size) {
  return e && e->encode_func ? e->encode_func(e, (uint8_t *)data, size) : -1;
}
static int decode(RtpDecoder *d, uint8_t *p, size_t n) {
  size_t offset, length;
  if ((p[1] & 127) != d->type || rtp_payload(p, n, &offset, &length))
    return -1;
  if (length && d->on_packet)
    d->on_packet(p + offset, length, d->user_data);
  return 0;
}
void rtp_decoder_init(RtpDecoder *d, MediaCodec codec, RtpOnPacket cb,
                      void *ctx) {
  memset(d, 0, sizeof(*d));
  if (codec != CODEC_OPUS)
    return;
  d->type = PT_OPUS;
  d->on_packet = cb;
  d->user_data = ctx;
  d->decode_func = decode;
}
int rtp_decoder_decode(RtpDecoder *d, const uint8_t *data, size_t size) {
  return d && d->decode_func && data && size >= 12
             ? d->decode_func(d, (uint8_t *)data, size)
             : -1;
}
