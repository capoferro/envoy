#pragma once

#include "common/common/logger.h"
#include "common/http/header_map_impl.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Cache {

// Byte range from an HTTP request.
class RawByteRange {
public:
  // - If first==UINT64_MAX, construct a RawByteRange requesting the final last
  // body bytes.
  // - Otherwise, construct a RawByteRange requesting the [first,last] body
  // bytes. Prereq: first == UINT64_MAX || first <= last Invariant: isSuffix() ||
  // firstBytePos() <= lastBytePos
  RawByteRange(uint64_t first, uint64_t last) : first_byte_pos_(first), last_byte_pos_(last) {
    RELEASE_ASSERT(isSuffix() || first <= last, "Illegal byte range.");
  }
  bool isSuffix() const { return first_byte_pos_ == UINT64_MAX; }
  uint64_t firstBytePos() const {
    ASSERT(!isSuffix());
    return first_byte_pos_;
  }
  uint64_t lastBytePos() const {
    ASSERT(!isSuffix());
    return last_byte_pos_;
  }
  uint64_t suffixLength() const {
    ASSERT(isSuffix());
    return last_byte_pos_;
  }

private:
  uint64_t first_byte_pos_;
  uint64_t last_byte_pos_;
};

class CacheHeaderUtility : Logger::Loggable<Logger::Id::cache> {
public:
  // Get ranges defined by range-related headers in the provided request
  // headers.
  static std::vector<RawByteRange> getRanges(const Http::HeaderMap& request_headers);
};

} // namespace Cache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
