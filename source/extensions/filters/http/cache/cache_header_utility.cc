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

std::vector<RawByteRange> CacheHeaderUtility::getRanges(const Http::HeaderMap& request_headers) {
  // Range headers are only valid on GET requests so make sure we don't get here
  // with another type of request.
  ASSERT(request_headers.Method() != nullptr);
  ASSERT(request_headers.Method()->value() == Http::Headers::get().MethodValues.Get);

  // Multiple instances of range headers are invalid.
  // https://tools.ietf.org/html/rfc7230#section-3.2.2
  std::vector<absl::string_view> range_headers;
  Http::HeaderUtility::getAllOfHeader(request_headers, Http::Headers::get().Range.get(),
                                      range_headers);
  absl::string_view range;
  if (range_headers.size() == 1) {
    range = range_headers.front();
  } else {
    ENVOY_LOG(debug, "Multiple range headers provided in request. Ignoring.");
    return {};
  }

  // Prevent DoS attacks with excessively long range strings.
  if (range.length() > 100) {
    ENVOY_LOG(debug, "Excessively long range header. Ignoring.");
    return {};
  }
  if (!absl::ConsumePrefix(&range, "bytes=")) {
    ENVOY_LOG(debug, "Invalid range header. range-unit not correctly specified.");
    return {};
  }

  std::vector<RawByteRange> ranges;
  while (!range.empty()) {
    absl::optional<uint64_t> first = StringUtil::readAndRemoveLeadingDigits(range);
    if (!first) {
      first = UINT64_MAX;
    }
    if (!absl::ConsumePrefix(&range, "-")) {
      ranges.clear();
      break;
    }
    absl::optional<uint64_t> last = StringUtil::readAndRemoveLeadingDigits(range);
    if (!last) {
      last = first;
      first = UINT64_MAX;
    }
    if (first != UINT64_MAX && last < first) {
      ranges.clear();
      break;
    }
    if (!range.empty() && !absl::ConsumePrefix(&range, ",")) {
      ENVOY_LOG(debug, "Unexpected characters at the end of range header.");
      ranges.clear();
      break;
    }

    ranges.push_back(RawByteRange(first.value(), last.value()));
  }

  return ranges;
}

} // namespace Cache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
