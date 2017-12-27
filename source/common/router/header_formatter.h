#pragma once

#include <functional>
#include <memory>
#include <string>

#include "envoy/access_log/access_log.h"

namespace Envoy {
namespace Router {

/**
 * Interface for all types of header formatters used for custom request headers.
 */
class HeaderFormatter {
public:
  virtual ~HeaderFormatter() {}

  virtual const std::string format(const Envoy::RequestInfo::RequestInfo& request_info) const PURE;

  /**
   * @return bool indicating whether the formatted header should be appended to the existing
   *              headers
   */
  virtual bool append() const PURE;
};

typedef std::unique_ptr<HeaderFormatter> HeaderFormatterPtr;

/**
 * A formatter that expands the request header variable to a value based on info in RequestInfo.
 */
class RequestInfoHeaderFormatter : public HeaderFormatter {
public:
  RequestInfoHeaderFormatter(const std::string& field_name, bool append);

  // HeaderFormatter::format
  const std::string format(const Envoy::RequestInfo::RequestInfo& request_info) const override;
  bool append() const override { return append_; }

private:
  std::function<std::string(const Envoy::RequestInfo::RequestInfo&)> field_extractor_;
  const bool append_;
};

/**
 * A formatter that returns back the same static header value.
 */
class PlainHeaderFormatter : public HeaderFormatter {
public:
  PlainHeaderFormatter(const std::string& static_header_value, bool append)
      : static_value_(static_header_value), append_(append){};

  // HeaderFormatter::format
  const std::string format(const Envoy::RequestInfo::RequestInfo&) const override {
    return static_value_;
  };
  bool append() const override { return append_; }

private:
  const std::string static_value_;
  const bool append_;
};

} // namespace Router
} // namespace Envoy
