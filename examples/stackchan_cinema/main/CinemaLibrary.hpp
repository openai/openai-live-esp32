#pragma once
#include "CinemaSearch.hpp"
#include "cJSON.h"
#include <cstdio>

namespace cinema {
// Explicit playback lasts one complete clip. Transcript cues cannot replace a
// pending/playing request, but another explicit request can supersede it.
struct VideoRequest {
  enum class State { none, queued, playing, finished, failed, cancelled };
  State state = State::none;
  int clip = -1;
  uint32_t epoch = 0;
  int64_t deadline = 0;
  bool blocks() const {
    return state == State::queued || state == State::playing;
  }
  int pending_clip(int active, uint32_t active_epoch) const {
    return state == State::queued && !(clip == active && epoch == active_epoch)
               ? clip
               : -1;
  }
  void queue(int id, uint32_t generation, int64_t now) {
    clip = id;
    epoch = generation;
    state = State::queued;
    deadline = now + 15000000;
  }
  bool expire(int64_t now) {
    if (state != State::queued || now < deadline)
      return false;
    state = State::failed;
    return true;
  }
  void published(int id, uint32_t generation) {
    if (state == State::queued && clip == id && epoch == generation)
      state = State::playing;
  }
  void finish(int id, uint32_t generation, bool ok) {
    if (blocks() && clip == id && epoch == generation)
      state = ok ? State::finished : State::failed;
  }
  void cancel() {
    if (blocks())
      state = State::cancelled;
  }
  const char *status() const {
    switch (state) {
    case State::queued:
      return "queued";
    case State::playing:
      return "playing";
    case State::finished:
      return "finished";
    case State::failed:
      return "failed";
    case State::cancelled:
      return "cancelled";
    default:
      return "none";
    }
  }
};

struct LibrarySnapshot {
  const char *pack = "";
  unsigned count = 0;
  int current = -1; // Last frame handed to the display, not the prefetch slot.
  bool enabled = false, visible = false, card = false;
  VideoRequest request;
};
struct VideoInfo {
  const char *title = "", *description = "", *mood = "", *basis = "unspecified";
  double seconds = 0;
  bool candidate = false; // Eligible to load; SD existence/CRC checked on load.
};

inline constexpr char LIBRARY_TOOLS[] = R"(
{"type":"function","name":"search_videos","description":"Search the local silent film catalog: top 8 matches, 3 per page. Empty query browses it. Returns next_offset; scores are not probabilities. Does not play anything.","parameters":{"type":"object","properties":{"query":{"type":"string","maxLength":256},"offset":{"type":"integer","minimum":0,"maximum":30000}},"required":["query","offset"],"additionalProperties":false},"strict":true},
{"type":"function","name":"inspect_video","description":"Read a clip's catalog description, or the current screen clip with null clip_id. Use offset 0 first, then next_offset for more text. Sampled-frame descriptions are not full-video vision.","parameters":{"type":"object","properties":{"clip_id":{"type":["integer","null"],"minimum":0,"maximum":29999},"offset":{"type":"integer","minimum":0,"maximum":4096}},"required":["clip_id","offset"],"additionalProperties":false},"strict":true},
{"type":"function","name":"play_video","description":"Queue one specific catalog clip by returned clip_id. Silent playback, one full clip, then automatic selection resumes. Queued is not displayed; inspect_video checks status. Do not guess IDs.","parameters":{"type":"object","properties":{"clip_id":{"type":"integer","minimum":0,"maximum":29999}},"required":["clip_id"],"additionalProperties":false},"strict":true}
)";
inline bool library_tool(const char *name) {
  return name &&
         (!strcmp(name, "search_videos") || !strcmp(name, "inspect_video") ||
          !strcmp(name, "play_video"));
}

