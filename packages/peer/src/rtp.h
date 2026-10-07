#ifndef RTP_H_
#define RTP_H_

#include <stdint.h>

#include "config.h"
#include "peer_connection.h"

typedef enum { PT_OPUS = 111 } RtpPayloadType;

typedef struct RtpEncoder RtpEncoder;
typedef struct RtpDecoder RtpDecoder;
typedef void (*RtpOnPacket)(uint8_t *packet, size_t bytes, void *user_data);

struct RtpDecoder {
  RtpPayloadType type;
  RtpOnPacket on_packet;
  int (*decode_func)(RtpDecoder *rtp_decoder, uint8_t *data, size_t size);
  void *user_data;
};

struct RtpEncoder {
  RtpPayloadType type;
  RtpOnPacket on_packet;
  int (*encode_func)(RtpEncoder *rtp_encoder, uint8_t *data, size_t size);
  void *user_data;
  uint16_t seq_number;
  uint32_t ssrc;
  uint32_t timestamp;
  uint32_t timestamp_increment;
  uint8_t buf[CONFIG_MTU + 128];
};

int rtp_packet_validate(uint8_t *packet, size_t size);
int rtp_payload(const uint8_t *, size_t, size_t *, size_t *);

void rtp_encoder_init(RtpEncoder *rtp_encoder, MediaCodec codec,
                      RtpOnPacket on_packet, void *user_data);

int rtp_encoder_encode(RtpEncoder *rtp_encoder, const uint8_t *data,
                       size_t size);

void rtp_decoder_init(RtpDecoder *rtp_decoder, MediaCodec codec,
                      RtpOnPacket on_packet, void *user_data);

int rtp_decoder_decode(RtpDecoder *rtp_decoder, const uint8_t *data,
                       size_t size);

uint32_t rtp_get_ssrc(uint8_t *packet);

#endif // RTP_H_
