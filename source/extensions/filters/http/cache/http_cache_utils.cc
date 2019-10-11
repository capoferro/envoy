#include "extensions/filters/http/cache/http_cache_utils.h"
#include "common/common/utility.h"

#include <array>
#include <string>

#include "absl/algorithm/container.h"
#include "absl/strings/ascii.h"
#include "absl/strings/numbers.h"
#include "absl/strings/strip.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Cache {
namespace Internal {

namespace {

// True for characters defined as tchars by
// https://tools.ietf.org/html/rfc7230#section-3.2.6
//
// tchar           = "!" / "#" / "$" / "%" / "&" / "'" / "*" / "+"
//                 / "-" / "." / "^" / "_" / "`" / "|" / "~" / DIGIT / ALPHA
bool tchar(char c) {
  switch (c) {
  case '!':
  case '#':
  case '$':
  case '%':
  case '&':
  case '*':
  case '+':
  case '-':
  case '.':
  case '^':
  case '_':
  case '`':
  case '|':
  case '~':
    return true;
  }
  return absl::ascii_isalnum(c);
}

// Removes an initial HTTP header field value token, as defined by
// https://tools.ietf.org/html/rfc7230#section-3.2.6. Returns true if an initial
// token was present.
//
// token           = 1*tchar
bool eatToken(absl::string_view& s) {
  const absl::string_view::iterator token_end = c_find_if_not(s, &tchar);
  if (token_end == s.begin()) {
    return false;
  }
  s.remove_prefix(token_end - s.begin());
  return true;
}

// Removes an initial token or quoted-string (if present), as defined by
// https://tools.ietf.org/html/rfc7234#section-5.2. If a cache-control directive
// has an argument (as indicated by '='), it should be in this form.
//
// quoted-string   = DQUOTE *( qdtext / quoted-pair ) DQUOTE
// qdtext          = HTAB / SP /%x21 / %x23-5B / %x5D-7E / obs-text
// obs-text        = %x80-FF
// quoted-pair     = "\" ( HTAB / SP / VCHAR / obs-text )
// VCHAR           =  %x21-7E  ; visible (printing) characters
//
// For example, the directive "my-extension=42" has an argument of "42", so an
// input of "public, my-extension=42, max-age=999"
void eatDirectiveArgument(absl::string_view& s) {
  if (s.empty()) {
    return;
  }
  if (s.front() == '"') {
    // TODO(toddmgreer) handle \-escaped quotes
    const size_t closing_quote = s.find('"', 1);
    s.remove_prefix(closing_quote);
  } else {
    eatToken(s);
  }
}
} // namespace

// If s is nonnull and begins with a decimal number ([0-9]+), removes it from
// the input and returns a SystemTime::duration representing that many seconds.
// If s is null or doesn't begin with digits, returns
// SystemTime::duration::zero(). If parsing overflows, returns
// SystemTime::duration::max().
SystemTime::duration eatLeadingDuration(absl::string_view& s) {
  absl::optional<uint64_t> n = StringUtil::readAndRemoveLeadingDigits(s);
  SystemTime::duration duration = SystemTime::duration::zero();
  if (n) {
    int64_t signed_n = static_cast<int64_t>(n.value());
    if (signed_n < 0) {
      duration = SystemTime::duration::max();
    } else {
      duration = std::chrono::seconds(signed_n);
    }
  }
  if (!s.empty()) {
    if (isdigit(s[0])) {
      // uint64_t overflow detected (ie. string contains digits but they were
      // not consumed). In order to determine if this is a number, and we should
      // return max(), or if contains non-digit characters and we
      // should return zero(), we need to read the characters that failed to
      // parse to the next comma and check if there are non-digits.  In
      // practice, this should almost never happen.
      std::size_t nondigit = s.find_first_not_of("0123456789");
      if (nondigit == std::string::npos || s[nondigit] == ',') {
        // All digits, it's an overflow
        duration = SystemTime::duration::max();
      } else {
        // Not all digits, it's an invalid string
        duration = SystemTime::duration::zero();
      }
    } else if (s[0] != ',') {
      duration = SystemTime::duration::zero();
    }
  }

  return duration;
}

// Returns the effective max-age represented by cache-control. If the result is
// SystemTime::duration::zero(), or is less than the response's, the response
// should be validated.
//
// TODO(toddmgreer) Write a CacheControl class to fully parse the cache-control
// header value. Consider sharing with the gzip filter.
SystemTime::duration effectiveMaxAge(absl::string_view cache_control) {
  // The grammar for This Cache-Control header value should be:
  // Cache-Control   = 1#cache-directive
  // cache-directive = token [ "=" ( token / quoted-string ) ]
  // token           = 1*tchar
  // tchar           = "!" / "#" / "$" / "%" / "&" / "'" / "*" / "+"
  //                 / "-" / "." / "^" / "_" / "`" / "|" / "~" / DIGIT / ALPHA
  // quoted-string   = DQUOTE *( qdtext / quoted-pair ) DQUOTE
  // qdtext          = HTAB / SP /%x21 / %x23-5B / %x5D-7E / obs-text
  // obs-text        = %x80-FF
  // quoted-pair     = "\" ( HTAB / SP / VCHAR / obs-text )
  // VCHAR           =  %x21-7E  ; visible (printing) characters
  SystemTime::duration max_age = SystemTime::duration::zero();
  bool found_s_maxage = false;
  while (!cache_control.empty()) {
    // Each time through the loop, we eat one cache-directive. Each branch
    // either returns or completely eats a cache-directive.
    if (ConsumePrefix(&cache_control, "no-cache")) {
      if (eatToken(cache_control)) {
        // The token wasn't no-cache; it just started that way, so we must
        // finish eating this cache-directive.
        if (ConsumePrefix(&cache_control, "=")) {
          eatDirectiveArgument(cache_control);
        }
      } else {
        // Found a no-cache directive, so validation is required.
        return SystemTime::duration::zero();
      }
    } else if (ConsumePrefix(&cache_control, "s-maxage=")) {
      max_age = eatLeadingDuration(cache_control);
      found_s_maxage = true;
      cache_control = StripLeadingAsciiWhitespace(cache_control);
      if (!cache_control.empty() && cache_control[0] != ',') {
        // Unexpected text at end of directive
        return SystemTime::duration::zero();
      }
    } else if (!found_s_maxage && ConsumePrefix(&cache_control, "max-age=")) {
      max_age = eatLeadingDuration(cache_control);
    } else if (eatToken(cache_control)) {
      // Unknown directive--ignore.
      if (ConsumePrefix(&cache_control, "=")) {
        eatDirectiveArgument(cache_control);
      }
    } else {
      // This directive starts with illegal characters. Require validation.
      return SystemTime::duration::zero();
    }
    // Whichever branch we took should have consumed the entire cache-directive,
    // so we just need to eat the delimiter and optional whitespace.
    ConsumePrefix(&cache_control, ",");
    cache_control = StripLeadingAsciiWhitespace(cache_control);
  }
  return max_age;
}

SystemTime httpTime(const Http::HeaderEntry* header_entry) {
  if (!header_entry) {
    return {};
  }
  absl::Time time;
  const std::string input(header_entry->value().getStringView());

  // Acceptable Date/Time Formats per
  // https://tools.ietf.org/html/rfc7231#section-7.1.1.1
  //
  // Sun, 06 Nov 1994 08:49:37 GMT    ; IMF-fixdate
  // Sunday, 06-Nov-94 08:49:37 GMT   ; obsolete RFC 850 format
  // Sun Nov  6 08:49:37 1994         ; ANSI C's asctime() format
  const std::array<std::string, 3> rfc7231_date_formats = {
      "%a, %d %b %Y %H:%M:%S GMT", "%A, %d-%b-%y %H:%M:%S GMT", "%a %b %e %H:%M:%S %Y"};
  for (const std::string& format : rfc7231_date_formats) {
    if (absl::ParseTime(format, input, &time, nullptr)) {
      return ToChronoTime(time);
    }
  }
  return {};
}
} // namespace Internal
} // namespace Cache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
