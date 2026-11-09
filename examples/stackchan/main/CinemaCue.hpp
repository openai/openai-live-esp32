#pragma once
#include "cJSON.h"
#include <cstring>
namespace cinema {
inline bool parse_cue(const char *arguments, char (&output)[257]) {
  output[0] = 0;
  if (!arguments || strlen(arguments) > 512 || strstr(arguments, "\\u0000"))
    return false;
  auto *root = cJSON_ParseWithOpts(arguments, nullptr, true);
  auto *query = cJSON_GetObjectItemCaseSensitive(root, "description");
  const bool valid = cJSON_IsObject(root) && root->child == query && query &&
                     !query->next && cJSON_IsString(query) &&
                     strlen(query->valuestring) >= 3 &&
                     strlen(query->valuestring) <= 256;
  if (valid)
    strcpy(output, query->valuestring);
  cJSON_Delete(root);
  return valid;
}
} // namespace cinema
