#include "base64_helpers.h"
#include <string>
#include <vector>
#include <cstdint>

namespace esphome {
namespace utils {

static constexpr const char *BASE64_CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                            "abcdefghijklmnopqrstuvwxyz"
                                            "0123456789+/";

static inline void base64_encode_triple(const char *char_array_3, int count, std::string &ret) {
  char char_array_4[4];
  char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
  char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
  char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
  char_array_4[3] = char_array_3[2] & 0x3f;

  for (int j = 0; j < count; j++)
    ret += BASE64_CHARS[static_cast<uint8_t>(char_array_4[j])];
}

std::string base64_encode(const std::vector<uint8_t> &buf) {
  return base64_encode(buf.data(), buf.size());
}

std::string base64_encode(const uint8_t *buf, size_t buf_len) {
  std::string ret;
  int i = 0;
  char char_array_3[3];

  while (buf_len--) {
    char_array_3[i++] = *(buf++);
    if (i == 3) {
      base64_encode_triple(char_array_3, 4, ret);
      i = 0;
    }
  }

  if (i) {
    for (int j = i; j < 3; j++)
      char_array_3[j] = '\0';

    base64_encode_triple(char_array_3, i + 1, ret);

    while ((i++ < 3))
      ret += '=';
  }

  return ret;
}

} // namespace utils
} // namespace esphome
