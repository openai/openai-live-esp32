#include "agent.h"
#include "dtls_srtp.h"
#include "peer.h"
#include "ports.h"
#include "rtcp.h"
#include "rtp.h"
#include "sctp.h"
#include "sdp.h"
#include "stun.h"
#include "utils.h"
#include "wire.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>

static unsigned checks;
#define CHECK(x)                                                               \
  do {                                                                         \
    ++checks;                                                                  \
    if (!(x)) {                                                                \
      fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #x);           \
      abort();                                                                 \
    }                                                                          \
  } while (0)
static void test_stun(void) {
  StunMessage a, b;
  char password[] = "test-password-with-at-least-22-characters";
  stun_msg_create(&a, 1);
  stun_msg_create(&b, 1);
  CHECK(!wire_equal(a.buf + 8, b.buf + 8, 12));
  CHECK(!stun_msg_write_attr(&a, STUN_ATTR_TYPE_USERNAME, 9, "abcd:efgh"));
  CHECK(!stun_msg_finish(&a, STUN_CREDENTIAL_SHORT_TERM, password,
                         strlen(password)));
  CHECK(!stun_msg_is_valid(a.buf, a.size, password));
  CHECK(stun_msg_is_valid(a.buf, a.size, "wrong"));
  for (size_t i = 0; i < a.size; ++i) {
    uint8_t changed[STUN_ATTR_BUF_SIZE];
    memcpy(changed, a.buf, a.size);
    changed[i] ^= 1;
    CHECK(stun_msg_is_valid(changed, a.size, password));
  }
  for (size_t i = 0; i < a.size; ++i)
    CHECK(stun_msg_parse(&b, a.buf, i));
  uint8_t long_message[STUN_ATTR_BUF_SIZE + 1] = {0};
  CHECK(stun_msg_parse(&b, long_message, sizeof(long_message)));
  stun_msg_create(&b, 1);
  CHECK(stun_msg_is_valid(b.buf, b.size, password));
  uint8_t value[600] = {0};
  memset(value, 'a', sizeof(value));
  CHECK(!stun_msg_write_attr(&b, STUN_ATTR_TYPE_USERNAME, sizeof(value),
                             (char *)value));
  CHECK(stun_parse_msg_buf(&b));
  stun_msg_create(&b, 1);
  CHECK(stun_msg_write_attr(&b, STUN_ATTR_TYPE_USERNAME, UINT16_MAX,
                            (char *)value));
  CHECK(stun_msg_finish(&b, STUN_CREDENTIAL_SHORT_TERM, password,
                        strlen(password)));
  stun_msg_create(&b, 1);
  CHECK(!stun_msg_write_attr(&b, STUN_ATTR_TYPE_MESSAGE_INTEGRITY, 19,
                             (char *)value));
  CHECK(stun_parse_msg_buf(&b));
  CHECK(!stun_msg_parse(&b, a.buf, a.size));
  CHECK(!strcmp(b.username, "abcd:efgh"));
  uint8_t unaligned[STUN_ATTR_BUF_SIZE + 1];
  memcpy(unaligned + 1, a.buf, a.size);
  CHECK(!stun_msg_is_valid(unaligned + 1, a.size, password));
}
static void test_rtp_rtcp(void) {
  uint8_t p[128] = {0x80, 111};
  size_t offset, size;
  for (size_t i = 0; i < 12; ++i)
    CHECK(rtp_payload(p, i, &offset, &size));
  CHECK(!rtp_payload(p, 15, &offset, &size));
  CHECK(offset == 12 && size == 3);
  p[0] = 0x8f;
  CHECK(rtp_payload(p, 15, &offset, &size));
  p[0] = 0x90;
  wire_put16(p + 14, 65535);
  CHECK(rtp_payload(p, 16, &offset, &size));
  wire_put16(p + 14, 1);
  CHECK(!rtp_payload(p, 23, &offset, &size));
  CHECK(offset == 20 && size == 3);
  p[0] = 0xa0;
  p[14] = 0;
  CHECK(rtp_payload(p, 15, &offset, &size));
  p[14] = 4;
  CHECK(rtp_payload(p, 15, &offset, &size));
  p[14] = 3;
  CHECK(!rtp_payload(p, 15, &offset, &size));
  CHECK(size == 0);
  uint8_t rr[16] = {0x80, 201, 0, 1, 0, 0, 0, 1};
  CHECK(!rtcp_validate(rr, 8));
  rr[0] = 0x81;
  CHECK(rtcp_validate(rr, 8));
  rr[0] = 0x80;
  CHECK(rtcp_validate(rr, 7));
  CHECK(rtcp_validate(rr, 9));
  wire_put16(rr + 2, 65535);
  CHECK(rtcp_validate(rr, 8));
  wire_put16(rr + 2, 1);
  rr[0] = 0xa0;
  rr[7] = 0;
  CHECK(rtcp_validate(rr, 8));
}
static void mutate_sdp(const char *source, const char *needle,
                       const char *replacement) {
  char bad[PEER_SDP_LIMIT];
  const char *at = strstr(source, needle);
  CHECK(at != NULL);
  size_t prefix = (size_t)(at - source), suffix = strlen(at + strlen(needle)),
         n = strlen(replacement);
  CHECK(prefix + n + suffix < sizeof(bad));
  memcpy(bad, source, prefix);
  memcpy(bad + prefix, replacement, n);
  memcpy(bad + prefix + n, at + strlen(needle), suffix + 1);
  PeerSdp parsed;
  CHECK(peer_sdp_parse(bad, prefix + n + suffix, &parsed));
}
static void test_sdp(void) {
  char fp[96], offer[PEER_SDP_LIMIT];
  for (int i = 0; i < 32; ++i)
    snprintf(fp + 3 * i, 4, "AA%s", i == 31 ? "" : ":");
  CHECK(!peer_sdp_offer(
      offer, sizeof(offer), "abcdefghijklmnop",
      "abcdefghijklmnopqrstuvwxyz012345", fp, 1234, 1, 1,
      "a=candidate:1 1 UDP 2122260223 127.0.0.1 12345 typ host\r\n"));
  PeerSdp parsed;
  CHECK(!peer_sdp_parse(offer, strlen(offer), &parsed));
  CHECK(parsed.audio && parsed.data && parsed.candidate_count == 1 &&
        parsed.ssrc == 1234);
  mutate_sdp(offer, "a=ice-ufrag:abcdefghijklmnop", "a=ice-ufrag:x");
  mutate_sdp(offer, "a=ice-pwd:abcdefghijklmnopqrstuvwxyz012345",
             "a=ice-pwd:short");
  mutate_sdp(offer, "a=setup:actpass", "a=setup:garbage");
  mutate_sdp(offer, "sha-256 AA:", "sha-256 ZZ:");
  mutate_sdp(offer, "UDP 2122260223", "UDP 99999999999999999999999999999");
  mutate_sdp(offer, "127.0.0.1 12345", "127.0.0.1 65536");
  mutate_sdp(offer, "127.0.0.1 12345", "224.0.0.1 12345");
  mutate_sdp(offer, "candidate:1 1 UDP", "candidate:1 2 UDP");
  mutate_sdp(offer, "candidate:1 1 UDP", "candidate:1 1 TCP");
  mutate_sdp(offer, "a=rtcp-mux", "a=unsupported");
  mutate_sdp(offer, "a=sendrecv", "a=inactive");
  mutate_sdp(offer, "a=rtpmap:111 opus/48000/2", "a=rtpmap:111 H264/90000");
  mutate_sdp(offer, "a=group:BUNDLE audio data",
             "a=group:BUNDLE audio unknown");
  mutate_sdp(offer, "a=sctp-port:5000", "a=sctp-port:0");
  mutate_sdp(offer, "a=ice-ufrag:abcdefghijklmnop",
             "a=ignored:abcdefghijklmnop");
  mutate_sdp(offer, "a=group:BUNDLE audio data", "a=group:BUNDLE data audio");
  mutate_sdp(offer, "a=max-message-size:65536",
             "a=max-message-size:65536\r\na=max-message-size:1");
  char huge[1100];
  memset(huge, 'a', sizeof(huge) - 1);
  huge[sizeof(huge) - 1] = 0;
  mutate_sdp(offer, "abcdefghijklmnop", huge);
  char candidates[2048] = {0};
  size_t pos = 0;
  for (int i = 0; i < 11; ++i)
    pos += (size_t)snprintf(
        candidates + pos, sizeof(candidates) - pos,
        "a=candidate:%d 1 UDP 2122260223 127.0.0.1 %d typ host\r\n", i,
        10000 + i);
  CHECK(!peer_sdp_offer(offer, sizeof(offer), "abcdefghijklmnop",
                        "abcdefghijklmnopqrstuvwxyz012345", fp, 1234, 1, 1,
                        candidates));
  CHECK(peer_sdp_parse(offer, strlen(offer), &parsed));
  CHECK(peer_sdp_parse(offer, PEER_SDP_LIMIT, &parsed));
  char tiny[10];
  CHECK(peer_sdp_offer(tiny, sizeof(tiny), "abcd", "long-password", fp, 1, 1, 1,
                       ""));
  CHECK(!tiny[0]);
}
static void test_crypto(void) {
  uint8_t key[30] = {0};
  for (size_t i = 0; i < sizeof(key); ++i)
    key[i] = (uint8_t)i;
  DtlsSrtp d = {.state = DTLS_SRTP_STATE_CONNECTED};
  srtp_policy_t policy = {0};
  srtp_crypto_policy_set_rtp_default(&policy.rtp);
  srtp_crypto_policy_set_rtcp_default(&policy.rtcp);
  policy.key = key;
  policy.ssrc.type = ssrc_any_outbound;
  CHECK(srtp_create(&d.srtp_out, &policy) == 0);
  policy.ssrc.type = ssrc_any_inbound;
  CHECK(srtp_create(&d.srtp_in, &policy) == 0);
  uint8_t p[128] = {0x80, 111, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0xf8, 0xff, 0xfe};
  int n = 15;
  CHECK(!dtls_srtp_encrypt_rtp_packet(&d, p, &n));
  uint8_t replay[128], bad[128];
  memcpy(replay, p, (size_t)n);
  memcpy(bad, p, (size_t)n);
  bad[n - 1] ^= 1;
  int bad_n = n;
  CHECK(dtls_srtp_decrypt_rtp_packet(&d, bad, &bad_n));
  CHECK(bad_n == 0);
  int original_n = n;
  CHECK(!dtls_srtp_decrypt_rtp_packet(&d, p, &n));
  CHECK(n == 15 && p[12] == 0xf8);
  CHECK(dtls_srtp_decrypt_rtp_packet(&d, replay, &original_n));
  CHECK(original_n == 0);
  uint8_t rtcp[128] = {0x80, 201, 0, 1, 0, 0, 0, 1};
  n = 8;
  CHECK(!dtls_srtp_encrypt_rctp_packet(&d, rtcp, &n));
  memcpy(replay, rtcp, (size_t)n);
  original_n = n;
  CHECK(!dtls_srtp_decrypt_rtcp_packet(&d, rtcp, &n));
  CHECK(!rtcp_validate(rtcp, (size_t)n));
  CHECK(dtls_srtp_decrypt_rtcp_packet(&d, replay, &original_n));
  CHECK(original_n == 0);
  n = 8;
  CHECK(dtls_srtp_decrypt_rtcp_packet(&d, rtcp, &n));
  CHECK(n == 0);
  srtp_dealloc(d.srtp_in);
  srtp_dealloc(d.srtp_out);
}
static unsigned messages;
static void on_message(char *p, size_t n, void *ctx, uint16_t sid) {
  (void)p;
  (void)n;
  (void)ctx;
  (void)sid;
  ++messages;
}
static void test_dcep(void) {
  DtlsSrtp dtls = {.role = DTLS_SRTP_ROLE_SERVER};
  Sctp s = {.dtls_srtp = &dtls,
            .connected = 1,
            .onmessage = on_message,
            .stream_count = 1};
  s.stream_table[0].sid = 1;
  s.stream_table[0].state = 1;
  uint8_t ack = 2;
  CHECK(!sctp_handle_message(&s, &ack, 1, PPID_CONTROL, 1));
  CHECK(!sctp_handle_message(&s, (const uint8_t *)"hello", 5, PPID_STRING, 1));
  CHECK(messages == 1);
  CHECK(sctp_handle_message(&s, (const uint8_t *)"hello", 5, PPID_STRING, 3));
  const uint8_t invalid[] = {0xc0, 0x80};
  CHECK(sctp_handle_message(&s, invalid, sizeof(invalid), PPID_STRING, 1));
  CHECK(messages == 1);
  uint8_t open[12] = {3};
  wire_put16(open + 8, 65535);
  CHECK(sctp_handle_message(&s, open, sizeof(open), PPID_CONTROL, 0));
  CHECK(s.stream_count == 1);
  CHECK(!wire_utf8(invalid, 2));
  CHECK(!wire_utf8((const uint8_t *)"\xed\xa0\x80", 3));
  CHECK(wire_utf8((const uint8_t *)"\xf0\x9f\x8e\x99", 4));
}
static void readable(int fd) {
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(fd, &fds);
  struct timeval timeout = {1, 0};
  CHECK(select(fd + 1, &fds, NULL, NULL, &timeout) == 1);
}
static void test_udp_boundary(void) {
  Agent a;
  CHECK(!agent_create(&a));
  UdpSocket sender = {.fd = -1}, stranger = {.fd = -1};
  CHECK(!udp_socket_open_on(&sender, AF_INET, 0, "127.0.0.1"));
  CHECK(!udp_socket_open_on(&stranger, AF_INET, 0, "127.0.0.1"));
  IceCandidate remote = {.addr = sender.bind_addr};
  IceCandidatePair pair = {.remote = &remote};
  a.selected_pair = &pair;
  Address target = a.udp_sockets[0].bind_addr;
  CHECK(addr_from_string("127.0.0.1", &target));
  uint8_t dtls[13] = {22, 0xfe, 0xfd}, out[STUN_ATTR_BUF_SIZE];
  CHECK(udp_socket_sendto(&sender, &target, dtls, sizeof(dtls)) ==
        sizeof(dtls));
  readable(a.udp_sockets[0].fd);
  CHECK(agent_recv(&a, out, sizeof(out)) ==
        sizeof(dtls)); /* DTLS must not be consumed as STUN. */
  CHECK(udp_socket_sendto(&stranger, &target, dtls, sizeof(dtls)) ==
        sizeof(dtls));
  readable(a.udp_sockets[0].fd);
  CHECK(agent_recv(&a, out, sizeof(out)) ==
        0); /* A different source port is not the selected peer. */
  uint8_t large[STUN_ATTR_BUF_SIZE + 1] = {22};
  CHECK(udp_socket_sendto(&sender, &target, large, sizeof(large)) ==
        sizeof(large));
  readable(a.udp_sockets[0].fd);
  CHECK(agent_recv(&a, out, sizeof(out)) == 0); /* Reject, never truncate. */
  a.selected_pair = NULL;
  CHECK(udp_socket_sendto(&sender, &target, dtls, sizeof(dtls)) ==
        sizeof(dtls));
  readable(a.udp_sockets[0].fd);
  CHECK(agent_recv(&a, out, sizeof(out)) ==
        0); /* No DTLS before ICE authentication. */
  agent_destroy(&a);
  udp_socket_close(&sender);
  udp_socket_close(&stranger);
}
static void test_stun_discovery(void) {
  Agent a;
  CHECK(!agent_create(&a));
  a.host_addr.family = AF_INET;
  CHECK(addr_from_string("127.0.0.1", &a.host_addr));
  UdpSocket server = {.fd = -1};
  CHECK(!udp_socket_open_on(&server, AF_INET, 0, "127.0.0.1"));
  char url[64];
  snprintf(url, sizeof(url), "stun:127.0.0.1:%u", server.bind_addr.port);
  pid_t child = fork();
  CHECK(child >= 0);
  if (!child) {
    readable(server.fd);
    Address source;
    uint8_t request[1536];
    int n = udp_socket_recvfrom(&server, &source, request, sizeof(request));
    CHECK(n >= 20 && !stun_probe(request, (size_t)n));
    for (int invalid = 1; invalid >= 0; --invalid) {
      StunMessage response;
      stun_msg_create(&response, 0x0101);
      memcpy(response.buf + 8, request + 8, 12);
      Address mapped = source;
      if (invalid)
        addr_set_port(&mapped,
                      1); /* A bad fingerprint must not select this mapping. */
      uint8_t attr[20];
      int length =
          stun_set_mapped_address((char *)attr, response.buf + 4, &mapped);
      CHECK(!stun_msg_write_attr(&response, STUN_ATTR_TYPE_XOR_MAPPED_ADDRESS,
                                 (uint16_t)length, (char *)attr));
      uint32_t fingerprint;
      wire_put16(response.buf + 2, (uint16_t)(response.size + 8 - 20));
      stun_calculate_fingerprint((char *)response.buf, response.size,
                                 &fingerprint);
      if (invalid)
        fingerprint ^= 1;
      CHECK(!stun_msg_write_attr(&response, STUN_ATTR_TYPE_FINGERPRINT, 4,
                                 (char *)&fingerprint));
      CHECK(udp_socket_sendto(&server, &source, response.buf,
                              (int)response.size) > 0);
    }
    _exit(0);
  }
  agent_gather_candidate(&a, NULL, NULL, NULL);
  agent_gather_candidate(&a, url, NULL, NULL);
  int status;
  CHECK(waitpid(child, &status, 0) == child);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  CHECK(a.local_candidates_count == 2);
  CHECK(a.local_candidates[1].addr.port == a.udp_sockets[0].bind_addr.port);
  CHECK(a.local_candidates[1].type == ICE_CANDIDATE_TYPE_SRFLX);
  agent_gather_candidate(&a, "turn:invalid", NULL, NULL);
  CHECK(a.local_candidates_count == 2);
  agent_gather_candidate(&a, "stun:127.0.0.1:70000", NULL, NULL);
  CHECK(a.local_candidates_count == 2);
  agent_destroy(&a);
  udp_socket_close(&server);
  Address loopback = {.family = AF_INET};
  CHECK(ports_get_host_addr(&loopback, "lo") == 1);
  CHECK(ports_get_host_addr(&loopback, "no-such-peer-interface") == 0);
}
static void test_ice_authentication(void) {
  Agent a;
  CHECK(!agent_create(&a));
  UdpSocket remote = {.fd = -1}, stranger = {.fd = -1};
  CHECK(!udp_socket_open_on(&remote, AF_INET, 0, "127.0.0.1"));
  CHECK(!udp_socket_open_on(&stranger, AF_INET, 0, "127.0.0.1"));
  a.host_addr.family = AF_INET;
  CHECK(addr_from_string("127.0.0.1", &a.host_addr));
  agent_create_ice_credential(&a);
  agent_gather_candidate(&a, NULL, NULL, NULL);
  strcpy(a.remote_ufrag, "abcdefghijklmnop");
  strcpy(a.remote_upwd, "abcdefghijklmnopqrstuvwxyz012345");
  a.remote_candidates_count = 1;
  a.remote_candidates[0].addr = remote.bind_addr;
  a.mode = AGENT_MODE_CONTROLLING;
  agent_update_candidate_pairs(&a);
  CHECK(!agent_select_candidate_pair(&a));
  CHECK(agent_send_binding_request(&a) > 0);
  Address destination;
  uint8_t packet[STUN_ATTR_BUF_SIZE], replybuf[STUN_ATTR_BUF_SIZE];
  readable(remote.fd);
  int n = udp_socket_recvfrom(&remote, &destination, packet, sizeof(packet));
  CHECK(n > 0);
  CHECK(!stun_msg_is_valid(packet, (size_t)n, a.remote_upwd));
  StunMessage response;
  stun_msg_create(&response, 0x0101);
  memcpy(response.buf + 8, packet + 8, 12);
  CHECK(!stun_msg_finish(&response, STUN_CREDENTIAL_SHORT_TERM, a.remote_upwd,
                         strlen(a.remote_upwd)));
  CHECK(udp_socket_sendto(&stranger, &destination, response.buf,
                          (int)response.size) > 0);
  readable(a.udp_sockets[0].fd);
  CHECK(!agent_recv(&a, replybuf, sizeof(replybuf)));
  CHECK(a.binding_request_pending);
  StunMessage wrong_id;
  stun_msg_create(&wrong_id, 0x0101);
  memcpy(wrong_id.buf + 8, packet + 8, 12);
  wrong_id.buf[8] ^= 1;
  CHECK(!stun_msg_finish(&wrong_id, STUN_CREDENTIAL_SHORT_TERM, a.remote_upwd,
                         strlen(a.remote_upwd)));
  CHECK(udp_socket_sendto(&remote, &destination, wrong_id.buf,
                          (int)wrong_id.size) > 0);
  readable(a.udp_sockets[0].fd);
  CHECK(!agent_recv(&a, replybuf, sizeof(replybuf)));
  CHECK(a.binding_request_pending);
  response.buf[response.size - 1] ^= 1;
  CHECK(udp_socket_sendto(&remote, &destination, response.buf,
                          (int)response.size) > 0);
  readable(a.udp_sockets[0].fd);
  CHECK(!agent_recv(&a, replybuf, sizeof(replybuf)));
  CHECK(a.binding_request_pending);
  response.buf[response.size - 1] ^= 1;
  CHECK(udp_socket_sendto(&remote, &destination, response.buf,
                          (int)response.size) > 0);
  readable(a.udp_sockets[0].fd);
  CHECK(!agent_recv(&a, replybuf, sizeof(replybuf)));
  CHECK(!a.binding_request_pending);
  CHECK(a.nominated_pair->state == ICE_CANDIDATE_STATE_SUCCEEDED);
  a.last_consent = 42; /* A replay of the old authenticated response cannot
                          refresh consent. */
  CHECK(udp_socket_sendto(&remote, &destination, response.buf,
                          (int)response.size) > 0);
  readable(a.udp_sockets[0].fd);
  CHECK(!agent_recv(&a, replybuf, sizeof(replybuf)));
  CHECK(a.last_consent == 42);
  agent_destroy(&a);
  udp_socket_close(&remote);
  udp_socket_close(&stranger);
}
static void test_fragments(void) {
  DtlsSrtp dtls = {.role = DTLS_SRTP_ROLE_SERVER};
  Sctp s = {.dtls_srtp = &dtls,
            .connected = 1,
            .onmessage = on_message,
            .stream_count = 1};
  s.stream_table[0].sid = 1;
  s.stream_table[0].state = 2;
  uint8_t *large = malloc(PEER_MESSAGE_LIMIT);
  CHECK(large != NULL);
  memset(large, 'a', PEER_MESSAGE_LIMIT);
  unsigned before = messages;
  sctp_receive_chunk(&s, large, PEER_MESSAGE_LIMIT, PPID_STRING, 1, 0);
  sctp_receive_chunk(&s, large, 1, PPID_STRING, 1, 0);
  sctp_receive_chunk(&s, large, 4, PPID_STRING, 1, 1);
  CHECK(messages == before);
  sctp_receive_chunk(&s, large, 4, PPID_STRING, 1, 1);
  CHECK(messages == before + 1);
  sctp_receive_chunk(&s, large, 3, PPID_STRING, 1, 0);
  sctp_receive_chunk(&s, large, 3, PPID_BINARY, 1, 1);
  CHECK(messages == before + 1);
  s.stream_table[0].state = 1;
  uint8_t ack = 2;
  sctp_receive_chunk(&s, &ack, 1, PPID_CONTROL, 1, 0);
  sctp_receive_chunk(&s, &ack, 1, PPID_CONTROL, 1, 1);
  CHECK(s.stream_table[0].state == 1);
  sctp_receive_chunk(&s, &ack, 1, PPID_CONTROL, 1, 1);
  CHECK(s.stream_table[0].state == 2);
  sctp_destroy_association(&s);
  free(large);
}
static int descriptor_count(void) {
  int count = 0;
  for (int i = 0; i < 256; ++i)
    if (fcntl(i, F_GETFD) != -1)
      ++count;
  return count;
}
static void test_lifecycle(void) {
  int baseline = descriptor_count();
  PeerConfiguration config = {.local_address = "127.0.0.1",
                              .audio_codec = CODEC_OPUS};
  CHECK(peer_connection_create(NULL) == NULL);
  for (int i = 0; i < 30; ++i) {
    PeerConnection *pc = peer_connection_create(&config);
    CHECK(pc != NULL);
    CHECK(peer_deinit() == -1);
    const char *sdp = peer_connection_create_offer(pc);
    CHECK(sdp != NULL);
    CHECK(peer_connection_set_remote_description(pc, sdp, SDP_TYPE_ANSWER));
    CHECK(peer_connection_get_state(pc) == PEER_CONNECTION_NEW);
    CHECK(peer_connection_set_remote_description_n(pc, "bad", 3,
                                                   SDP_TYPE_ANSWER));
    peer_connection_close(pc);
    peer_connection_close(pc);
    CHECK(peer_connection_get_state(pc) == PEER_CONNECTION_CLOSED);
    CHECK(peer_connection_send_audio(pc, (const uint8_t *)"a", 1));
    peer_connection_destroy(pc);
  }
  CHECK(descriptor_count() == baseline);
  config.video_codec = CODEC_H264;
  CHECK(peer_connection_create(&config) == NULL);
  config.video_codec = CODEC_NONE;
  config.ice_servers[0].urls = "turn:example.com:3478";
  CHECK(peer_connection_create(&config) == NULL);
  char a[33], b[33];
  CHECK(!utils_random_string(a, 32));
  CHECK(!utils_random_string(b, 32));
  CHECK(strcmp(a, b));
}
int main(void) {
  CHECK(!peer_init());
  test_stun();
  test_rtp_rtcp();
  test_sdp();
  test_crypto();
  test_dcep();
  test_fragments();
  test_udp_boundary();
  test_ice_authentication();
  test_stun_discovery();
  test_lifecycle();
  CHECK(!peer_deinit());
  for (int i = 0; i < 10; ++i) {
    CHECK(!peer_init());
    CHECK(!peer_init());
    CHECK(!peer_deinit());
    CHECK(!peer_deinit());
  }
  printf("PASS %u security assertions\n", checks);
  return 0;
}
