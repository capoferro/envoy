#include "extensions/filters/http/cache/cache_header_utility.h"

#include <vector>

#include "common/common/utility.h"
#include "common/http/header_map_impl.h"
#include "common/http/header_utility.h"
#include "common/http/headers.h"

#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Cache {

namespace {
const std::string bytes_{"bytes"};
}

std::vector<RawByteRange> CacheHeaderUtility::getRanges(const Http::HeaderMap& request_headers) {
  // Range headers are only valid on GET requests so make sure we don't get here
  // with another type of request.
  ASSERT(request_headers.Method() != nullptr && request_headers.Method()->value() == Http::Headers::get().MethodValues.Get);

  // Multiple instances of range headers are considered invalid.
  // https://tools.ietf.org/html/rfc7230#section-3.2.2
  std::vector<absl::string_view> range_headers;
  Http::HeaderUtility::getAllOfHeader(request_headers, Http::Headers::get().Range.get(),
                                      range_headers);
  absl::string_view range;
  if (range_headers.size() == 1) {
    range = range_headers.front();
  } else {
    return {};
  }

  // Prevent DoS attacks with excessively long range strings.
  if (range.length() > 100) {
    return {};
  }
  if (!absl::ConsumePrefix(&range, bytes_) ||
      !absl::ConsumePrefix(&range, "=")) {
    return {};
  }

  std::vector<RawByteRange> ranges;
  while (!range.empty()) {
    uint64_t first, last;
    if (absl::ConsumePrefix(&range, "-")) {
      first = UINT64_MAX;
    } else if (!StringUtil::consumeLeadingDigits(&range, &first)) {
      ranges.clear();
      break;
    }

    if (absl::ConsumePrefix(&range, "-")) {
      if (first == UINT64_MAX) {
        ranges.clear();
        break;
      }
    } else {
      if (!StringUtil::consumeLeadingDigits(&range, &first)) {
        ranges.clear();
        break;
      }
    }

    if (!StringUtil::consumeLeadingDigits(&range, &last)) {
      if (!range.empty()) {
        ranges.clear();
        break;
      }
      last = first;
      first = UINT64_MAX;
    }

    ranges.push_back(RawByteRange(first, last));

    if (!absl::ConsumePrefix(&range, ",") && !range.empty()) {
      ranges.clear();
      break;
    }
  }

  return ranges;
}

} // namespace Cache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
