#pragma once

#include <strings.h>

#include <chrono>
#include <cstdint>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "envoy/common/time.h"

#include "absl/strings/string_view.h"

namespace Envoy {
/**
 * Utility class for formatting dates given a strftime style format string.
 */
class DateFormatter {
public:
  DateFormatter(const std::string& format_string) : format_string_(format_string) {}

  /**
   * @return std::string representing the GMT/UTC time based on the input time.
   */
  std::string fromTime(const SystemTime& time);

  /**
   * @return std::string representing the current GMT/UTC time based on the format string.
   */
  std::string now();

private:
  std::string fromTimeT(time_t time);

  std::string format_string_;
};

/**
 * Utility class for access log date/time format with milliseconds support.
 */
class AccessLogDateTimeFormatter {
public:
  static std::string fromTime(const SystemTime& time);
};

/**
 * Production implementation of SystemTimeSource that returns the current time.
 */
class ProdSystemTimeSource : public SystemTimeSource {
public:
  // SystemTimeSource
  SystemTime currentTime() override { return std::chrono::system_clock::now(); }

  static ProdSystemTimeSource instance_;
};

/**
 * Production implementation of MonotonicTimeSource that returns the current time.
 */
class ProdMonotonicTimeSource : public MonotonicTimeSource {
public:
  // MonotonicTimeSource
  MonotonicTime currentTime() override { return std::chrono::steady_clock::now(); }

  static ProdMonotonicTimeSource instance_;
};

/**
 * Class used for creating non-copying std::istream's. See InputConstMemoryStream below.
 */
class ConstMemoryStreamBuffer : public std::streambuf {
public:
  ConstMemoryStreamBuffer(const char* data, size_t size);
};

/**
 * std::istream class similar to std::istringstream, except that it provides a view into a region of
 * constant memory. It can be more efficient than std::istringstream because it doesn't copy the
 * provided string.
 *
 * See https://stackoverflow.com/a/13059195/4447365.
 */
class InputConstMemoryStream : public virtual ConstMemoryStreamBuffer, public std::istream {
public:
  InputConstMemoryStream(const char* data, size_t size);
};

/**
 * Utility class for date/time helpers.
 */
class DateUtil {
public:
  /**
   * @return whether a time_point contains a valid, not default constructed time.
   */
  static bool timePointValid(SystemTime time_point);

  /**
   * @return whether a time_point contains a valid, not default constructed time.
   */
  static bool timePointValid(MonotonicTime time_point);
};

/**
 * Utility routines for working with strings.
 */
class StringUtil {
public:
  static const char WhitespaceChars[];

  /**
   * Convert a string to an unsigned long, checking for error.
   * @param return TRUE if successful, FALSE otherwise.
   */
  static bool atoul(const char* str, uint64_t& out, int base = 10);

  /**
   * Perform a case insensitive compare of 2 strings.
   * @param lhs supplies string 1.
   * @param rhs supplies string 2.
   * @return < 0, 0, > 0 depending on the comparison result.
   */
  static int caseInsensitiveCompare(const char* lhs, const char* rhs) {
    return strcasecmp(lhs, rhs);
  }

  /**
   * Convert an unsigned integer to a base 10 string as fast as possible.
   * @param out supplies the string to fill.
   * @param out_len supplies the length of the output buffer. Must be >= MIN_ITOA_OUT_LEN.
   * @param i supplies the number to convert.
   * @return the size of the string, not including the null termination.
   */
  static constexpr size_t MIN_ITOA_OUT_LEN = 21;
  static uint32_t itoa(char* out, size_t out_len, uint64_t i);

  /**
   * Trim leading whitespace from a string view.
   * @param source supplies the string view to be trimmed.
   * @return trimmed string view.
   */
  static absl::string_view ltrim(absl::string_view source);

  /**
   * Trim trailing whitespaces from a string view.
   * @param source supplies the string view to be trimmed.
   * @return trimmed string view.
   */
  static absl::string_view rtrim(absl::string_view source);

  /**
   * Trim leading and trailing whitespaces from a string view.
   * @param source supplies the string view to be trimmed.
   * @return trimmed string view.
   */
  static absl::string_view trim(absl::string_view source);

