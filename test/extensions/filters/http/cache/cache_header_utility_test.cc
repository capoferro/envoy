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

class CacheHeaderUtilityTest : public testing::Test {};

TEST_F(CacheHeaderUtilityTest, getRanges) {
  Http::TestHeaderMapImpl headers{{":method", "GET"}, {"range", "bytes=0-4"}};
  auto result_vector = CacheHeaderUtility::getRanges(headers);
  ASSERT_EQ(1, result_vector.size());
  auto result = result_vector.front();
  ASSERT_EQ(0, result.firstBytePos());
  ASSERT_EQ(4, result.lastBytePos());
}

TEST_F(CacheHeaderUtilityTest, getRangesWithRangeUnit) {
  Http::TestHeaderMapImpl headers{
      {":method", "GET"}, {"range", "other=0-4"}, {"range-unit", "other"}};
  auto result_vector = CacheHeaderUtility::getRanges(headers);
  ASSERT_EQ(1, result_vector.size());
  auto result = result_vector.front();
  ASSERT_EQ(0, result.firstBytePos());
  ASSERT_EQ(4, result.lastBytePos());
}

class InvalidRangeHeaderGetRangesTest
    : public CacheHeaderUtilityTest,
      public testing::WithParamInterface<Http::TestHeaderMapImpl> {
protected:
  Http::TestHeaderMapImpl headers() { return GetParam(); }
};

// clang-format off
INSTANTIATE_TEST_SUITE_P(Default, InvalidRangeHeaderGetRangesTest, testing::Values(
    Http::TestHeaderMapImpl({{":method", "GET"}, {"range", ""}}),
    Http::TestHeaderMapImpl({{":method", "GET"}, {"range", "bytes=1-2"}, {"range-unit", "other"}}),
    Http::TestHeaderMapImpl({{":method", "GET"}, {"range", "bytes=1-2"}, {"range-unit", ""}}),
    Http::TestHeaderMapImpl({{":method", "GET"}, {"range", "=1-2"}}),
    Http::TestHeaderMapImpl({{":method", "GET"}, {"range", "=1-2"}, {"range-unit", ""}}),
    Http::TestHeaderMapImpl({{":method", "GET"}, {"range", "bytes=1-2"}, {"range", "bytes=1-2"}}),
    Http::TestHeaderMapImpl({{":method", "GET"}, {"range", "bytes=1-2"}, {"range", "bytes=3-4"}}),
    Http::TestHeaderMapImpl({{":method", "GET"}, {"range", "other=1-2"}, {"range-unit", "other"}, {"range-unit", "other"}}),
    Http::TestHeaderMapImpl({{":method", "GET"}, {"range-unit", "other"}}),
    Http::TestHeaderMapImpl({{":method", "POST"}, {"range", "bytes=1-2"}}),
    Http::TestHeaderMapImpl({{"range", "bytes=1-2"}})));
// clang-format on

TEST_P(InvalidRangeHeaderGetRangesTest, getRangesNoRange) {
  auto h = headers();
  auto result_vector = CacheHeaderUtility::getRanges(h);
  ASSERT_EQ(0, result_vector.size());
}

class ParseInvalidRangeHeaderTest
    : public CacheHeaderUtilityTest,
      public testing::WithParamInterface<std::tuple<std::string, std::string>> {
protected:
  absl::string_view rangeUnit() { return absl::string_view(std::get<0>(GetParam())); }
  absl::string_view range() { return absl::string_view(std::get<1>(GetParam())); }
};

