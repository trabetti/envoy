#include "common/router/config_utility.h"

#include <regex>
#include <string>
#include <vector>

#include "common/common/assert.h"

namespace Envoy {
namespace Router {

bool ConfigUtility::QueryParameterMatcher::matches(
    const Http::Utility::QueryParams& request_query_params) const {
  auto query_param = request_query_params.find(name_);
  if (query_param == request_query_params.end()) {
    return false;
  } else if (is_regex_) {
    return std::regex_match(query_param->second, regex_pattern_);
  } else if (value_.length() == 0) {
    return true;
  } else {
    return (value_ == query_param->second);
  }
}

Upstream::ResourcePriority
ConfigUtility::parsePriority(const envoy::api::v2::RoutingPriority& priority) {
  switch (priority) {
  case envoy::api::v2::RoutingPriority::DEFAULT:
    return Upstream::ResourcePriority::Default;
  case envoy::api::v2::RoutingPriority::HIGH:
    return Upstream::ResourcePriority::High;
  default:
    NOT_IMPLEMENTED;
  }
}

bool ConfigUtility::matchHeaders(const Http::HeaderMap& request_headers,
                                 const std::vector<HeaderData>& config_headers) {
  bool matches = true;

  if (!config_headers.empty()) {
    for (const HeaderData& cfg_header_data : config_headers) {
      const Http::HeaderEntry* header = request_headers.get(cfg_header_data.name_);
      if (cfg_header_data.value_.empty()) {
        matches &= (header != nullptr);
      } else if (!cfg_header_data.is_regex_) {
        matches &= (header != nullptr) && (header->value() == cfg_header_data.value_.c_str());
      } else {
        matches &= (header != nullptr) &&
                   std::regex_match(header->value().c_str(), cfg_header_data.regex_pattern_);
      }
      if (!matches) {
        break;
      }
    }
  }

  return matches;
}

bool ConfigUtility::matchQueryParams(
    const Http::Utility::QueryParams& query_params,
    const std::vector<QueryParameterMatcher>& config_query_params) {
  for (const auto& config_query_param : config_query_params) {
    if (!config_query_param.matches(query_params)) {
      return false;
    }
  }

  return true;
}

Http::Code ConfigUtility::parseRedirectResponseCode(
    const envoy::api::v2::RedirectAction::RedirectResponseCode& code) {
  switch (code) {
  case envoy::api::v2::RedirectAction::MOVED_PERMANENTLY:
    return Http::Code::MovedPermanently;
  case envoy::api::v2::RedirectAction::FOUND:
    return Http::Code::Found;
  case envoy::api::v2::RedirectAction::SEE_OTHER:
    return Http::Code::SeeOther;
  case envoy::api::v2::RedirectAction::TEMPORARY_REDIRECT:
    return Http::Code::TemporaryRedirect;
  case envoy::api::v2::RedirectAction::PERMANENT_REDIRECT:
    return Http::Code::PermanentRedirect;
  default:
    NOT_IMPLEMENTED;
  }
}

Http::Code ConfigUtility::parseClusterNotFoundResponseCode(
    const envoy::api::v2::RouteAction::ClusterNotFoundResponseCode& code) {
  switch (code) {
  case envoy::api::v2::RouteAction::SERVICE_UNAVAILABLE:
    return Http::Code::ServiceUnavailable;
  case envoy::api::v2::RouteAction::NOT_FOUND:
    return Http::Code::NotFound;
  default:
    NOT_IMPLEMENTED;
  }
}

} // namespace Router
} // namespace Envoy
