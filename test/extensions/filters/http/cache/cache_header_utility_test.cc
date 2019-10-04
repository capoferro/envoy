#include <string>
#include <tuple>
#include <vector>

#include "extensions/filters/http/cache/cache_header_utility.h"
#include "extensions/filters/http/cache/http_cache.h"

#include "test/test_common/utility.h"

#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Cache {

namespace {
Http::TestHeaderMapImpl makeTestHeaderMap(std::string rangeValue) {
  return Http::TestHeaderMapImpl({{":method", "GET"}, {"range", rangeValue}});
}
}

TEST(CacheHeaderUtilityTest, getRanges) {
  Http::TestHeaderMapImpl headers{{":method", "GET"}, {"range", "bytes=0-4"}};
  auto result_vector = CacheHeaderUtility::getRanges(headers);
  ASSERT_EQ(1, result_vector.size());
  auto result = result_vector.front();
  ASSERT_EQ(0, result.firstBytePos());
  ASSERT_EQ(4, result.lastBytePos());
}

class ParseInvalidRangeHeaderTest :
    public testing::Test, public testing::WithParamInterface<std::string> {
protected:
  Http::TestHeaderMapImpl range() { return makeTestHeaderMap(GetParam()); }
};

// clang-format off
INSTANTIATE_TEST_SUITE_P(
    Default, ParseInvalidRangeHeaderTest,
    testing::Values("1-2",
                    "12",
                    "a",
                    "a1",
                    "bytes1-2",
                    "bytes=12",
                    "bytes=1-2-3",
                    "bytes=1-2-",
                    "bytes=1--3",
                    "bytes=--2",
                    "bytes=2--",
                    "bytes=-2-",
                    "bytes=-1-2",
                    "bytes=a-2",
                    "bytes=2-a",
                    "bytes=-a",
                    "bytes=a-",
                    "bytes=a1-2",
                    "bytes=1-a2",
                    "bytes=1a-2",
                    "bytes=1-2a",
                    "bytes=1-2,3-a",
                    "bytes=1-a,3-4",
                    "bytes=1-2,3a-4",
                    "bytes=1-2,3-4a",
                    "bytes=1-2,3-4-5",
                    "bytes=1-2,3-4,a",
                    // excessively long string
                    "bytes=1000-1000,1001-1001,1002-1002,1003-1003,1004-1004,1005-1005,1006-1006,1007-1007,1008-1008,1000-",
                    // UINT64_MAX-UINT64_MAX+1
                    "bytes=18446744073709551615-18446744073709551616",
                    // UINT64_MAX+1-UINT64_MAX+2
                    "bytes=18446744073709551616-18446744073709551617"));
// clang-format on

TEST_P(ParseInvalidRangeHeaderTest, InvalidRangeReturnsEmpty) {
  auto result_vector = CacheHeaderUtility::getRanges(range());
  ASSERT_EQ(0, result_vector.size());
}

TEST(CacheHeaderUtilityTest, parseRangeHeaderValue) {
  auto result_vector =
      CacheHeaderUtility::getRanges(makeTestHeaderMap("bytes=500-999"));
  ASSERT_EQ(1, result_vector.size());
  auto result = result_vector.front();
  ASSERT_EQ(500, result.firstBytePos());
  ASSERT_EQ(999, result.lastBytePos());
}

TEST(CacheHeaderUtilityTest, getRangesSuffix) {
  auto result_vector =
      CacheHeaderUtility::getRanges(makeTestHeaderMap("bytes=-500"));
  ASSERT_EQ(1, result_vector.size());
  auto result = result_vector.front();
  ASSERT_EQ(500, result.suffixLength());
}

TEST(CacheHeaderUtilityTest, getRangesSuffixAlt) {
  auto result_vector =
      CacheHeaderUtility::getRanges(makeTestHeaderMap("bytes=500-"));
  ASSERT_EQ(1, result_vector.size());
  auto result = result_vector.front();
  ASSERT_EQ(500, result.suffixLength());
}

TEST(CacheHeaderUtilityTest, getRangesMultipleRanges) {
  auto result_vector =
      CacheHeaderUtility::getRanges(makeTestHeaderMap("bytes=10-20,30-40,50-50,-1"));
  ASSERT_EQ(4, result_vector.size());

  ASSERT_EQ(10, result_vector[0].firstBytePos());
  ASSERT_EQ(20, result_vector[0].lastBytePos());

  ASSERT_EQ(30, result_vector[1].firstBytePos());
  ASSERT_EQ(40, result_vector[1].lastBytePos());

  ASSERT_EQ(50, result_vector[2].firstBytePos());
  ASSERT_EQ(50, result_vector[2].lastBytePos());

  ASSERT_EQ(1, result_vector[3].suffixLength());
}

TEST(CacheHeaderUtilityTest, parseLongRangeHeaderValue) {
  auto result_vector = CacheHeaderUtility::getRanges(makeTestHeaderMap("bytes=1000-1000,1001-1001,1002-1002,1003-1003,1004-1004,1005-1005,1006-1006,1007-1007,1008-1008,100-"));
  ASSERT_EQ(10, result_vector.size());
}

TEST(CacheHeaderUtilityTest, parseUint64MaxBytes) {
  // UINT64_MAX-1 - UINT64_MAX
  // Note: UINT64_MAX is a sentry value for suffixes in the first value, so we
  // do not support UINT64_MAX as a first bytes value.
  auto result_vector = CacheHeaderUtility::getRanges(makeTestHeaderMap("bytes=18446744073709551614-18446744073709551615"));
  ASSERT_EQ(1, result_vector.size());
  ASSERT_EQ(18446744073709551614UL, result_vector[0].firstBytePos());
  ASSERT_EQ(18446744073709551615UL, result_vector[0].lastBytePos());
}

} // namespace Cache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
