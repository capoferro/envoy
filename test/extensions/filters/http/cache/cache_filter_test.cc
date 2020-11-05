#include "envoy/event/dispatcher.h"

#include "common/http/headers.h"

#include "extensions/filters/http/cache/cache_filter.h"
#include "extensions/filters/http/cache/simple_http_cache/simple_http_cache.h"

#include "test/extensions/filters/http/cache/common.h"
#include "test/mocks/server/factory_context.h"
#include "test/test_common/simulated_time_system.h"
#include "test/test_common/utility.h"

#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Cache {
namespace {

class CacheFilterTest : public ::testing::Test {
protected:
  // The filter has to be created as a shared_ptr to enable shared_from_this() which is used in the
  // cache callbacks.
  CacheFilterSharedPtr makeFilter(HttpCache& cache) {
    auto filter = std::make_shared<CacheFilter>(config_, /*stats_prefix=*/"", context_.scope(),
                                                context_.timeSource(), cache);
    filter->setDecoderFilterCallbacks(decoder_callbacks_);
    filter->setEncoderFilterCallbacks(encoder_callbacks_);
    return filter;
  }

  void SetUp() override {
    ON_CALL(decoder_callbacks_, dispatcher()).WillByDefault(::testing::ReturnRef(*dispatcher_));
    ON_CALL(encoder_callbacks_, encoderBufferLimit())
        .WillByDefault(::testing::Return(buffer_limit_));
    // Initialize the time source (otherwise it returns the real time).
    time_source_.setSystemTime(std::chrono::hours(1));
    // Use the initialized time source to set the response date and last modified headers.
    response_date_ = formatter_.now(time_source_);
    response_headers_.setDate(response_date_);
    response_last_modified_ = formatter_.now(time_source_);
  }

  void setBufferLimit(uint64_t buffer_limit) {
    buffer_limit_ = buffer_limit;
    ON_CALL(encoder_callbacks_, encoderBufferLimit())
        .WillByDefault(::testing::Return(buffer_limit_));
  }

  uint64_t getBufferLimit() const { return buffer_limit_; }

  void generateExpectedDataChunks(const std::string& body) {
    ASSERT(!body.empty());
    expected_data_chunks_.clear();

    size_t i = 0;
    while (i < body.size()) {
      expected_data_chunks_.push_back(body.substr(i, buffer_limit_));
      i += buffer_limit_;
    }
  }

  void testDecodeRequestMiss(CacheFilterSharedPtr filter) {
    // The filter should not encode any headers or data as no cached response exists.
    EXPECT_CALL(decoder_callbacks_, encodeHeaders_).Times(0);
    EXPECT_CALL(decoder_callbacks_, encodeData).Times(0);

    // The filter should stop decoding iteration when decodeHeaders is called as a cache lookup is
    // in progress.
    EXPECT_EQ(filter->decodeHeaders(request_headers_, true),
              Http::FilterHeadersStatus::StopAllIterationAndWatermark);

    // The filter should continue decoding when the cache lookup result (miss) is ready.
    EXPECT_CALL(decoder_callbacks_, continueDecoding).Times(1);

    // The cache lookup callback should be posted to the dispatcher.
    // Run events on the dispatcher so that the callback is invoked.
    dispatcher_->run(Event::Dispatcher::RunType::Block);

    ::testing::Mock::VerifyAndClearExpectations(&decoder_callbacks_);
  }

  void testDecodeRequestHitNoBody(CacheFilterSharedPtr filter) {
    // The filter should encode cached headers.
    EXPECT_CALL(decoder_callbacks_,
                encodeHeaders_(testing::AllOf(IsSupersetOfHeaders(response_headers_),
                                              HeaderHasValueRef(Http::Headers::get().Age, age)),
                               true));

    // The filter should not encode any data as the response has no body.
    EXPECT_CALL(decoder_callbacks_, encodeData).Times(0);

    // The filter should stop decoding iteration when decodeHeaders is called as a cache lookup is
    // in progress.
    EXPECT_EQ(filter->decodeHeaders(request_headers_, true),
              Http::FilterHeadersStatus::StopAllIterationAndWatermark);

    // The filter should not continue decoding when the cache lookup result is ready, as the
    // expected result is a hit.
    EXPECT_CALL(decoder_callbacks_, continueDecoding).Times(0);

    // The cache lookup callback should be posted to the dispatcher.
    // Run events on the dispatcher so that the callback is invoked.
    dispatcher_->run(Event::Dispatcher::RunType::Block);

    ::testing::Mock::VerifyAndClearExpectations(&decoder_callbacks_);
  }

