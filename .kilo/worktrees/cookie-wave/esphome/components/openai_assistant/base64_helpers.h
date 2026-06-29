#ifndef BASE64_HELPERS_H
#define BASE64_HELPERS_H

#include <string>
#include <vector>
#include <cstdint>

namespace esphome {
namespace utils {

/// Encode a byte buffer to base64 string.
std::string base64_encode(const uint8_t *buf, size_t buf_len);
/// Encode a byte vector to base64 string.
std::string base64_encode(const std::vector<uint8_t> &buf);

} // namespace utils
} // namespace esphome

#endif // BASE64_HELPERS_H