struct LibraryArguments {
  enum class Kind { search, inspect, play } kind;
  int clip = -1;
  unsigned offset = 0;
  char query[257]{};
};
inline bool parse_library_arguments(const char *name, const char *wire,
                                    LibraryArguments &out) {
  if (!library_tool(name) || !wire || strlen(wire) > 512 ||
      strstr(wire, "\\u0000"))
    return false;
  out = {};
  out.kind = !strcmp(name, "search_videos")   ? LibraryArguments::Kind::search
             : !strcmp(name, "inspect_video") ? LibraryArguments::Kind::inspect
                                              : LibraryArguments::Kind::play;
  cJSON *root = cJSON_ParseWithOpts(wire, nullptr, true);
  bool ok = cJSON_IsObject(root);
  unsigned seen = 0;
  for (auto *v = root ? root->child : nullptr; ok && v; v = v->next) {
    unsigned bit = 0;
    if (v->string && !strcmp(v->string, "query") &&
        out.kind == LibraryArguments::Kind::search) {
      bit = 1;
      ok = cJSON_IsString(v) && strlen(v->valuestring) <= 256;
      if (ok)
        strcpy(out.query, v->valuestring);
    } else if (v->string && !strcmp(v->string, "clip_id") &&
               out.kind != LibraryArguments::Kind::search) {
      bit = 2;
      if (cJSON_IsNull(v) && out.kind == LibraryArguments::Kind::inspect)
        out.clip = -1;
      else {
        ok = cJSON_IsNumber(v) && std::isfinite(v->valuedouble) &&
             v->valuedouble == std::floor(v->valuedouble) &&
             v->valuedouble >= 0 && v->valuedouble < 30000;
        if (ok)
          out.clip = int(v->valuedouble);
      }
    } else if (v->string && !strcmp(v->string, "offset") &&
               out.kind != LibraryArguments::Kind::play) {
      bit = 4;
      ok = cJSON_IsNumber(v) && std::isfinite(v->valuedouble) &&
           v->valuedouble == std::floor(v->valuedouble) &&
           v->valuedouble >= 0 &&
           v->valuedouble <=
               (out.kind == LibraryArguments::Kind::inspect ? 4096 : 30000);
      if (ok)
        out.offset = unsigned(v->valuedouble);
    } else
      ok = false;
    if (seen & bit)
      ok = false;
    seen |= bit;
  }
  ok &= seen == (out.kind == LibraryArguments::Kind::search    ? 5u
                 : out.kind == LibraryArguments::Kind::inspect ? 6u
                                                               : 2u);
  cJSON_Delete(root);
  return ok;
}

// Count JSON-escaped bytes and end at a UTF-8 boundary. Catalog strings are
// validated UTF-8 at pack generation; quotes/control bytes cannot overflow the
// shared SDK's 1024-byte tool result. Offsets are bytes, not code points.
inline size_t text_chunk(const char *source, char *out, size_t budget) {
  size_t used = 0, cost = 0;
  while (source[used]) {
    const auto c = static_cast<unsigned char>(source[used]);
    size_t n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    const size_t encoded = c < 32 ? 6 : (c == '"' || c == '\\') ? 2 : n;
    if (cost + encoded > budget)
      break;
    // A defensive guard for malformed/truncated strings, though generated
    // metadata never contains them.
    bool valid = c < 0x80 || (c >= 0xc2 && c <= 0xf4);
    for (size_t i = 1; valid && i < n; ++i)
      valid = source[used + i] &&
              (static_cast<unsigned char>(source[used + i]) & 0xc0) == 0x80;
    if (!valid)
      break;
    memcpy(out + used, source + used, n);
    used += n;
    cost += encoded;
  }
  out[used] = 0;
  return used;
}
struct LibraryJson {
  cJSON *root = cJSON_CreateObject();
  bool ok = root;
  ~LibraryJson() { cJSON_Delete(root); }
  void number(cJSON *obj, const char *key, double n) {
    ok &= cJSON_AddNumberToObject(obj, key, n) != nullptr;
  }
  void boolean(cJSON *obj, const char *key, bool b) {
    ok &= cJSON_AddBoolToObject(obj, key, b) != nullptr;
  }
  void null(cJSON *obj, const char *key) {
    ok &= cJSON_AddNullToObject(obj, key) != nullptr;
  }
  void text(cJSON *obj, const char *key, const char *s) {
    ok &= cJSON_AddStringToObject(obj, key, s) != nullptr;
  }
  size_t chunk(cJSON *obj, const char *key, const char *s, size_t limit) {
    char buffer[385];
    const size_t n = text_chunk(s, buffer, std::min(limit, sizeof(buffer) - 1));
    text(obj, key, buffer);
    return n;
  }
  bool write(char *out, size_t capacity) {
    if (!out || !capacity)
      return false;
    out[0] = 0;
    char *wire = ok ? cJSON_PrintUnformatted(root) : nullptr;
    const bool fits = wire && strlen(wire) < capacity;
    if (fits)
      strcpy(out, wire);
    cJSON_free(wire);
    if (!fits)
      snprintf(out, capacity, "{\"error\":\"result_unavailable\"}");
    return fits;
  }
};

