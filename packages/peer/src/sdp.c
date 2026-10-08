#include "sdp.h"
#include "utils.h"
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int decimal(const char *s, uint32_t max, uint32_t *value) {
  if (!s || !*s)
    return -1;
  uint32_t n = 0;
  for (; *s; ++s) {
    if (*s < '0' || *s > '9' || n > (max - (uint32_t)(*s - '0')) / 10)
      return -1;
    n = 10 * n + (uint32_t)(*s - '0');
  }
  *value = n;
  return 0;
}
static int credential(char *dst, const char *value, size_t minimum) {
  size_t n = strlen(value);
  if (n < minimum || n > 256 || (dst[0] && strcmp(dst, value)))
    return -1;
  for (size_t i = 0; i < n; ++i)
    if (!isalnum((unsigned char)value[i]) && value[i] != '+' && value[i] != '/')
      return -1;
  memcpy(dst, value, n + 1);
  return 0;
}
static int fingerprint(char *out, const char *value) {
  if (strlen(value) != 95)
    return -1;
  char canonical[96];
  for (size_t i = 0; i < 95; ++i) {
    if (i % 3 == 2) {
      if (value[i] != ':')
        return -1;
    } else if (!isxdigit((unsigned char)value[i]))
      return -1;
    canonical[i] = (char)toupper((unsigned char)value[i]);
  }
  canonical[95] = 0;
  if (out[0] && strcmp(out, canonical))
    return -1;
  memcpy(out, canonical, sizeof(canonical));
  return 0;
}
int peer_sdp_parse(const char *data, size_t size, PeerSdp *out) {
  if (!data || !out || size < 5 || size >= PEER_SDP_LIMIT ||
      memchr(data, 0, size) || memcmp(data, "v=0\r\n", 5))
    return -1;
  PeerSdp s = {.max_message_size = PEER_MESSAGE_LIMIT};
  int section = 0, bundle_seen = 0, limits_seen = 0;
  unsigned transport[3] = {0};
  char bundle[70] = {0};
  for (size_t pos = 0; pos < size;) {
    const char *lf = memchr(data + pos, '\n', size - pos);
    if (!lf || lf == data + pos || lf[-1] != '\r')
      return -1;
    size_t n = (size_t)(lf - data - pos) - 1;
    if (n > 1023 || n < 2)
      return -1;
    char line[1024];
    memcpy(line, data + pos, n);
    line[n] = 0;
    pos += n + 2;
    for (size_t i = 0; i < n; ++i)
      if ((unsigned char)line[i] < 32 || (unsigned char)line[i] > 126)
        return -1;
    if (line[1] != '=')
      return -1;
    if (!strncmp(line, "m=", 2)) {
      char kind[16], port[16], protocol[32], format[32], extra;
      if (sscanf(line + 2, "%15s %15s %31s %31s %c", kind, port, protocol,
                 format, &extra) != 4)
        return -1;
      uint32_t number;
      if (decimal(port, 65535, &number) || !number)
        return -1;
      if (!strcmp(kind, "audio") && !s.audio && !s.data &&
          !strcmp(format, "111") && !strcmp(protocol, "UDP/TLS/RTP/SAVPF")) {
        s.audio = 1;
        section = 1;
      } else if (!strcmp(kind, "application") && !s.data &&
                 !strcmp(protocol, "UDP/DTLS/SCTP") &&
                 !strcmp(format, "webrtc-datachannel")) {
        s.data = 1;
        section = 2;
      } else
        return -1;
    } else if (!strncmp(line, "a=group:BUNDLE ", 15)) {
      if (bundle_seen++ || section || strlen(line + 15) >= sizeof(bundle))
        return -1;
      strcpy(bundle, line + 15);
    } else if (!strncmp(line, "a=ice-ufrag:", 12)) {
      if (credential(s.ufrag, line + 12, 4))
        return -1;
      transport[section] |= 1;
    } else if (!strncmp(line, "a=ice-pwd:", 10)) {
      if (credential(s.password, line + 10, 22))
        return -1;
      transport[section] |= 2;
    } else if (!strncmp(line, "a=fingerprint:sha-256 ", 22)) {
      if (fingerprint(s.fingerprint, line + 22))
        return -1;
      transport[section] |= 4;
    } else if (!strcmp(line, "a=ice-mismatch")) {
      return -1; /* No non-ICE fallback in this transport. */
    } else if (!strncmp(line, "a=tls-id:", 9)) {
      size_t length = strlen(line + 9);
      if (!section || length < 20 || length > 255 ||
          (s.tls_id[0] && strcmp(s.tls_id, line + 9)))
        return -1;
      for (size_t i = 9; i < n; ++i)
        if (!isalnum((unsigned char)line[i]) && !strchr("+/-_", line[i]))
          return -1;
      memcpy(s.tls_id, line + 9, length + 1);
    } else if (!strncmp(line, "a=setup:", 8)) {
      int role = !strcmp(line + 8, "active")    ? 1
                 : !strcmp(line + 8, "passive") ? 2
                 : !strcmp(line + 8, "actpass") ? 3
                                                : 0;
      if (!role || (s.setup && s.setup != role))
        return -1;
      s.setup = role;
      transport[section] |= 8;
    } else if (!strncmp(line, "a=mid:", 6)) {
      char *mid = section == 1 ? s.audio_mid : section == 2 ? s.data_mid : NULL;
      if (!mid || mid[0] || !line[6] || strlen(line + 6) > 32 ||
          strchr(line + 6, ' '))
        return -1;
      strcpy(mid, line + 6);
    } else if (!strcmp(line, "a=sendonly") || !strcmp(line, "a=recvonly") ||
               !strcmp(line, "a=inactive")) {
      return -1; /* Initial media API is explicitly bidirectional. */
    } else if (!strncmp(line, "a=rtpmap:", 9)) {
      if (section != 1 || strcmp(line + 9, "111 opus/48000/2") || s.opus)
        return -1;
      s.opus = 1;
    } else if (!strcmp(line, "a=rtcp-mux")) {
      if (section != 1)
        return -1;
      s.rtcp_mux = 1;
    } else if (!strncmp(line, "a=ssrc:", 7) && section == 1) {
      char *space = strchr(line + 7, ' ');
      if (!space)
        return -1;
      *space = 0;
      uint32_t value;
      if (decimal(line + 7, UINT32_MAX, &value) || !value ||
          (s.ssrc && s.ssrc != value))
        return -1;
      s.ssrc = value;
    } else if (!strncmp(line, "a=sctp-port:", 12)) {
      uint32_t port;
      if (section != 2 || s.sctp_port || decimal(line + 12, 65535, &port) ||
          !port)
        return -1;
      s.sctp_port = (uint16_t)port;
    } else if (!strncmp(line, "a=max-message-size:", 19)) {
      uint32_t limit;
      if (section != 2 || limits_seen++ ||
          decimal(line + 19, UINT32_MAX, &limit))
        return -1;
      s.max_message_size =
          limit && limit < PEER_MESSAGE_LIMIT ? limit : PEER_MESSAGE_LIMIT;
    } else if (!strncmp(line, "a=candidate:", 12)) {
      if (!section)
        return -1;
      IceCandidate c;
      int candidate_result = ice_candidate_from_description(&c, line, line + n);
      if (candidate_result < 0)
        return -1;
      if (candidate_result > 0)
        continue;
      size_t i;
      for (i = 0; i < s.candidate_count; ++i)
        if (addr_equal(&c.addr, &s.candidates[i].addr))
          break;
      if (i == s.candidate_count) {
        if (i == PEER_SDP_CANDIDATES)
          return -1;
        s.candidates[s.candidate_count++] = c;
      }
    }
  }
  if (!bundle_seen || !s.ufrag[0] || !s.password[0] || !s.fingerprint[0] ||
      !s.setup || !s.candidate_count || (!s.audio && !s.data) ||
      (s.audio && (transport[0] | transport[1]) != 15) ||
      (s.data && (transport[0] | transport[2]) != 15) ||
      (s.audio && (!s.rtcp_mux || !s.opus || !s.audio_mid[0])) ||
      (s.data && (!s.sctp_port || !s.data_mid[0])) ||
      (s.audio && s.data && !strcmp(s.audio_mid, s.data_mid)))
    return -1;
  int seen_audio = 0, seen_data = 0;
  char *save;
  for (char *mid = strtok_r(bundle, " ", &save); mid;
       mid = strtok_r(NULL, " ", &save)) {
    if (s.audio && !strcmp(mid, s.audio_mid) && !seen_audio && !seen_data)
      seen_audio = 1;
    else if (s.data && !strcmp(mid, s.data_mid) && !seen_data &&
             (!s.audio || seen_audio))
      seen_data = 1;
    else
      return -1;
  }
  if (seen_audio != s.audio || seen_data != s.data)
    return -1;
  *out = s;
  return 0;
}