  // This function assumes that there is a body created using std::string(body_size, 'a').
  void testDecodeRequestHitWithBody(CacheFilterSharedPtr filter, uint64_t body_size) {
    ASSERT(body_size > 0);
    // The filter should encode cached headers.
    EXPECT_CALL(decoder_callbacks_,
                encodeHeaders_(testing::AllOf(IsSupersetOfHeaders(response_headers_),
                                              HeaderHasValueRef(Http::Headers::get().Age, age)),
                               false));

    // The filter should encode data in chunks sized according to the buffer limit.
    const int chunks_count = (body_size + buffer_limit_ - 1) / buffer_limit_;
    // The size of all chunks except the last one is equal to the buffer_limit_.
    EXPECT_CALL(decoder_callbacks_,
                encodeData(testing::Property(&Buffer::Instance::toString,
                                             testing::Eq(std::string(getBufferLimit(), 'a'))),
                           false))
        .Times(chunks_count - 1);

    const uint64_t last_chunk_size =
        body_size % buffer_limit_ == 0 ? buffer_limit_ : body_size % buffer_limit_;
    EXPECT_CALL(decoder_callbacks_,
                encodeData(testing::Property(&Buffer::Instance::toString,
                                             testing::Eq(std::string(last_chunk_size, 'a'))),
                           true));

    // The filter should stop decoding iteration when decodeHeaders is called as a cache lookup is
    // in progress.
    EXPECT_EQ(filter->decodeHeaders(request_headers_, true),
              Http::FilterHeadersStatus::StopAllIterationAndWatermark);

    // The filter should not continue decoding when the cache lookup result is ready, as the
    // expected result is a hit.
    EXPECT_CALL(decoder_callbacks_, continueDecoding).Times(0);

    // The cache lookup callback should be posted to the dispatcher.
    // Run events on the dispatcher so that the callback is invoked.
    // The posted lookup callback will cause another callback to be posted (when getBody() is
    // called) which should also be invoked.
    dispatcher_->run(Event::Dispatcher::RunType::Block);

    ::testing::Mock::VerifyAndClearExpectations(&decoder_callbacks_);
  }

  // This function tests successful validation and verifies that |filter| injects body data in
  // correct chunks.
  void testSuccessfulValidation(CacheFilterSharedPtr filter, const std::string& body) {
    generateExpectedDataChunks(body);

    // Make request require validation.
    request_headers_.setReferenceKey(Http::CustomHeaders::get().CacheControl,
                                     Http::CustomHeaders::get().CacheControlValues.NoCache);

    // Decoding the request should find a cached response that requires validation.
    // As far as decoding the request is concerned, this is the same as a cache miss with the
    // exception of injecting validation precondition headers.
    testDecodeRequestMiss(filter);

    // Make sure validation conditional headers are added.
    const Http::TestRequestHeaderMapImpl injected_headers = {
        {"if-none-match", etag_}, {"if-modified-since", response_last_modified_}};
    EXPECT_THAT(request_headers_, IsSupersetOfHeaders(injected_headers));

    // Encode 304 response.
    // Advance time to make sure the cached date is updated with the 304 date.
    time_source_.advanceTimeWait(std::chrono::seconds(10));
    const std::string not_modified_date = formatter_.now(time_source_);
    Http::TestResponseHeaderMapImpl not_modified_response_headers = {{":status", "304"},
                                                                     {"date", not_modified_date}};

    // The filter should continue headers encoding without ending the stream as data will be
    // injected.
    EXPECT_EQ(filter->encodeHeaders(not_modified_response_headers, true),
              Http::FilterHeadersStatus::ContinueAndDontEndStream);

    // Check for the cached response headers with updated date.
    Http::TestResponseHeaderMapImpl updated_response_headers = response_headers_;
    updated_response_headers.setDate(not_modified_date);
    EXPECT_THAT(not_modified_response_headers, IsSupersetOfHeaders(updated_response_headers));

    // The filter should inject data in chunks sized according to the buffer limit.
    // Verify that each data chunk injected matches the expectation.
    for (size_t i = 0u; i < expected_data_chunks_.size(); i++) {
      EXPECT_CALL(encoder_callbacks_, injectEncodedDataToFilterChain(
                                          testing::Property(&Buffer::Instance::toString,
                                                            testing::Eq(expected_data_chunks_[i])),
                                          i == expected_data_chunks_.size() - 1u ? true : false))
          .Times(1);
    }

    // The cache getBody callback should be posted to the dispatcher.
    // Run events on the dispatcher so that the callback is invoked.
    dispatcher_->run(Event::Dispatcher::RunType::Block);

    ::testing::Mock::VerifyAndClearExpectations(&encoder_callbacks_);
  }

  void waitBeforeSecondRequest() { time_source_.advanceTimeWait(delay_); }

  SimpleHttpCache simple_cache_;
  envoy::extensions::filters::http::cache::v3alpha::CacheConfig config_;
  NiceMock<Server::Configuration::MockFactoryContext> context_;
  Event::SimulatedTimeSystem time_source_;
  DateFormatter formatter_{"%a, %d %b %Y %H:%M:%S GMT"};

  Http::TestRequestHeaderMapImpl request_headers_{
      {":path", "/"}, {":method", "GET"}, {"x-forwarded-proto", "https"}};
  Http::TestResponseHeaderMapImpl response_headers_{{":status", "200"},
                                                    {"cache-control", "public,max-age=3600"}};

  NiceMock<Http::MockStreamDecoderFilterCallbacks> decoder_callbacks_;
  NiceMock<Http::MockStreamEncoderFilterCallbacks> encoder_callbacks_;

  // Etag and last modified date header values, used for cache validation tests.
  std::string response_last_modified_, response_date_, etag_ = "abc123";

