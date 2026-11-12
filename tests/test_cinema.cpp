#include "CinemaAudio.hpp"
#include "CinemaCue.hpp"
#include "CinemaEmbedding.hpp"
#include "CinemaPolicy.hpp"
#include "CinemaStorage.hpp"
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
    for (unsigned i = 0; i < 30000; ++i) {
      char path[40], expected[40];
      snprintf(expected, sizeof(expected), "CINEMA3/%02X/%04u.SCF", i / 256, i);
      assert(film_path(i, true, path, sizeof(path)) && !strcmp(path, expected));
      snprintf(expected, sizeof(expected), "CINEMA/%04u.SCF", i);
      assert(film_path(i, false, path, sizeof(path)) &&
             !strcmp(path, expected));
    }
    char small[8];
    assert(!film_path(30000, true, small, sizeof(small)));
    assert(!film_path(0, true, small, sizeof(small)));
    assert(!film_path(0, true, nullptr, 0));
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
  {
    History recent;
    assert(recent.score_penalty(1, 10) == 0);
    recent.add(1, 10);
    assert(std::abs(recent.score_penalty(1, 10) - .08f) < 1e-6);
    assert(std::abs(recent.score_penalty(2, 10) - .02f) < 1e-6);
    assert(recent.score_penalty(2, 20) == 0);
    float last = recent.score_penalty(1, 10);
    for (unsigned age = 1; age <= 8; ++age) {
      recent.add(100 + age, 100 + age);
      const float penalty = recent.score_penalty(1, 10);
      assert(penalty < last); // Old clips recover gradually, then fully.
      assert(std::abs(penalty - .08f * float(8 - age) / 8) < 1e-6);
      assert(std::abs(recent.score_penalty(2, 10) - penalty / 4) < 1e-6);
      last = penalty;
    }
    for (unsigned i = 0; i < 30; ++i)
      recent.add(1, 10);
    assert(std::abs(recent.score_penalty(1, 10) - .08f) < 1e-6);
    assert(std::abs(recent.score_penalty(2, 10) - .02f) < 1e-6);
  }
  {
    Ranking close;
    close.add(1, .90f, 10);
    close.add(2, .89f, 20);
    close.add(3, .82f, 30);
    History recent, empty;
    recent.add(1, 10);
    recent.add(99, 99); // Top match was recent, but is not the current clip.
    assert(close.choose(empty, 0, true) == 1);
    VisualTopic topic;
    for (int i = 0; i < 10; ++i) {
      assert(topic.update(close, i * 1000000, 'u'));
      assert(topic.fresh);
      assert(topic.ranking.choose(recent, 0, topic.fresh) == 2);
      // New transcript chunks cannot bypass the penalty on the first pick.
    }
    unsigned baseline_repeats = 0, penalized_repeats = 0;
    for (unsigned i = 0; i < 10000; ++i) {
      const uint32_t draw = i * 429496u;
      baseline_repeats += close.choose(empty, draw) == 1;
      const int chosen = close.choose(recent, draw);
      penalized_repeats += chosen == 1;
      assert(chosen == 1 || chosen == 2); // No weak match, even after penalty.
    }
    assert(penalized_repeats > 0); // A soft penalty, not a blanket ban.
    assert(penalized_repeats < baseline_repeats / 4);
    // Another clip from the same film gets a smaller preference adjustment.
    History source;
    source.add(42, 10);
    source.add(99, 99);
    assert(close.choose(source, 0, true) == 2);
    // Immediate repeats are skipped for every possible draw endpoint and fresh
    // cues, even when every good candidate has already been played.
    recent.add(2, 20);
    recent.add(1, 10);
    for (const auto draw : {0u, 0x80000000u, UINT32_MAX}) {
      assert(close.choose(recent, draw) == 2);
      assert(close.choose(recent, draw, true) == 2);
    }
    Ranking only_good;
    only_good.add(1, .90f, 10);
    only_good.add(3, .82f,
                  30); // Would win if the raw relevance guard vanished.
    assert(only_good.choose(recent, 0, true) == 1);
    assert(only_good.choose(recent, UINT32_MAX) == 1);
    Ranking one;
    one.add(1, .90f, 10);
    assert(one.choose(recent, UINT32_MAX, true) == 1);
    assert(one.choose(recent, UINT32_MAX) == 1);
    assert(Ranking{}.choose(recent, 0, true) == -1);
    for (unsigned i = 0; i < 8; ++i)
      recent.add(100 + i, 100 + i);
    assert(close.choose(recent, 0, true) == 1); // Recovery after eight plays.
    assert(close.best[0].score == .90f); // Never rewrite the library's scores.
  }
  for (unsigned i = 0; i < 100; ++i)
    rank.add(i + 10, float(i) / 100, i);
  assert(rank.count == 8 && rank.best[0].id == 109 && rank.best[7].id == 102);
  {
    VisualTopic topic;
    Ranking boats, sports, weak;
    boats.add(10, .95f, 1);
    sports.add(20, .89f, 2);
    weak.add(30, MIN_RELEVANCE - .001f, 3);
    assert(!topic.active(0));
    assert(!topic.update(weak, 0, 'u'));
    assert(!topic.active(0)); // A miss cannot invent a topic.
    assert(topic.update(boats, 0, 'u'));
    assert(topic.fresh && topic.ranking.best[0].id == 10);
    assert(!topic.update(weak, 1000000, 'a'));
    assert(topic.fresh); // A pending best match must survive filler too.
    topic.fresh = false; // First matching clip was actually displayed.
    for (int64_t seconds = 10; seconds < 180; seconds += 10) {
      const auto now = seconds * 1000000;
      assert(!topic.update(weak, now, 'a'));
      assert(!topic.update(Ranking{}, now, 'u')); // Unknown vocabulary.
      assert(topic.active(now) && topic.ranking.best[0].id == 10);
      assert(
          !topic.fresh); // Do not cancel prefetch or restart best-only picks.
      assert(topic.expires_at == 180000000); // Filler cannot renew forever.
    }
    assert(topic.active(179999999));
    assert(!topic.active(180000000));
    assert(topic.update(weak, 180000000, 'u'));
    assert(topic.ranking.count == 0);
    assert(topic.update(boats, 200000000, 'u'));
    assert(topic.update(sports, 205000000, 'a'));
    assert(topic.fresh && topic.ranking.best[0].id == 20);
    assert(topic.expires_at == 385000000); // Strong topic changes take effect.
    assert(topic.update(boats, 210000000, 't'));
    assert(topic.expires_at == 255000000); // Explicit cues keep their policy.
    assert(!topic.update(weak, 220000000, 'u'));
    assert(topic.update(weak, 230000000, 't'));
    assert(!topic.active(230000000)); // Do not misrepresent a failed request.
    assert(topic.update(sports, 240000000, 'u'));
    topic.clear(); // Session end / cinema off discards the held topic.
    assert(!topic.active(240000000) && !topic.fresh && !topic.expires_at);
  }
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
  bool seen_visible = false;
  assert(!visibility_paused(true, false,
                            seen_visible)); // First GUI refresh pending.
  assert(!visibility_paused(true, true, seen_visible));
  assert(visibility_paused(true, false, seen_visible)); // Photo/usage overlay.
  assert(
      !visibility_paused(true, true, seen_visible)); // Resume existing movie.
  assert(!visibility_paused(false, false, seen_visible));
  assert(
      !visibility_paused(true, false, seen_visible)); // Re-enable can publish.
  {
    int64_t clock = 0;
    unsigned attempts = 0, reads = 0;
    bool cancelled = false;
    const auto now = [&] { return clock; };
    const auto stopped = [&] { return cancelled; };
    const auto wait = [&] { clock += 100000; };
    assert(retry_card_access(
               [&] {
                 if (++attempts < 3)
                   return false; // Busy display, not a bad SD file.
                 ++reads;
                 return true;
               },
               stopped, now, wait) == ReadResult::ok);
    assert(attempts == 3 && reads == 1);
    attempts = 0;
    assert(retry_card_access(
               [&] {
                 ++attempts;
                 return false;
               },
               stopped, now, wait) == ReadResult::retry);
    assert(attempts == 21); // Finite wait, preserves retryable classification.
    attempts = 0;
    assert(retry_card_access(
               [&] {
                 ++attempts;
                 cancelled = true;
                 return false;
               },
               stopped, now, wait) == ReadResult::cancelled);
    assert(attempts == 1); // Stop/photo cancels before another lock attempt.
  }
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