static int append(char *out, size_t capacity, size_t *used, const char *format,
                  ...) {
  if (*used >= capacity)
    return -1;
  va_list args;
  va_start(args, format);
  int n = vsnprintf(out + *used, capacity - *used, format, args);
  va_end(args);
  if (n < 0 || (size_t)n >= capacity - *used) {
    out[0] = 0;
    return -1;
  }
  *used += (size_t)n;
  return 0;
}
int peer_sdp_offer(char *out, size_t capacity, const char *ufrag,
                   const char *password, const char *fingerprint_value,
                   uint32_t ssrc, int audio, int data, const char *candidates) {
  size_t used = 0;
  if (!out || !capacity || !ufrag || !password || !fingerprint_value ||
      !candidates)
    return -1;
  out[0] = 0;
  char tls_id[33];
  if (utils_random_string(tls_id, 32))
    return -1;
  if (append(out, capacity, &used,
             "v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\ns=-\r\nt=0 "
             "0\r\na=group:BUNDLE%s%s\r\na=msid-semantic: WMS peer\r\n",
             audio ? " audio" : "", data ? " data" : ""))
    return -1;
  for (int section = 0; section < 2; ++section) {
    if (section == 0 && !audio)
      continue;
    if (section == 1 && !data)
      continue;
    if (append(out, capacity, &used,
               "%s\r\nc=IN IP4 "
               "0.0.0.0\r\na=mid:%s\r\na=ice-ufrag:%s\r\na=ice-pwd:%s\r\na="
               "fingerprint:sha-256 %s\r\na=setup:actpass\r\n",
               section == 0
                   ? "m=audio 9 UDP/TLS/RTP/SAVPF 111"
                   : "m=application 9 UDP/DTLS/SCTP webrtc-datachannel",
               section == 0 ? "audio" : "data", ufrag, password,
               fingerprint_value))
      return -1;
    /* BUNDLE's IDENTICAL attributes need only appear on its first m-line. */
    if ((section == 0 || !audio) &&
        append(out, capacity, &used, "a=tls-id:%s\r\n", tls_id))
      return -1;
    if (!section) {
      if (append(out, capacity, &used,
                 "a=rtpmap:111 "
                 "opus/48000/2\r\na=sendrecv\r\na=rtcp-mux\r\na=ssrc:%" PRIu32
                 " cname:peer\r\n",
                 ssrc))
        return -1;
    } else if (append(out, capacity, &used,
                      "a=sctp-port:5000\r\na=max-message-size:65536\r\n"))
      return -1;
    if (append(out, capacity, &used, "%sa=end-of-candidates\r\n", candidates))
      return -1;
  }
  return 0;
}
