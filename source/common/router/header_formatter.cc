#include "common/router/header_formatter.h"

#include <string>

#include "envoy/common/optional.h"

#include "common/access_log/access_log_formatter.h"
#include "common/common/logger.h"
#include "common/common/utility.h"
#include "common/config/metadata.h"
#include "common/json/json_loader.h"
#include "common/request_info/utility.h"

#include "fmt/format.h"

namespace Envoy {
namespace Router {

namespace {

std::string formatUpstreamMetadataParseException(const std::string& params,
                                                 const EnvoyException* cause = nullptr) {
  std::string reason;
  if (cause != nullptr) {
    reason = fmt::format(", because {}", cause->what());
  }

  return fmt::format("Incorrect header configuration. Expected format "
                     "UPSTREAM_METADATA([\"namespace\", \"k\", ...]), actual format "
                     "UPSTREAM_METADATA{}{}",
                     params, reason);
}

// Parses the parameters for UPSTREAM_METADATA and returns a function suitable for accessing the
// specified metadata from an RequestInfo::RequestInfo. Expects a string formatted as:
//   (["a", "b", "c"])
// There must be at least 2 array elements (a metadata namespace and at least 1 key).
std::function<std::string(const Envoy::RequestInfo::RequestInfo&)>
parseUpstreamMetadataField(const std::string& params_str) {
  if (params_str.empty() || params_str.front() != '(' || params_str.back() != ')') {
    throw EnvoyException(formatUpstreamMetadataParseException(params_str));
  }

  std::vector<std::string> params;
  try {
    Json::ObjectSharedPtr parsed_params =
        Json::Factory::loadFromString(StringUtil::subspan(params_str, 1, params_str.size() - 1));

    for (const auto& param : parsed_params->asObjectArray()) {
      params.emplace_back(param->asString());
    }
  } catch (Json::Exception& e) {
    throw EnvoyException(formatUpstreamMetadataParseException(params_str, &e));
  }

  // Minimum parameters are a metadata namespace (e.g. "envoy.lb") and a metadata key.
  if (params.size() < 2) {
    throw EnvoyException(formatUpstreamMetadataParseException(params_str));
  }

  return [params](const Envoy::RequestInfo::RequestInfo& request_info) -> std::string {
    Upstream::HostDescriptionConstSharedPtr host = request_info.upstreamHost();
    if (!host) {
      return std::string();
    }

    const ProtobufWkt::Value* value =
        &Config::Metadata::metadataValue(host->metadata(), params[0], params[1]);
    if (value->kind_case() == ProtobufWkt::Value::KIND_NOT_SET) {
      // No kind indicates default ProtobufWkt::Value which means namespace or key not
      // found.
      return std::string();
    }

    size_t i = 2;
    while (i < params.size()) {
      if (!value->has_struct_value()) {
        break;
      }

      const auto field_it = value->struct_value().fields().find(params[i]);
      if (field_it == value->struct_value().fields().end()) {
        return std::string();
      }

      value = &field_it->second;
      i++;
    }

    if (i < params.size()) {
      // Didn't find all the keys.
      return std::string();
    }

    switch (value->kind_case()) {
    case ProtobufWkt::Value::kNumberValue:
      return fmt::format("{}", value->number_value());

    case ProtobufWkt::Value::kStringValue:
      return value->string_value();

    case ProtobufWkt::Value::kBoolValue:
      return value->bool_value() ? "true" : "false";

    default:
      // Unsupported type or null value.
      ENVOY_LOG_MISC(debug, "unsupported value type for metadata [{}]",
                     StringUtil::join(params, ", "));
      return std::string();
    }
  };
}

} // namespace

RequestInfoHeaderFormatter::RequestInfoHeaderFormatter(const std::string& field_name, bool append)
    : append_(append) {
  if (field_name == "PROTOCOL") {
    field_extractor_ = [](const Envoy::RequestInfo::RequestInfo& request_info) {
      return Envoy::AccessLog::AccessLogFormatUtils::protocolToString(request_info.protocol());
    };
  } else if (field_name == "CLIENT_IP" || field_name == "DOWNSTREAM_REMOTE_ADDRESS_WITHOUT_PORT") {
    // DEPRECATED: "CLIENT_IP" will be removed post 1.6.0.
    field_extractor_ = [](const Envoy::RequestInfo::RequestInfo& request_info) {
      return RequestInfo::Utility::formatDownstreamAddressNoPort(
          *request_info.downstreamRemoteAddress());
    };
  } else if (StringUtil::startsWith(field_name.c_str(), "UPSTREAM_METADATA")) {
    field_extractor_ =
        parseUpstreamMetadataField(field_name.substr(sizeof("UPSTREAM_METADATA") - 1));
  } else {
    throw EnvoyException(fmt::format("field '{}' not supported as custom header", field_name));
  }
}

const std::string
RequestInfoHeaderFormatter::format(const Envoy::RequestInfo::RequestInfo& request_info) const {
  return field_extractor_(request_info);
}

} // namespace Router
} // namespace Envoy
