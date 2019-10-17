#include "extensions/filters/http/cache/simple_http_cache.h"

#include "envoy/registry/registry.h"

#include "common/buffer/buffer_impl.h"
#include "common/common/lock_guard.h"
#include "common/http/header_map_impl.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Cache {
namespace {

class SimpleLookupContext : public LookupContext {
public:
  explicit SimpleLookupContext(SimpleHttpCache& cache, LookupRequest&& request)
      : cache_(cache), request_(std::move(request)) {}

  void getHeaders(LookupHeadersCallback&& cb) override {
    auto entry = cache_.lookup(request_);
    body_ = std::move(entry.body);
    cb(entry.response_headers
           ? request_.makeLookupResult(std::move(entry.response_headers), body_.size())
           : LookupResult{});
  }

  void getBody(const AdjustedByteRange& range, LookupBodyCallback&& cb) override {
    RELEASE_ASSERT(range.lastBytePos() < body_.length(), "Attempt to read past end of body.");
    cb(std::make_unique<Buffer::OwnedImpl>(&body_[range.firstBytePos()],
                                           range.lastBytePos() - range.firstBytePos() + 1));
  }

  void getTrailers(LookupTrailersCallback&& cb) override {
    // TODO(toddmgreer) Support trailers.
    ASSERT(false, "We didn't say there were trailers.");
    cb(nullptr);
  }

  const LookupRequest& request() const { return request_; }

private:
  SimpleHttpCache& cache_;
  const LookupRequest request_;
  std::string body_;
};

class SimpleInsertContext : public InsertContext {
public:
  SimpleInsertContext(LookupContext& lookup_context, SimpleHttpCache& cache)
      : key_(dynamic_cast<SimpleLookupContext&>(lookup_context).request().key()), cache_(cache) {}

  void insertHeaders(const Http::HeaderMap& response_headers, bool end_stream) override {
    ASSERT(!committed_);
    response_headers_ = std::make_unique<Http::HeaderMapImpl>(response_headers);
    if (end_stream) {
      commit();
    }
  }

  void insertBody(const Buffer::Instance& chunk, InsertCallback ready_for_next_chunk,
                  bool end_stream) override {
    ASSERT(!committed_);
    ASSERT(ready_for_next_chunk || end_stream);

    body_.add(chunk);
    if (end_stream) {
      commit();
    } else {
      ready_for_next_chunk(true);
    }
  }

  void insertTrailers(const Http::HeaderMap&) override {
    ASSERT(false); // TODO(toddmgreer) support trailers
  }

private:
  void commit() {
    committed_ = true;
    cache_.insert(key_, std::move(response_headers_), body_.toString());
  }

  Key key_;
  Http::HeaderMapImplPtr response_headers_;
  SimpleHttpCache& cache_;
  int32 byte_range_limit_;
  Buffer::OwnedImpl body_;
  bool committed_ = false;
};
} // namespace

LookupContextPtr SimpleHttpCache::makeLookupContext(LookupRequest&& request) {
  return std::make_unique<SimpleLookupContext>(*this, std::move(request));
}

void SimpleHttpCache::updateHeaders(LookupContextPtr&& lookup_context,
                                    Http::HeaderMapPtr&& response_headers) {
  ASSERT(lookup_context);
  ASSERT(response_headers);
  // TODO(toddmgreer) Support updating headers.
  ASSERT(false);
}

SimpleHttpCache::SimpleHttpCache(envoy::config::filter::http::cache::v2alpha::Cache& config) : HttpCache(), byte_range_parse_limit_(config.byte_range_parse_limit) {
}

SimpleHttpCache::Entry SimpleHttpCache::lookup(const LookupRequest& request) {
  Thread::LockGuard lock(mutex_);
  auto iter = map_.find(request.key());
  if (iter == map_.end()) {
    return Entry{};
  }
  ASSERT(iter->second.response_headers);
  return SimpleHttpCache::Entry{
      std::make_unique<Http::HeaderMapImpl>(*iter->second.response_headers), iter->second.body};
}

void SimpleHttpCache::insert(const Key& key, Http::HeaderMapPtr&& response_headers,
                             std::string&& body) {
  Thread::LockGuard lock(mutex_);
  map_[key] = SimpleHttpCache::Entry{std::move(response_headers), std::move(body)};
}

InsertContextPtr SimpleHttpCache::makeInsertContext(LookupContextPtr&& lookup_context) {
  ASSERT(lookup_context != nullptr);
  return std::make_unique<SimpleInsertContext>(*lookup_context, *this);
}

CacheInfo SimpleHttpCache::cacheInfo() const {
  CacheInfo cache_info;
  cache_info.name_ = "SimpleHttpCache";
  return cache_info;
}

class SimpleHttpCacheFactory : public HttpCacheFactory {
public:
  SimpleHttpCacheFactory() : HttpCacheFactory("SimpleHttpCache") {}
  HttpCache& getCache() override { return cache_; }

private:
  SimpleHttpCache cache_;
};

static Registry::RegisterFactory<SimpleHttpCacheFactory, HttpCacheFactory> register_;

} // namespace Cache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