  Api::ApiPtr api_ = Api::createApiForTest();
  Event::DispatcherPtr dispatcher_ = api_->allocateDispatcher("test_thread");
  const Seconds delay_ = Seconds(10);
  const std::string age = std::to_string(delay_.count());

private:
  uint64_t buffer_limit_ = 1024;
  std::vector<std::string> expected_data_chunks_;
};

TEST_F(CacheFilterTest, UncacheableRequest) {
  request_headers_.setHost("UncacheableRequest");

  // POST requests are uncacheable
  request_headers_.setMethod(Http::Headers::get().MethodValues.Post);

  for (int request = 1; request <= 2; request++) {
    // Create filter for the request
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    // Decode request headers
    // The filter should not encode any headers or data as no cached response exists.
    EXPECT_CALL(decoder_callbacks_, encodeHeaders_).Times(0);
    EXPECT_CALL(decoder_callbacks_, encodeData).Times(0);

    // Uncacheable requests should bypass the cache filter-> No cache lookups should be initiated.
    EXPECT_EQ(filter->decodeHeaders(request_headers_, true), Http::FilterHeadersStatus::Continue);
    ::testing::Mock::VerifyAndClearExpectations(&decoder_callbacks_);

    // Encode response header
    EXPECT_EQ(filter->encodeHeaders(response_headers_, true), Http::FilterHeadersStatus::Continue);
    filter->onDestroy();
  }
}

TEST_F(CacheFilterTest, UncacheableResponse) {
  request_headers_.setHost("UncacheableResponse");

  // Responses with "Cache-Control: no-store" are uncacheable
  response_headers_.setReferenceKey(Http::CustomHeaders::get().CacheControl, "no-store");

  for (int request = 1; request <= 2; request++) {
    // Create filter for the request.
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    testDecodeRequestMiss(filter);

    // Encode response headers.
    EXPECT_EQ(filter->encodeHeaders(response_headers_, true), Http::FilterHeadersStatus::Continue);
    filter->onDestroy();
  }
}

TEST_F(CacheFilterTest, CacheMiss) {
  for (int request = 1; request <= 2; request++) {
    // Each iteration a request is sent to a different host, therefore the second one is a miss
    request_headers_.setHost("CacheMiss" + std::to_string(request));

    // Create filter for request 1
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    testDecodeRequestMiss(filter);

    // Encode response header
    EXPECT_EQ(filter->encodeHeaders(response_headers_, true), Http::FilterHeadersStatus::Continue);
    filter->onDestroy();
  }
}

TEST_F(CacheFilterTest, CacheHitNoBody) {
  request_headers_.setHost("CacheHitNoBody");

  {
    // Create filter for request 1.
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    testDecodeRequestMiss(filter);

    // Encode response headers.
    EXPECT_EQ(filter->encodeHeaders(response_headers_, true), Http::FilterHeadersStatus::Continue);
    filter->onDestroy();
  }
  waitBeforeSecondRequest();
  {
    // Create filter for request 2.
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    testDecodeRequestHitNoBody(filter);

    filter->onDestroy();
  }
}

TEST_F(CacheFilterTest, CacheHitWithBody) {
  request_headers_.setHost("CacheHitWithBody");
  const uint64_t body_size = 3;
  const std::string body = std::string(body_size, 'a');

  {
    // Create filter for request 1.
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    testDecodeRequestMiss(filter);

    // Encode response.
    Buffer::OwnedImpl buffer(body);
    response_headers_.setContentLength(body.size());
    EXPECT_EQ(filter->encodeHeaders(response_headers_, false), Http::FilterHeadersStatus::Continue);
    EXPECT_EQ(filter->encodeData(buffer, true), Http::FilterDataStatus::Continue);

    filter->onDestroy();
  }
  waitBeforeSecondRequest();
  {
    // Create filter for request 2
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    testDecodeRequestHitWithBody(filter, body_size);

    filter->onDestroy();
  }
}

TEST_F(CacheFilterTest, SuccessfulValidation) {
  request_headers_.setHost("SuccessfulValidation");
  const std::string body = "123";

  {
    // Create filter for request 1
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    testDecodeRequestMiss(filter);

    // Encode response
    // Add Etag & Last-Modified headers to the response for validation
    response_headers_.setReferenceKey(Http::CustomHeaders::get().Etag, etag_);
    response_headers_.setReferenceKey(Http::CustomHeaders::get().LastModified,
                                      response_last_modified_);

    Buffer::OwnedImpl buffer(body);
    response_headers_.setContentLength(body.size());
    EXPECT_EQ(filter->encodeHeaders(response_headers_, false), Http::FilterHeadersStatus::Continue);
    EXPECT_EQ(filter->encodeData(buffer, true), Http::FilterDataStatus::Continue);
    filter->onDestroy();
  }
  waitBeforeSecondRequest();
  {
    // Create filter for request 2
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    testSuccessfulValidation(filter, body);

    filter->onDestroy();
  }
}

TEST_F(CacheFilterTest, UnsuccessfulValidation) {
  request_headers_.setHost("UnsuccessfulValidation");
  const std::string body = std::string(3, 'a');

  {
    // Create filter for request 1
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    testDecodeRequestMiss(filter);

    // Encode response
    // Add Etag & Last-Modified headers to the response for validation.
    response_headers_.setReferenceKey(Http::CustomHeaders::get().Etag, etag_);
    response_headers_.setReferenceKey(Http::CustomHeaders::get().LastModified,
                                      response_last_modified_);

    Buffer::OwnedImpl buffer(body);
    response_headers_.setContentLength(body.size());
    EXPECT_EQ(filter->encodeHeaders(response_headers_, false), Http::FilterHeadersStatus::Continue);
    EXPECT_EQ(filter->encodeData(buffer, true), Http::FilterDataStatus::Continue);
    filter->onDestroy();
  }
  waitBeforeSecondRequest();
  {
    // Create filter for request 2.
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    // Make request require validation.
    request_headers_.setReferenceKey(Http::CustomHeaders::get().CacheControl,
                                     Http::CustomHeaders::get().CacheControlValues.NoCache);

    // Decoding the request should find a cached response that requires validation.
    // As far as decoding the request is concerned, this is the same as a cache miss with the
    // exception of injecting validation precondition headers.
    testDecodeRequestMiss(filter);

    // Make sure validation conditional headers are added.
    const Http::TestRequestHeaderMapImpl injected_headers = {
        {"if-none-match", etag_}, {"if-modified-since", response_last_modified_}};
    EXPECT_THAT(request_headers_, IsSupersetOfHeaders(injected_headers));

    // Encode new response.
    // Change the status code to make sure new headers are served, not the cached ones.
    response_headers_.setStatus(201);

    // The filter should not stop encoding iteration as this is a new response.
    EXPECT_EQ(filter->encodeHeaders(response_headers_, false), Http::FilterHeadersStatus::Continue);
    Buffer::OwnedImpl new_body;
    EXPECT_EQ(filter->encodeData(new_body, true), Http::FilterDataStatus::Continue);

    // The response headers should have the new status.
    EXPECT_THAT(response_headers_, HeaderHasValueRef(Http::Headers::get().Status, "201"));

    // The filter should not encode any data.
    EXPECT_CALL(encoder_callbacks_, addEncodedData).Times(0);

    // If a cache getBody callback is made, it should be posted to the dispatcher.
    // Run events on the dispatcher so that any available callbacks are invoked.
    dispatcher_->run(Event::Dispatcher::RunType::Block);

    ::testing::Mock::VerifyAndClearExpectations(&encoder_callbacks_);

    filter->onDestroy();
  }
}

TEST_F(CacheFilterTest, SingleSatisfiableRange) {
  request_headers_.setHost("SingleSatisfiableRange");
  const std::string body = "abc";

  {
    // Create filter for request 1.
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    testDecodeRequestMiss(filter);

    // Encode response.
    Buffer::OwnedImpl buffer(body);
    response_headers_.setContentLength(body.size());
    EXPECT_EQ(filter->encodeHeaders(response_headers_, false), Http::FilterHeadersStatus::Continue);
    EXPECT_EQ(filter->encodeData(buffer, true), Http::FilterDataStatus::Continue);
    filter->onDestroy();
  }
  waitBeforeSecondRequest();
  {
    // Add range info to headers.
    request_headers_.addReference(Http::Headers::get().Range, "bytes=-2");

    response_headers_.setStatus(static_cast<uint64_t>(Http::Code::PartialContent));
    response_headers_.addReference(Http::Headers::get().ContentRange, "bytes 1-2/3");
    response_headers_.setContentLength(2);

    // Create filter for request 2
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    // Decode request 2 header
    EXPECT_CALL(decoder_callbacks_,
                encodeHeaders_(testing::AllOf(IsSupersetOfHeaders(response_headers_),
                                              HeaderHasValueRef(Http::Headers::get().Age, age)),
                               false));

    EXPECT_CALL(
        decoder_callbacks_,
        encodeData(testing::Property(&Buffer::Instance::toString, testing::Eq("bc")), true));
    EXPECT_EQ(filter->decodeHeaders(request_headers_, true),
              Http::FilterHeadersStatus::StopAllIterationAndWatermark);

    // The cache lookup callback should be posted to the dispatcher.
    // Run events on the dispatcher so that the callback is invoked.
    // The posted lookup callback will cause another callback to be posted (when getBody() is
    // called) which should also be invoked.
    dispatcher_->run(Event::Dispatcher::RunType::Block);

    ::testing::Mock::VerifyAndClearExpectations(&decoder_callbacks_);
    filter->onDestroy();
  }
}

TEST_F(CacheFilterTest, MultipleSatisfiableRanges) {
  request_headers_.setHost("MultipleSatisfiableRanges");
  const std::string body = "abc";

  {
    // Create filter for request 1
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    testDecodeRequestMiss(filter);

    // Encode response header
    Buffer::OwnedImpl buffer(body);
    response_headers_.setContentLength(body.size());
    EXPECT_EQ(filter->encodeHeaders(response_headers_, false), Http::FilterHeadersStatus::Continue);
    EXPECT_EQ(filter->encodeData(buffer, true), Http::FilterDataStatus::Continue);
    filter->onDestroy();
  }
  waitBeforeSecondRequest();
  {
    // Add range info to headers
    // multi-part responses are not supported, 200 expected
    request_headers_.addReference(Http::Headers::get().Range, "bytes=0-1,-2");

    // Create filter for request 2
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    // Decode request 2 header
    EXPECT_CALL(decoder_callbacks_,
                encodeHeaders_(testing::AllOf(IsSupersetOfHeaders(response_headers_),
                                              HeaderHasValueRef(Http::Headers::get().Age, age)),
                               false));

    EXPECT_CALL(
        decoder_callbacks_,
        encodeData(testing::Property(&Buffer::Instance::toString, testing::Eq(body)), true));
    EXPECT_EQ(filter->decodeHeaders(request_headers_, true),
              Http::FilterHeadersStatus::StopAllIterationAndWatermark);

    // The cache lookup callback should be posted to the dispatcher.
    // Run events on the dispatcher so that the callback is invoked.
    // The posted lookup callback will cause another callback to be posted (when getBody() is
    // called) which should also be invoked.
    dispatcher_->run(Event::Dispatcher::RunType::Block);

    ::testing::Mock::VerifyAndClearExpectations(&decoder_callbacks_);
    filter->onDestroy();
  }
}

TEST_F(CacheFilterTest, NotSatisfiableRange) {
  request_headers_.setHost("NotSatisfiableRange");
  const std::string body = "abc";

  {
    // Create filter for request 1
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    testDecodeRequestMiss(filter);

    // Encode response header
    Buffer::OwnedImpl buffer(body);
    response_headers_.setContentLength(body.size());
    EXPECT_EQ(filter->encodeHeaders(response_headers_, false), Http::FilterHeadersStatus::Continue);
    EXPECT_EQ(filter->encodeData(buffer, true), Http::FilterDataStatus::Continue);
    filter->onDestroy();
  }
  waitBeforeSecondRequest();
  {
    // Add range info to headers
    request_headers_.addReference(Http::Headers::get().Range, "bytes=123-");

    response_headers_.setStatus(static_cast<uint64_t>(Http::Code::RangeNotSatisfiable));
    response_headers_.addReference(Http::Headers::get().ContentRange, "bytes */3");
    response_headers_.setContentLength(0);

    // Create filter for request 2
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    // Decode request 2 header
    EXPECT_CALL(decoder_callbacks_,
                encodeHeaders_(testing::AllOf(IsSupersetOfHeaders(response_headers_),
                                              HeaderHasValueRef(Http::Headers::get().Age, age)),
                               true));

    // 416 response should not have a body, so we don't expect a call to encodeData
    EXPECT_CALL(decoder_callbacks_,
                encodeData(testing::Property(&Buffer::Instance::toString, testing::Eq(body)), true))
        .Times(0);

    EXPECT_EQ(filter->decodeHeaders(request_headers_, true),
              Http::FilterHeadersStatus::StopAllIterationAndWatermark);

    // The cache lookup callback should be posted to the dispatcher.
    // Run events on the dispatcher so that the callback is invoked.
    // The posted lookup callback will cause another callback to be posted (when getBody() is
    // called) which should also be invoked.
    dispatcher_->run(Event::Dispatcher::RunType::Block);

    ::testing::Mock::VerifyAndClearExpectations(&decoder_callbacks_);
    filter->onDestroy();
  }
}

// Send two identical GET requests with bodies. The CacheFilter will just pass everything through.
TEST_F(CacheFilterTest, GetRequestWithBodyAndTrailers) {
  request_headers_.setHost("GetRequestWithBodyAndTrailers");
  const std::string body = std::string(3, 'a');
  Buffer::OwnedImpl request_buffer(body);
  Http::TestRequestTrailerMapImpl request_trailers;

  for (int i = 0; i < 2; ++i) {
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    EXPECT_EQ(filter->decodeHeaders(request_headers_, false), Http::FilterHeadersStatus::Continue);
    EXPECT_EQ(filter->decodeData(request_buffer, false), Http::FilterDataStatus::Continue);
    EXPECT_EQ(filter->decodeTrailers(request_trailers), Http::FilterTrailersStatus::Continue);

    EXPECT_EQ(filter->encodeHeaders(response_headers_, true), Http::FilterHeadersStatus::Continue);
    filter->onDestroy();
  }
}

// Checks the case where a cache lookup callback is posted to the dispatcher, then the CacheFilter
// was deleted (e.g. connection dropped with the client) before the posted callback was executed. In
// this case the CacheFilter should not be accessed after it was deleted, which is ensured by using
// a weak_ptr to the CacheFilter in the posted callback.
// This test may mistakenly pass (false positive) even if the the CacheFilter is accessed after
// being deleted, as filter_state_ may be accessed and read as "FilterState::Destroyed" which will
// result in a correct behavior. However, running the test with ASAN sanitizer enabled should
// reliably fail if the CacheFilter is accessed after being deleted.
TEST_F(CacheFilterTest, FilterDeletedBeforePostedCallbackExecuted) {
  request_headers_.setHost("FilterDeletedBeforePostedCallbackExecuted");
  {
    // Create filter for request 1.
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    testDecodeRequestMiss(filter);

    // Encode response headers.
    EXPECT_EQ(filter->encodeHeaders(response_headers_, true), Http::FilterHeadersStatus::Continue);
    filter->onDestroy();
  }
  {
    // Create filter for request 2.
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    // Call decode headers to start the cache lookup, which should immediately post the callback to
    // the dispatcher.
    EXPECT_EQ(filter->decodeHeaders(request_headers_, true),
              Http::FilterHeadersStatus::StopAllIterationAndWatermark);

    // Destroy the filter
    filter->onDestroy();
    filter.reset();

    // Make sure that onHeaders was not called by making sure no decoder callbacks were made.
    EXPECT_CALL(decoder_callbacks_, continueDecoding).Times(0);
    EXPECT_CALL(decoder_callbacks_, encodeHeaders_).Times(0);

    // Run events on the dispatcher so that the callback is invoked after the filter deletion.
    dispatcher_->run(Event::Dispatcher::RunType::Block);

    ::testing::Mock::VerifyAndClearExpectations(&decoder_callbacks_);
  }
}

// A new type alias for a different type of tests that use the exact same class.
// In these tests, realistically the data in request 1 should be encoded in several chunks too,
// however, the only purpose of request 1 is to put the response in the cache, so it shouldn't
// matter.
// Cases where the body size is less than the buffer_limit_ are not exercised as they are
// already tested in the above tests.
using CacheChunkSizeTest = CacheFilterTest;

// Test that a body with size exactly equal to the buffer limit will be encoded in 1 chunk.
TEST_F(CacheChunkSizeTest, EqualBufferLimit) {
  request_headers_.setHost("EqualBufferLimit");
  const std::string body = std::string(getBufferLimit(), 'a');

  {
    // Create filter for request 1.
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    testDecodeRequestMiss(filter);

    // Encode response.
    Buffer::OwnedImpl buffer(body);
    response_headers_.setContentLength(body.size());
    EXPECT_EQ(filter->encodeHeaders(response_headers_, false), Http::FilterHeadersStatus::Continue);
    EXPECT_EQ(filter->encodeData(buffer, true), Http::FilterDataStatus::Continue);

    filter->onDestroy();
  }
  waitBeforeSecondRequest();
  {
    // Create filter for request 2
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    // The body should be encoded in a single chunk.
    testDecodeRequestHitWithBody(filter, getBufferLimit());

    filter->onDestroy();
  }
}

// Test that a body with size greater than and divisible by buffer limit will be encoded as the
// correct number of chunks.
TEST_F(CacheChunkSizeTest, DivisibleByBufferLimit) {
  request_headers_.setHost("DivisibleByBufferLimit");
  const uint64_t body_size = getBufferLimit() * 3;
  const std::string body = std::string(body_size, 'a');

  {
    // Create filter for request 1.
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    testDecodeRequestMiss(filter);

    // Encode response.
    Buffer::OwnedImpl buffer(body);
    response_headers_.setContentLength(body.size());
    EXPECT_EQ(filter->encodeHeaders(response_headers_, false), Http::FilterHeadersStatus::Continue);
    EXPECT_EQ(filter->encodeData(buffer, true), Http::FilterDataStatus::Continue);

    filter->onDestroy();
  }
  waitBeforeSecondRequest();
  {
    // Create filter for request 2
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    // The body should be encoded in 3 chunks.
    testDecodeRequestHitWithBody(filter, body_size);

    filter->onDestroy();
  }
}

// Test that a body with size greater than but not divisible by buffer limit will be encoded as the
// correct number of chunks.
TEST_F(CacheChunkSizeTest, NotDivisbleByBufferLimit) {
  request_headers_.setHost("NotDivisbleByBufferLimit");
  const uint64_t body_size = getBufferLimit() * 4.5;
  const std::string body = std::string(body_size, 'a');

  {
    // Create filter for request 1.
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    testDecodeRequestMiss(filter);

    // Encode response.
    Buffer::OwnedImpl buffer(body);
    response_headers_.setContentLength(body.size());
    EXPECT_EQ(filter->encodeHeaders(response_headers_, false), Http::FilterHeadersStatus::Continue);
    EXPECT_EQ(filter->encodeData(buffer, true), Http::FilterDataStatus::Continue);

    filter->onDestroy();
  }
  waitBeforeSecondRequest();
  {
    // Create filter for request 2
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    // The body should be encoded in 5 chunks.
    testDecodeRequestHitWithBody(filter, body_size);

    filter->onDestroy();
  }
}

// Test that a body with size exactly equal to the buffer limit will be encoded in 1 chunk, in the
// case where validation takes place.
TEST_F(CacheChunkSizeTest, EqualBufferLimitWithValidation) {
  request_headers_.setHost("EqualBufferLimitWithValidation");
  const std::string body = std::string(getBufferLimit(), 'a');

  {
    // Create filter for request 1.
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    testDecodeRequestMiss(filter);

    // Encode response.
    // Add Etag & Last-Modified headers to the response for validation.
    response_headers_.setReferenceKey(Http::CustomHeaders::get().Etag, etag_);
    response_headers_.setReferenceKey(Http::CustomHeaders::get().LastModified,
                                      response_last_modified_);

    Buffer::OwnedImpl buffer(body);
    response_headers_.setContentLength(body.size());
    EXPECT_EQ(filter->encodeHeaders(response_headers_, false), Http::FilterHeadersStatus::Continue);
    EXPECT_EQ(filter->encodeData(buffer, true), Http::FilterDataStatus::Continue);

    filter->onDestroy();
  }
  waitBeforeSecondRequest();
  {
    // Create filter for request 2
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    testSuccessfulValidation(filter, body);

    filter->onDestroy();
  }
}

// Test that a body with size greater than and divisible by buffer limit will be encoded as the
// correct number of chunks, in the case where validation takes place.
TEST_F(CacheChunkSizeTest, DivisibleByBufferLimitWithValidation) {
  request_headers_.setHost("DivisibleByBufferLimitWithValidation");

  setBufferLimit(5);
  const std::string body = "1234567890abcde";

  {
    // Create filter for request 1.
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    testDecodeRequestMiss(filter);

    // Encode response.
    // Add Etag & Last-Modified headers to the response for validation.
    response_headers_.setReferenceKey(Http::CustomHeaders::get().Etag, etag_);
    response_headers_.setReferenceKey(Http::CustomHeaders::get().LastModified,
                                      response_last_modified_);

    Buffer::OwnedImpl buffer(body);
    response_headers_.setContentLength(body.size());
    EXPECT_EQ(filter->encodeHeaders(response_headers_, false), Http::FilterHeadersStatus::Continue);
    EXPECT_EQ(filter->encodeData(buffer, true), Http::FilterDataStatus::Continue);

    filter->onDestroy();
  }
  waitBeforeSecondRequest();
  {
    // Create filter for request 2
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    testSuccessfulValidation(filter, body);

    filter->onDestroy();
  }
}

// Test that a body with size greater than but not divisible by buffer limit will be encoded as the
// correct number of chunks, in the case where validation takes place.
TEST_F(CacheChunkSizeTest, NotDivisbleByBufferLimitWithValidation) {
  request_headers_.setHost("NotDivisbleByBufferLimitWithValidation");
  setBufferLimit(5);

  const std::string body = "1234567890abcdefg";

  {
    // Create filter for request 1.
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    testDecodeRequestMiss(filter);

    // Encode response.
    // Add Etag & Last-Modified headers to the response for validation.
    response_headers_.setReferenceKey(Http::CustomHeaders::get().Etag, etag_);
    response_headers_.setReferenceKey(Http::CustomHeaders::get().LastModified,
                                      response_last_modified_);

    Buffer::OwnedImpl buffer(body);
    response_headers_.setContentLength(body.size());
    EXPECT_EQ(filter->encodeHeaders(response_headers_, false), Http::FilterHeadersStatus::Continue);
    EXPECT_EQ(filter->encodeData(buffer, true), Http::FilterDataStatus::Continue);

    filter->onDestroy();
  }
  waitBeforeSecondRequest();
  {
    // Create filter for request 2
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    testSuccessfulValidation(filter, body);

    filter->onDestroy();
  }
}

// A new type alias for a different type of tests that use the exact same class.
using ValidationHeadersTest = CacheFilterTest;

TEST_F(ValidationHeadersTest, EtagAndLastModified) {
  request_headers_.setHost("EtagAndLastModified");

  // Make request 1 to insert the response into cache
  {
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);
    testDecodeRequestMiss(filter);

    // Add validation headers to the response
    response_headers_.setReferenceKey(Http::CustomHeaders::get().Etag, etag_);
    response_headers_.setReferenceKey(Http::CustomHeaders::get().LastModified,
                                      response_last_modified_);

    filter->encodeHeaders(response_headers_, true);
  }
  // Make request 2 to test for added conditional headers
  {
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    // Make sure the request requires validation
    request_headers_.setReferenceKey(Http::CustomHeaders::get().CacheControl,
                                     Http::CustomHeaders::get().CacheControlValues.NoCache);
    testDecodeRequestMiss(filter);

    // Make sure validation conditional headers are added
    const Http::TestRequestHeaderMapImpl injected_headers = {
        {"if-none-match", etag_}, {"if-modified-since", response_last_modified_}};
    EXPECT_THAT(request_headers_, IsSupersetOfHeaders(injected_headers));
  }
}

