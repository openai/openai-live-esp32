#include "CinemaAudio.hpp"
#include "CinemaCue.hpp"
#include "CinemaEmbedding.hpp"
#include "CinemaPolicy.hpp"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <vector>
using namespace cinema;
void put16(std::vector<uint8_t> &b, size_t p, unsigned v) {
  b[p] = v;
  b[p + 1] = v >> 8;
}
void put32(std::vector<uint8_t> &b, size_t p, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    b[p + i] = v >> (8 * i);
}
int main(int argc, char **argv) {
  {
    for (unsigned i = 0; i < 30000; ++i) {
      char name[16];
      snprintf(name, sizeof(name), "%04u.SCF", i);
      assert(film_index(name, 30000) == int(i));
    }
    assert(film_index("29999.sCf", 30000) == 29999);
    assert(film_index("0000.SCF", 0) == -1);
    assert(film_index("30000.SCF", 30000) == -1);
    for (const auto *name : {"", "0.SCF", "00000.SCF", "-001.SCF", "0000.SCFx",
                             "0000xSCF", "0000.SCX", "abcd.SCF", "../0.SCF"})
      assert(film_index(name, 30000) == -1);
    assert(film_index(nullptr, 30000) == -1);
  }
  {
    size_t written = 0, calls = 0;
    const auto silent = [&](const int16_t *pcm, size_t n) {
      assert(n <= 480);
      for (size_t i = 0; i < n; ++i)
        assert(pcm[i] == 0);
      written += n;
      ++calls;
      return 0;
    };
    assert(cinema::write_pcm(true, nullptr, 5761, silent) == 0);
    assert(written == 5761 && calls == 13);
    assert(cinema::write_pcm(true, nullptr, 1440,
                             [](const int16_t *, size_t) { return -7; }) == -7);
    int16_t source[]{1, 2, 3};
    assert(
        cinema::write_pcm(false, source, 3, [&](const int16_t *pcm, size_t n) {
          assert(pcm == source && n == 3);
          return 0;
        }) == 0);
  }

  struct Word {
    uint32_t offset;
    float scale, norm;
    int8_t vector[3];
  };
  const Word words[] = {{0, .5f, 2.f, {2, 0, 0}},
                        {4, .25f, 4.f, {0, 4, 0}},
                        {8, .2f, 5.f, {0, 0, 5}}};
  const char vocabulary[] = "cat\0dog\0train";
  float embedding[3];
  unsigned matched = 0;
  assert(embed_local("CAT, dog! cat", embedding, 3, words, 3, vocabulary,
                     &matched));
  assert(matched == 3 && std::abs(embedding[0] - 2 / std::sqrt(5.f)) < 1e-6 &&
         std::abs(embedding[1] - 1 / std::sqrt(5.f)) < 1e-6);
  assert(
      !embed_local("unknown \xe7\x8c\xab", embedding, 3, words, 3, vocabulary));
  assert(!embed_local((std::string(50, 'x') + "cat").c_str(), embedding, 3,
                      words, 3, vocabulary));
  assert(embed_local((std::string(50, 'x') + " cat").c_str(), embedding, 3,
                     words, 3, vocabulary));
  uint16_t query_ids[16]{};
  const unsigned nq =
      keywords("cat cat dog", words, 3, vocabulary, query_ids, 16);
  assert(nq == 2 && query_ids[0] == 0 && query_ids[1] == 1);
  const uint16_t exact[] = {0, 1, 65535}, unrelated[] = {2, 65535};
  assert(keyword_similarity(query_ids, nq, exact, 3, words, 3, 3) == 1.f);
  assert(keyword_similarity(query_ids, nq, unrelated, 2, words, 3, 3) == 0.f);
  assert(exact_coverage(query_ids, nq, exact, 3, words, 3) == 1.f);
  assert(exact_coverage(query_ids, nq, unrelated, 2, words, 3) == 0.f);
  Word salient[] = {words[0], words[1], words[2]};
  salient[2].scale = 1.f; // A late, important subject must displace filler.
  assert(keywords("cat dog train", salient, 3, vocabulary, query_ids, 2) == 2);
  assert(query_ids[0] == 2 && query_ids[1] == 0);
  assert(!embed_local("cat", embedding, 0, words, 3, vocabulary));
  auto *scene = cJSON_Parse(SCENE_TOOL);
  assert(scene && !strcmp(cJSON_GetObjectItem(scene, "name")->valuestring,
                          "set_visual_scene"));
  cJSON_Delete(scene);

  assert(crc32(reinterpret_cast<const uint8_t *>("123456789"), 9) ==
         0xcbf43926);
  std::vector<uint8_t> good(44);
  memcpy(good.data(), "SCFILM1\0", 8);
  put16(good, 8, 320);
  put16(good, 10, 240);
  put16(good, 12, 12);
  put16(good, 14, 1);
  put32(good, 16, 44);
  put32(good, 20, 4);
  put32(good, 32, 40);
  put32(good, 36, 4);
  good[40] = good[42] = 0xff;
  good[41] = 0xd8;
  good[43] = 0xd9;
  put32(good, 24, crc32(good.data() + 40, 4));
  Film f;
  assert(f.parse(good.data(), good.size()) && f.count == 1 && f.fps == 12);
  for (size_t size = 0; size < good.size(); ++size)
    assert(!f.parse(good.data(), size) && f.count == 0);
  for (auto p : {0u, 8u, 10u, 16u, 20u, 24u, 28u, 32u, 36u, 40u, 43u}) {
    auto bad = good;
    bad[p] ^= 1;
    assert(!f.parse(bad.data(), bad.size()) && f.count == 0);
  }
  for (auto value : {0u, 21u, 65535u}) {
    auto bad = good;
    put16(bad, 12, value);
    assert(!f.parse(bad.data(), bad.size()));
  }
  for (auto value : {0u, 145u, 65535u}) {
    auto bad = good;
    put16(bad, 14, value);
    assert(!f.parse(bad.data(), bad.size()));
  }
  for (auto value : {0u, 3u, 32769u, 0xffffffffu}) {
    auto bad = good;
    put32(bad, 36, value);
    assert(!f.parse(bad.data(), bad.size()));
  }
  assert(!f.parse(nullptr, 44));
  float q[] = {3, 4};
  assert(normalize(q, 2));
  int8_t v[] = {60, 80};
  assert(std::abs(cosine(q, v, 100, 2) - 1) < 1e-6);
  float zero[] = {0, 0};
  assert(!normalize(zero, 2));
  float nan[] = {std::numeric_limits<float>::quiet_NaN()};
  assert(!normalize(nan, 1));
  float inf[] = {std::numeric_limits<float>::infinity()};
  assert(!normalize(inf, 1));
  Ranking rank;
  History history;
  assert(rank.choose(history, 0) == -1);
  rank.add(1, .8f, 1);
  rank.add(2, .79f, 2);
  rank.add(3, .1f, 3);
  history.add(1, 1);
  assert(rank.choose(history, 0x80000000) == 2);
  assert(rank.choose(history, 0) ==
         2); // No immediate repeat when a relevant alternative exists.
  for (unsigned i = 0; i < 1000; ++i)
    assert(rank.choose(history, i * 4294967u) !=
           3); // No unrelated random picks.
  for (unsigned i = 0; i < 100; ++i)
    rank.add(i + 10, float(i) / 100, i);
  assert(rank.count == 8 && rank.best[0].id == 109 && rank.best[7].id == 102);
  assert(frame_at(-1, 12, 96) == 0);
  assert(frame_at(3000000, 12, 96) == 36);
  assert(frame_at(1000000000, 12, 96) == 96);
  assert(frame_at(100, 0, 0) == 0);
  assert(frame_at(100, 1000001, 96) == 0);
  assert(!transition_due(23, 12, 96, true));
  assert(transition_due(24, 12, 96, true));
  assert(!transition_due(95, 12, 96, false));
  assert(transition_due(96, 12, 96, false));
  assert(!transition_due(200, 0, 0, true));
  assert(loop_frame(95, 96) == 95);
  assert(loop_frame(96, 96) == 0); // Slow prefetch keeps the picture moving.
  assert(loop_frame(193, 96) == 1);
  assert(loop_frame(UINT64_MAX, 0) == 0);
  char cue[257];
  transcript_cue(nullptr, cue);
  assert(!cue[0]);
  transcript_cue("a short thought", cue);
  assert(!strcmp(cue, "a short thought"));
  const std::string tail(254, 'x');
  transcript_cue((std::string(40, 'a') + "\xe7\x8c\xab" + tail).c_str(), cue);
  assert(cue == tail); // Cut starts inside the three-byte cat character.
  const std::string emoji_tail = "\xf0\x9f\x90\x88" + std::string(252, 'b');
  transcript_cue((std::string(40, 'a') + emoji_tail).c_str(), cue);
  assert(cue == emoji_tail); // Complete four-byte boundary survives.
  assert(parse_cue("{\"description\":\"old telephone\"}", cue));
  assert(!strcmp(cue, "old telephone"));
  for (auto bad :
       {"null", "[]", "{}", "{\"description\":1}", "{\"description\":\"xy\"}",
        "{\"description\":\"abc\",\"extra\":true}",
        "{\"description\":\"abc\",\"description\":\"def\"}",
        "{\"description\":\"abc\\u0000hidden\"}",
        "{\"description\":\"abc\"} trailing"})
    assert(!parse_cue(bad, cue) && !cue[0]);
  std::string huge = "{\"description\":\"" + std::string(257, 'a') + "\"}";
  assert(!parse_cue(huge.c_str(), cue));
  Mailbox mailbox;
  uint32_t old = 0, latest = 0;
  assert(!mailbox.take(cue, old));
  assert(mailbox.submit("first conversation", 0, false));
  assert(mailbox.take(cue, old));
  assert(mailbox.accepts(old));
  assert(mailbox.submit("camera counterpart", 1, true));
  assert(!mailbox.accepts(old));
  assert(!mailbox.submit("streamed delta", 20000000, false));
  assert(mailbox.take(cue, latest) && !strcmp(cue, "camera counterpart"));
  mailbox.invalidate(); // Stop/off must invalidate even a request already in
                        // flight.
  assert(!mailbox.accepts(latest));
  assert(mailbox.submit("next session", 20000001, false));
  assert(mailbox.take(cue, latest) && !strcmp(cue, "next session"));
  // Also validate real exported pack files, without needing the embedded codec.
  for (int i = 1; i < argc; ++i) {
    std::ifstream input(argv[i], std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(input)), {});
    if (!f.parse(data.data(), data.size())) {
      fprintf(stderr, "Invalid film: %s\n", argv[i]);
      return 1;
    }
  }
  printf("Cinema parser/selection checks passed; %d exported files validated\n",
         argc - 1);
}