  /**
   * Look up for an exactly token in a delimiter-separated string view.
   * @param source supplies the delimiter-separated string view.
   * @param multi-delimiter supplies chars used to split the delimiter-separated string view.
   * @param token supplies the lookup string view.
   * @param trim_whitespace remove leading and trailing whitespaces from each of the split
   * string views; default = true.
   * @return true if found and false otherwise.
   *
   * E.g,
   *
   * findToken("A=5; b", "=;", "5")   . true
   * findToken("A=5; b", "=;", "A=5") . false
   * findToken("A=5; b", "=;", "A")   . true
   * findToken("A=5; b", "=;", "b")   . true
   * findToken("A=5", ".", "A=5")     . true
   */
  static bool findToken(absl::string_view source, absl::string_view delimiters,
                        absl::string_view token, bool trim_whitespace = true);

  /**
   * Crop characters from a string view starting at the first character of the matched
   * delimiter string view until the end of the source string view.
   * @param source supplies the string view to be processed.
   * @param delimiter supplies the string view that delimits the starting point for deletion.
   * @param trim_whitespace remove leading and trailing whitespaces from each of the split
   * string views; default = true.
   * @return sub-string of the string view if any.
   */
  static absl::string_view cropRight(absl::string_view source, absl::string_view delimiters,
                                     bool trim_whitespace = true);

  /**
   * Split a delimiter-separated string view.
   * @param source supplies the delimiter-separated string view.
   * @param multi-delimiter supplies chars used to split the delimiter-separated string view.
   * @param keep_empty_string result contains empty strings if the string starts or ends with
   * 'split', or if instances of 'split' are adjacent; default = false.
   * @return true if found and false otherwise.
   */
  static std::vector<absl::string_view> splitToken(absl::string_view source,
                                                   absl::string_view delimiters,
                                                   bool keep_empty_string = false);

  /**
   * Size-bounded string copying and concatenation
   */
  static size_t strlcpy(char* dst, const char* src, size_t size);

  /**
   * Join elements of a vector into a string delimited by delimiter.
   * @param source supplies the strings to join.
   * @param delimiter supplies the delimiter to join them together.
   * @return string combining elements of `source` with `delimiter` in between each element.
   */
  static std::string join(const std::vector<std::string>& source, const std::string& delimiter);

  /**
   * Version of substr() that operates on a start and end index instead of a start index and a
   * length.
   */
  static std::string subspan(const std::string& source, size_t start, size_t end);

  /**
   * Escape strings for logging purposes. Returns a copy of the string with
   * \n, \r, \t, and " (double quote) escaped.
   * @param source supplies the string to escape.
   * @return escaped string.
   */
  static std::string escape(const std::string& source);

  /**
   * @return true if @param source ends with @param end.
   */
  static bool endsWith(const std::string& source, const std::string& end);

  /**
   * @param case_sensitive determines if the compare is case sensitive
   * @return true if @param source starts with @param start and ignores cases.
   */
  static bool startsWith(const char* source, const std::string& start, bool case_sensitive = true);

  /**
   * Provide a default value for a string if empty.
   * @param s string.
   * @param default_value replacement for s if empty.
   * @return s is !s.empty() otherwise default_value.
   */
  static const std::string& nonEmptyStringOrDefault(const std::string& s,
                                                    const std::string& default_value);

  /**
   * Convert a string to upper case.
   * @param s string.
   * @return std::string s converted to upper case.
   */
  static std::string toUpper(absl::string_view s);
};

/**
 * Utilities for finding primes
 */
class Primes {
public:
  /**
   * Determines whether x is prime.
   */
  static bool isPrime(uint32_t x);

  /**
   * Finds the next prime number larger than x.
   */
  static uint32_t findPrimeLargerThan(uint32_t x);
};

/**
 * Utilities for constructing regular expressions.
 */
class RegexUtil {
public:
  /*
   * Constructs a std::regex, converting any std::regex_error exception into an EnvoyException.
   * @param regex std::string containing the regular expression to parse.
   * @param flags std::regex::flag_type containing parser flags. Defaults to std::regex::optimize.
   * @return std::regex constructed from regex and flags
   * @throw EnvoyException if the regex string is invalid.
   */
  static std::regex parseRegex(const std::string& regex,
                               std::regex::flag_type flags = std::regex::optimize);
};

} // namespace Envoy