TEST_F(ValidationHeadersTest, EtagOnly) {
  request_headers_.setHost("EtagOnly");

  // Make request 1 to insert the response into cache
  {
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);
    testDecodeRequestMiss(filter);

    // Add validation headers to the response
    response_headers_.setReferenceKey(Http::CustomHeaders::get().Etag, etag_);

    filter->encodeHeaders(response_headers_, true);
  }
  // Make request 2 to test for added conditional headers
  {
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    // Make sure the request requires validation
    request_headers_.setReferenceKey(Http::CustomHeaders::get().CacheControl,
                                     Http::CustomHeaders::get().CacheControlValues.NoCache);
    testDecodeRequestMiss(filter);

    // Make sure validation conditional headers are added
    // If-Modified-Since falls back to date
    const Http::TestRequestHeaderMapImpl injected_headers = {{"if-none-match", etag_},
                                                             {"if-modified-since", response_date_}};
    EXPECT_THAT(request_headers_, IsSupersetOfHeaders(injected_headers));
  }
}

TEST_F(ValidationHeadersTest, LastModifiedOnly) {
  request_headers_.setHost("LastModifiedOnly");

  // Make request 1 to insert the response into cache
  {
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);
    testDecodeRequestMiss(filter);

    // Add validation headers to the response
    response_headers_.setReferenceKey(Http::CustomHeaders::get().LastModified,
                                      response_last_modified_);

    filter->encodeHeaders(response_headers_, true);
  }
  // Make request 2 to test for added conditional headers
  {
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    // Make sure the request requires validation
    request_headers_.setReferenceKey(Http::CustomHeaders::get().CacheControl,
                                     Http::CustomHeaders::get().CacheControlValues.NoCache);
    testDecodeRequestMiss(filter);

    // Make sure validation conditional headers are added
    const Http::TestRequestHeaderMapImpl injected_headers = {
        {"if-modified-since", response_last_modified_}};
    EXPECT_THAT(request_headers_, IsSupersetOfHeaders(injected_headers));
  }
}

