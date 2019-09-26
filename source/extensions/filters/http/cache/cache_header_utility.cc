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
const absl::string_view bytes_{"bytes"};
}

std::vector<RawByteRange> CacheHeaderUtility::getRanges(const Http::HeaderMap& request_headers) {
  if (request_headers.Method() == nullptr ||
      request_headers.Method()->value().getStringView() != Http::Headers::get().MethodValues.Get) {
    // Range headers are only valid on GET requests so don't bother parsing
    // range if we're not going to use it.
    return {};
  }

  // Multiple instances of range/range-unit headers are considered invalid.
  // https://tools.ietf.org/html/rfc7230#section-3.2.2
  // TODO: should we log when encountering multiple headers?
  std::vector<absl::string_view> range_unit_headers;
  Http::HeaderUtility::getAllOfHeader(request_headers, Http::Headers::get().RangeUnit.get(),
                                      range_unit_headers);
  absl::string_view range_unit;
  if (range_unit_headers.size() == 1) {
    range_unit = range_unit_headers.front();
  } else if (range_unit_headers.empty()) {
    range_unit = bytes_;
  } else {
    return {};
  }

  std::vector<absl::string_view> range_headers;
  Http::HeaderUtility::getAllOfHeader(request_headers, Http::Headers::get().Range.get(),
                                      range_headers);
  absl::string_view range;
  if (range_headers.size() == 1) {
    range = range_headers.front();
  } else {
    return {};
  }

  return parseRangeHeaderValue(range_unit, range);
}

std::vector<RawByteRange> CacheHeaderUtility::parseRangeHeaderValue(absl::string_view range_unit,
                                                               absl::string_view range) {
  if (range_unit.empty() || !absl::ConsumePrefix(&range, range_unit) ||
      !absl::ConsumePrefix(&range, "=")) {
    return {};
  }

  std::vector<RawByteRange> ranges;
  while (!range.empty()) {
    uint64_t first, last;
    if (!StringUtil::consumeLeadingDigits(&range, &first)) {
      if (!absl::ConsumePrefix(&range, "-")) {
        ranges.clear();
        break;
      }
      first = UINT64_MAX;
    }

    if (!absl::ConsumePrefix(&range, "-")) {
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