// Backend operations run on the existing low-priority, cancellable tool worker.
// Search/browse never mutate the active scene. Only play() changes playback.
template <class Backend>
bool execute_library_tool(const char *name, const char *arguments, char *out,
                          size_t capacity, Backend &backend) {
  LibraryArguments args;
  LibraryJson json;
  auto *root = json.root;
  auto error = [&](const char *message) {
    json.text(root, "error", message);
    return json.write(out, capacity);
  };
  if (!parse_library_arguments(name, arguments, args))
    return error("invalid_arguments");
  if (!backend.proceed())
    return error("cancelled");
  const auto snapshot = backend.snapshot();
  json.text(root, "pack", snapshot.pack);
  json.number(root, "catalog_clips", snapshot.count);
  json.boolean(root, "enabled", snapshot.enabled);
  json.boolean(root, "card_ready", snapshot.card);
  if (snapshot.current >= 0)
    json.number(root, "screen_clip", snapshot.current);
  else
    json.null(root, "screen_clip");
  json.boolean(root, "screen_visible", snapshot.visible);
  json.text(root, "request_status", snapshot.request.status());
  if (snapshot.request.clip >= 0) {
    json.number(root, "requested_clip", snapshot.request.clip);
    json.number(root, "request_id", snapshot.request.epoch);
  }
  if (args.kind == LibraryArguments::Kind::search) {
    Ranking matches;
    const bool browse = !args.query[0];
    if (!browse) {
      const auto status = backend.search(args.query, matches);
      if (status == SearchResult::cancelled)
        return error("cancelled");
      if (status == SearchResult::unknown_words)
        return error("unrecognized_query_words");
    }
    cJSON *list = cJSON_AddArrayToObject(root, "results");
    json.ok &= list != nullptr;
    unsigned cursor = args.offset, added = 0;
    const unsigned end = browse ? snapshot.count : matches.count;
    for (; cursor < end && added < 3; ++cursor) {
      if (!(cursor & 127) && !backend.proceed())
        return error("cancelled");
      const int id = browse ? int(cursor) : matches.best[cursor].id;
      const auto info = backend.video(id);
      if (!info.candidate ||
          (!browse && matches.best[cursor].score < MIN_RELEVANCE))
        continue;
      cJSON *item = cJSON_CreateObject();
      if (!item || !cJSON_AddItemToArray(list, item)) {
        cJSON_Delete(item);
        json.ok = false;
        break;
      }
      json.number(item, "clip_id", id);
      json.chunk(item, "title", info.title, 40);
      json.chunk(item, "summary", info.description, 64);
      json.text(item, "basis", info.basis);
      if (!browse)
        json.number(item, "score",
                    std::round(matches.best[cursor].score * 1000) / 1000);
      ++added;
    }
    if (cursor < end)
      json.number(root, "next_offset", cursor);
    else
      json.null(root, "next_offset");
    json.text(root, "availability", "checked_on_load");
  } else {
    const int id = args.clip < 0 ? snapshot.current : args.clip;
    if (id < 0 || unsigned(id) >= snapshot.count)
      return error("clip_not_found");
    const auto info = backend.video(id);
    json.number(root, "clip_id", id);
    if (args.kind == LibraryArguments::Kind::play) {
      if (!info.candidate)
        return error("clip_unavailable");
      // Reserve enough result space before committing any playback mutation.
      if (!out || capacity < 1024)
        return error("result_buffer_too_small");
      if (!backend.proceed())
        return error("cancelled");
      // Replace snapshot fields rather than reporting the old request's state.
      cJSON_DeleteItemFromObjectCaseSensitive(root, "request_status");
      cJSON_DeleteItemFromObjectCaseSensitive(root, "requested_clip");
      cJSON_DeleteItemFromObjectCaseSensitive(root, "request_id");
      json.text(root, "request_status", "queued");
      json.number(root, "requested_clip", id);
      json.number(root, "request_id", UINT32_MAX);
      // Allocate/serialize before the side effect. Updating the numeric ticket
      // and printing into this preflighted buffer require no further
      // allocation.
      if (!json.ok ||
          !cJSON_PrintPreallocated(root, out, int(capacity), false)) {
        out[0] = 0;
        return false;
      }
      uint32_t ticket = 0;
      if (!backend.proceed() || !backend.play(id, ticket)) {
        cJSON_DeleteItemFromObjectCaseSensitive(root, "request_status");
        json.text(root, "request_status", "not_queued");
        return error("playback_unavailable");
      }
      cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(root, "request_id"),
                           ticket);
      return cJSON_PrintPreallocated(root, out, int(capacity), false);
    } else {
      const auto length = strlen(info.description);
      if (args.offset > length ||
          (static_cast<unsigned char>(info.description[args.offset]) & 0xc0) ==
              0x80)
        return error("invalid_description_offset");
      json.chunk(root, "title", info.title, 80);
      json.chunk(root, "mood", info.mood, 32);
      json.text(root, "basis", info.basis);
      json.number(root, "seconds", info.seconds);
      json.boolean(root, "load_candidate", info.candidate);
      json.number(root, "description_offset", args.offset);
      const auto used =
          json.chunk(root, "description", info.description + args.offset, 320);
      if (args.offset + used < length)
        json.number(root, "next_offset", args.offset + used);
      else
        json.null(root, "next_offset");
    }
  }
  if (!backend.proceed())
    return error("cancelled");
  return json.write(out, capacity);
}
} // namespace cinema