TEST_F(ValidationHeadersTest, NoEtagOrLastModified) {
  request_headers_.setHost("NoEtagOrLastModified");

  // Make request 1 to insert the response into cache
  {
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);
    testDecodeRequestMiss(filter);
    filter->encodeHeaders(response_headers_, true);
  }
  // Make request 2 to test for added conditional headers
  {
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    // Make sure the request requires validation
    request_headers_.setReferenceKey(Http::CustomHeaders::get().CacheControl,
                                     Http::CustomHeaders::get().CacheControlValues.NoCache);
    testDecodeRequestMiss(filter);

    // Make sure validation conditional headers are added
    // If-Modified-Since falls back to date
    const Http::TestRequestHeaderMapImpl injected_headers = {{"if-modified-since", response_date_}};
    EXPECT_THAT(request_headers_, IsSupersetOfHeaders(injected_headers));
  }
}

TEST_F(ValidationHeadersTest, InvalidLastModified) {
  request_headers_.setHost("InvalidLastModified");

  // Make request 1 to insert the response into cache
  {
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);
    testDecodeRequestMiss(filter);

    // Add validation headers to the response
    response_headers_.setReferenceKey(Http::CustomHeaders::get().LastModified, "invalid-date");
    filter->encodeHeaders(response_headers_, true);
  }
  // Make request 2 to test for added conditional headers
  {
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    // Make sure the request requires validation
    request_headers_.setReferenceKey(Http::CustomHeaders::get().CacheControl,
                                     Http::CustomHeaders::get().CacheControlValues.NoCache);
    testDecodeRequestMiss(filter);

    // Make sure validation conditional headers are added
    // If-Modified-Since falls back to date
    const Http::TestRequestHeaderMapImpl injected_headers = {{"if-modified-since", response_date_}};
    EXPECT_THAT(request_headers_, IsSupersetOfHeaders(injected_headers));
  }
}

