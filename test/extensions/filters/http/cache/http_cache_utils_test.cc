#include <vector>

#include "common/http/header_map_impl.h"

#include "extensions/filters/http/cache/http_cache_utils.h"

#include "test/test_common/utility.h"

#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Cache {
namespace Internal {
namespace {

class HttpTimeTest : public testing::TestWithParam<const char*> {
protected:
  Http::TestHeaderMapImpl response_headers_{{"date", GetParam()}};
};

const char* const ok_times[] = {
    "Sun, 06 Nov 1994 08:49:37 GMT",  // IMF-fixdate
    "Sunday, 06-Nov-94 08:49:37 GMT", // obsolete RFC 850 format
    "Sun Nov  6 08:49:37 1994"        // ANSI C's asctime() format
};

INSTANTIATE_TEST_SUITE_P(Ok, HttpTimeTest, testing::ValuesIn(ok_times));

TEST_P(HttpTimeTest, Ok) {
  const std::time_t time = SystemTime::clock::to_time_t(httpTime(response_headers_.Date()));
  EXPECT_STREQ(ctime(&time), "Sun Nov  6 08:49:37 1994\n");
}

TEST(HttpTime, Null) { EXPECT_EQ(httpTime(nullptr), SystemTime()); }

TEST(EffectiveMaxAge, NegativeMaxAge) {
  EXPECT_EQ(SystemTime::duration::zero(), effectiveMaxAge("public, max-age=-1"));
}

class EffectiveMaxAgeTest : public testing::TestWithParam<std::tuple<std::string, SystemTime::duration>> {
 protected:
  std::string date_header_{std::get<0>(GetParam())};
  SystemTime::duration expected_duration() { return std::get<1>(GetParam()); };
};

const std::tuple<std::string, SystemTime::duration> effective_max_age_strings[] = {
  {"public, max-age=3600", std::chrono::seconds(3600)},
  {"public, max-age=3600,", std::chrono::seconds(3600)}, // todo: valid?
  {"public, max-age=-1", SystemTime::duration::zero()},
  {"public, max-age=3600z", SystemTime::duration::zero()},
  {"public, max-age=", SystemTime::duration::zero()},
  {"public, max-age=", SystemTime::duration::zero()},
  // INT64_MAX+1
  {"public, max-age=9223372036854775808", SystemTime::duration::max()},
  // INT64_MAX+1 + unexpected character
  {"public, max-age=9223372036854775808z", SystemTime::duration::zero()},
  // UINT64_MAX+1
  {"public, max-age=18446744073709551616", SystemTime::duration::max()},
  {"public, max-age=18446744073709551616,", SystemTime::duration::max()}, // todo: valid?
  // UINT64_MAX+1 + unexpected character
  {"public, max-age=18446744073709551616z", SystemTime::duration::zero()},
  {"public", SystemTime::duration::zero()}
};

INSTANTIATE_TEST_SUITE_P(Ok, EffectiveMaxAgeTest, testing::ValuesIn(effective_max_age_strings));

TEST_P(EffectiveMaxAgeTest, All) {
  EXPECT_EQ(expected_duration(), effectiveMaxAge(date_header_));
}

} // namespace
} // namespace Internal
} // namespace Cache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