// clang-format off
INSTANTIATE_TEST_SUITE_P(
    Default, ParseInvalidRangeHeaderTest,
    testing::Values(std::make_tuple("bytes", "1-2"),
                    std::make_tuple("bytes", "12"),
                    std::make_tuple("bytes", "a"),
                    std::make_tuple("bytes", "a1"),
                    std::make_tuple("bytes", "bytes1-2"),
                    std::make_tuple("bytes", "bytes=12"),
                    std::make_tuple("bytes", "bytes=1-2-3"),
                    std::make_tuple("bytes", "bytes=1-2-"),
                    std::make_tuple("bytes", "bytes=1--3"),
                    std::make_tuple("bytes", "bytes=--2"),
                    std::make_tuple("bytes", "bytes=2--"),
                    std::make_tuple("bytes", "bytes=-2-"),
                    std::make_tuple("bytes", "bytes=-1-2"),
                    std::make_tuple("bytes", "bytes=a-2"),
                    std::make_tuple("bytes", "bytes=2-a"),
                    std::make_tuple("bytes", "bytes=-a"),
                    std::make_tuple("bytes", "bytes=a-"),
                    std::make_tuple("bytes", "bytes=a1-2"),
                    std::make_tuple("bytes", "bytes=1-a2"),
                    std::make_tuple("bytes", "bytes=1a-2"),
                    std::make_tuple("bytes", "bytes=1-2a"),
                    std::make_tuple("bytes", "bytes=1-2,3-a"),
                    std::make_tuple("bytes", "bytes=1-a,3-4"),
                    std::make_tuple("bytes", "bytes=1-2,3a-4"),
                    std::make_tuple("bytes", "bytes=1-2,3-4a"),
                    std::make_tuple("bytes", "bytes=1-2,3-4-5"),
                    std::make_tuple("bytes", "bytes=1-2,3-4,a"),
                    std::make_tuple("bytes", "other=1-2"),
                    std::make_tuple("",      "bytes=1-2"),
                    std::make_tuple("other", "bytes=1-2")));
// clang-format on

TEST_P(ParseInvalidRangeHeaderTest, InvalidRangeReturnsEmpty) {
  auto result_vector = CacheHeaderUtility::parseRangeHeaderValue(rangeUnit(), range());
  ASSERT_EQ(0, result_vector.size());
}

TEST_F(CacheHeaderUtilityTest, parseRangeHeaderValue) {
  auto result_vector =
      CacheHeaderUtility::parseRangeHeaderValue("bytes", absl::string_view("bytes=500-999"));
  ASSERT_EQ(1, result_vector.size());
  auto result = result_vector.front();
  ASSERT_EQ(500, result.firstBytePos());
  ASSERT_EQ(999, result.lastBytePos());
}

TEST_F(CacheHeaderUtilityTest, parseRangeHeaderValueSuffix) {
  auto result_vector =
      CacheHeaderUtility::parseRangeHeaderValue("bytes", absl::string_view("bytes=-500"));
  ASSERT_EQ(1, result_vector.size());
  auto result = result_vector.front();
  ASSERT_EQ(500, result.suffixLength());
}

TEST_F(CacheHeaderUtilityTest, parseRangeHeaderValueSuffixAlt) {
  auto result_vector =
      CacheHeaderUtility::parseRangeHeaderValue("bytes", absl::string_view("bytes=500-"));
  ASSERT_EQ(1, result_vector.size());
  auto result = result_vector.front();
  ASSERT_EQ(500, result.suffixLength());
}

TEST_F(CacheHeaderUtilityTest, parseRangeHeaderValueMultipleRanges) {
  auto result_vector =
      CacheHeaderUtility::parseRangeHeaderValue("bytes", "bytes=10-20,30-40,50-50,-1");
  ASSERT_EQ(4, result_vector.size());

  ASSERT_EQ(10, result_vector[0].firstBytePos());
  ASSERT_EQ(20, result_vector[0].lastBytePos());

  ASSERT_EQ(30, result_vector[1].firstBytePos());
  ASSERT_EQ(40, result_vector[1].lastBytePos());

  ASSERT_EQ(50, result_vector[2].firstBytePos());
  ASSERT_EQ(50, result_vector[2].lastBytePos());

  ASSERT_EQ(1, result_vector[3].suffixLength());
}

} // namespace Cache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