TEST_F(CacheChunkSizeTest, HandleDownstreamWatermarkCallbacks) {
  request_headers_.setHost("DownstreamPressureHandling");
  const int chunks_count = 3;
  const uint64_t body_size = getBufferLimit() * chunks_count;
  const std::string body = std::string(body_size, 'a');
  {
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);

    testDecodeRequestMiss(filter);

    // Add Etag & Last-Modified headers to the response for validation.
    response_headers_.setReferenceKey(Http::CustomHeaders::get().Etag, etag_);
    response_headers_.setReferenceKey(Http::CustomHeaders::get().LastModified,
                                      response_last_modified_);

    Buffer::OwnedImpl buffer(body);
    response_headers_.setContentLength(body.size());
    EXPECT_EQ(filter->encodeHeaders(response_headers_, false), Http::FilterHeadersStatus::Continue);
    EXPECT_EQ(filter->encodeData(buffer, true), Http::FilterDataStatus::Continue);

    filter->onDestroy();
  }
  waitBeforeSecondRequest();
  {
    CacheFilterSharedPtr filter = makeFilter(simple_cache_);
    // Set require validation.
    request_headers_.setReferenceKey(Http::CustomHeaders::get().CacheControl,
                                     Http::CustomHeaders::get().CacheControlValues.NoCache);

    // Cached response requiring validation is treated as a cache miss.
    testDecodeRequestMiss(filter);

    // Verify validation conditional headers are added.
    const Http::TestRequestHeaderMapImpl injected_headers = {
        {"if-none-match", etag_}, {"if-modified-since", response_last_modified_}};
    EXPECT_THAT(request_headers_, IsSupersetOfHeaders(injected_headers));

    // Advance time so that the cached date is updated.
    time_source_.advanceTimeWait(std::chrono::seconds(10));
    const std::string not_modified_date = formatter_.now(time_source_);
    Http::TestResponseHeaderMapImpl not_modified_response_headers = {{":status", "304"},
                                                                     {"date", not_modified_date}};

    // The filter should continue headers encoding without ending the stream as data will be
    // injected.
    EXPECT_EQ(filter->encodeHeaders(not_modified_response_headers, true),
              Http::FilterHeadersStatus::ContinueAndDontEndStream);

    // Verify the cached response headers with the updated date.
    Http::TestResponseHeaderMapImpl updated_response_headers = response_headers_;
    updated_response_headers.setDate(not_modified_date);
    EXPECT_THAT(not_modified_response_headers, IsSupersetOfHeaders(updated_response_headers));

    // Downstream backs up multiple times, increase watermarks.
    filter->onAboveWriteBufferHighWatermark();
    filter->onAboveWriteBufferHighWatermark();

    // The first cache lookup callback is already posted to the dispatcher before the watermark
    // increases. Run the event loop to invoke the callback. No additional callbacks will be
    // invoked due to watermark being greater than zero.
    EXPECT_CALL(encoder_callbacks_,
                injectEncodedDataToFilterChain(
                    testing::Property(&Buffer::Instance::toString,
                                      testing::Eq(std::string(getBufferLimit(), 'a'))),
                    false))
        .Times(1);
    dispatcher_->run(Event::Dispatcher::RunType::Block);

    // Lower the watermark, but still above 0.
    filter->onBelowWriteBufferLowWatermark();
    EXPECT_CALL(encoder_callbacks_,
                injectEncodedDataToFilterChain(
                    testing::Property(&Buffer::Instance::toString,
                                      testing::Eq(std::string(getBufferLimit(), 'a'))),
                    _))
        .Times(0);
    dispatcher_->run(Event::Dispatcher::RunType::Block);

    // Further lower the watermark, resume processing.
    filter->onBelowWriteBufferLowWatermark();
    EXPECT_CALL(encoder_callbacks_,
                injectEncodedDataToFilterChain(
                    testing::Property(&Buffer::Instance::toString,
                                      testing::Eq(std::string(getBufferLimit(), 'a'))),
                    _))
        .Times(2);
    dispatcher_->run(Event::Dispatcher::RunType::Block);

    ::testing::Mock::VerifyAndClearExpectations(&encoder_callbacks_);

    filter->onDestroy();
  }
}

} // namespace
} // namespace Cache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
