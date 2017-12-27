#include "common/access_log/access_log_formatter.h"

#include <cstdint>
#include <string>
#include <vector>

#include "common/common/assert.h"
#include "common/common/utility.h"
#include "common/request_info/utility.h"

#include "fmt/format.h"

namespace Envoy {
namespace AccessLog {

static const std::string UnspecifiedValueString = "-";

const std::string AccessLogFormatUtils::DEFAULT_FORMAT =
    "[%START_TIME%] \"%REQ(:METHOD)% %REQ(X-ENVOY-ORIGINAL-PATH?:PATH)% %PROTOCOL%\" "
    "%RESPONSE_CODE% %RESPONSE_FLAGS% %BYTES_RECEIVED% %BYTES_SENT% %DURATION% "
    "%RESP(X-ENVOY-UPSTREAM-SERVICE-TIME)% "
    "\"%REQ(X-FORWARDED-FOR)%\" \"%REQ(USER-AGENT)%\" \"%REQ(X-REQUEST-ID)%\" "
    "\"%REQ(:AUTHORITY)%\" \"%UPSTREAM_HOST%\"\n";

FormatterPtr AccessLogFormatUtils::defaultAccessLogFormatter() {
  return FormatterPtr{new FormatterImpl(DEFAULT_FORMAT)};
}

static const std::string Http10String = "HTTP/1.0";
static const std::string Http11String = "HTTP/1.1";
static const std::string Http2String = "HTTP/2";

const std::string&
AccessLogFormatUtils::protocolToString(const Optional<Http::Protocol>& protocol) {
  if (protocol.valid()) {
    switch (protocol.value()) {
    case Http::Protocol::Http10:
      return Http10String;
    case Http::Protocol::Http11:
      return Http11String;
    case Http::Protocol::Http2:
      return Http2String;
    }
  } else {
    return UnspecifiedValueString;
  }

  NOT_REACHED;
}

FormatterImpl::FormatterImpl(const std::string& format) {
  formatters_ = AccessLogFormatParser::parse(format);
}

std::string FormatterImpl::format(const Http::HeaderMap& request_headers,
                                  const Http::HeaderMap& response_headers,
                                  const RequestInfo::RequestInfo& request_info) const {
  std::string log_line;
  log_line.reserve(256);

  for (const FormatterPtr& formatter : formatters_) {
    log_line += formatter->format(request_headers, response_headers, request_info);
  }

  return log_line;
}

void AccessLogFormatParser::parseCommand(const std::string& token, const size_t start,
                                         std::string& main_header, std::string& alternative_header,
                                         Optional<size_t>& max_length) {
  size_t end_request = token.find(')', start);

  if (end_request != token.length() - 1) {
    // Closing bracket is not found.
    if (end_request == std::string::npos) {
      throw EnvoyException(fmt::format("Closing bracket is missing in token: {}", token));
    }

    // Closing bracket should be either last one or followed by ':' to denote limitation.
    if (token[end_request + 1] != ':') {
      throw EnvoyException(fmt::format("Incorrect position of ')' in token: {}", token));
    }

    std::string length_str = token.substr(end_request + 2);
    uint64_t length_value;

    if (!StringUtil::atoul(length_str.c_str(), length_value)) {
      throw EnvoyException(fmt::format("Length must be an integer, given: {}", length_str));
    }

    max_length.value(length_value);
  }

  std::string header_name = token.substr(start, end_request - start);
  size_t separator = header_name.find('?');

  if (separator == std::string::npos) {
    main_header = header_name;
    alternative_header = "";
  } else {
    main_header = header_name.substr(0, separator);
    alternative_header = header_name.substr(separator + 1, end_request - separator - 1);
  }
}

std::vector<FormatterPtr> AccessLogFormatParser::parse(const std::string& format) {
  std::string current_token;
  std::vector<FormatterPtr> formatters;

  for (size_t pos = 0; pos < format.length(); ++pos) {
    if (format[pos] == '%') {
      if (!current_token.empty()) {
        formatters.emplace_back(FormatterPtr{new PlainStringFormatter(current_token)});
        current_token = "";
      }

      size_t command_end_position = format.find('%', pos + 1);
      if (command_end_position == std::string::npos) {
        throw EnvoyException(fmt::format(
            "Incorrect configuration: {}. Expected end of operation '%', around position {}",
            format, pos));
      }
      std::string token = format.substr(pos + 1, command_end_position - (pos + 1));

      if (token.find("REQ(") == 0) {
        std::string main_header, alternative_header;
        Optional<size_t> max_length;
        const size_t start = 4;

        parseCommand(token, start, main_header, alternative_header, max_length);

        formatters.emplace_back(
            FormatterPtr(new RequestHeaderFormatter(main_header, alternative_header, max_length)));
      } else if (token.find("RESP(") == 0) {
        std::string main_header, alternative_header;
        Optional<size_t> max_length;
        const size_t start = 5;

        parseCommand(token, start, main_header, alternative_header, max_length);

        formatters.emplace_back(
            FormatterPtr(new ResponseHeaderFormatter(main_header, alternative_header, max_length)));
      } else {
        formatters.emplace_back(FormatterPtr(new RequestInfoFormatter(token)));
      }

      pos = command_end_position;
    } else {
      current_token += format[pos];
    }
  }

  if (!current_token.empty()) {
    formatters.emplace_back(FormatterPtr{new PlainStringFormatter(current_token)});
  }

  return formatters;
}

RequestInfoFormatter::RequestInfoFormatter(const std::string& field_name) {
  if (field_name == "START_TIME") {
    field_extractor_ = [](const RequestInfo::RequestInfo& request_info) {
      return AccessLogDateTimeFormatter::fromTime(request_info.startTime());
    };
  } else if (field_name == "REQUEST_DURATION") {
    field_extractor_ = [](const RequestInfo::RequestInfo& request_info) {
      Optional<std::chrono::microseconds> duration = request_info.requestReceivedDuration();
      if (duration.valid()) {
        return std::to_string(
            std::chrono::duration_cast<std::chrono::milliseconds>(duration.value()).count());
      } else {
        return UnspecifiedValueString;
      }
    };
  } else if (field_name == "RESPONSE_DURATION") {
    field_extractor_ = [](const RequestInfo::RequestInfo& request_info) {
      Optional<std::chrono::microseconds> duration = request_info.responseReceivedDuration();
      if (duration.valid()) {
        return std::to_string(
            std::chrono::duration_cast<std::chrono::milliseconds>(duration.value()).count());
      } else {
        return UnspecifiedValueString;
      }
    };
  } else if (field_name == "BYTES_RECEIVED") {
    field_extractor_ = [](const RequestInfo::RequestInfo& request_info) {
      return std::to_string(request_info.bytesReceived());
    };
  } else if (field_name == "PROTOCOL") {
    field_extractor_ = [](const RequestInfo::RequestInfo& request_info) {
      return AccessLogFormatUtils::protocolToString(request_info.protocol());
    };
  } else if (field_name == "RESPONSE_CODE") {
    field_extractor_ = [](const RequestInfo::RequestInfo& request_info) {
      return request_info.responseCode().valid()
                 ? std::to_string(request_info.responseCode().value())
                 : "0";
    };
  } else if (field_name == "BYTES_SENT") {
    field_extractor_ = [](const RequestInfo::RequestInfo& request_info) {
      return std::to_string(request_info.bytesSent());
    };
  } else if (field_name == "DURATION") {
    field_extractor_ = [](const RequestInfo::RequestInfo& request_info) {
      return std::to_string(
          std::chrono::duration_cast<std::chrono::milliseconds>(request_info.duration()).count());
    };
  } else if (field_name == "RESPONSE_FLAGS") {
    field_extractor_ = [](const RequestInfo::RequestInfo& request_info) {
      return RequestInfo::ResponseFlagUtils::toShortString(request_info);
    };
  } else if (field_name == "UPSTREAM_HOST") {
    field_extractor_ = [](const RequestInfo::RequestInfo& request_info) {
      if (request_info.upstreamHost()) {
        return request_info.upstreamHost()->address()->asString();
      } else {
        return UnspecifiedValueString;
      }
    };
  } else if (field_name == "UPSTREAM_CLUSTER") {
    field_extractor_ = [](const RequestInfo::RequestInfo& request_info) {
      std::string upstream_cluster_name;
      if (nullptr != request_info.upstreamHost()) {
        upstream_cluster_name = request_info.upstreamHost()->cluster().name();
      }

      return upstream_cluster_name.empty() ? UnspecifiedValueString : upstream_cluster_name;
    };
  } else if (field_name == "UPSTREAM_LOCAL_ADDRESS") {
    field_extractor_ = [](const RequestInfo::RequestInfo& request_info) {
      return request_info.upstreamLocalAddress() != nullptr
                 ? request_info.upstreamLocalAddress()->asString()
                 : UnspecifiedValueString;
    };
  } else if (field_name == "DOWNSTREAM_LOCAL_ADDRESS") {
    field_extractor_ = [](const RequestInfo::RequestInfo& request_info) {
      return request_info.downstreamLocalAddress()->asString();
    };
  } else if (field_name == "DOWNSTREAM_REMOTE_ADDRESS") {
    field_extractor_ = [](const RequestInfo::RequestInfo& request_info) {
      return request_info.downstreamRemoteAddress()->asString();
    };
  } else if (field_name == "DOWNSTREAM_ADDRESS" ||
             field_name == "DOWNSTREAM_REMOTE_ADDRESS_WITHOUT_PORT") {
    // DEPRECATED: "DOWNSTREAM_ADDRESS" will be removed post 1.6.0.
    field_extractor_ = [](const RequestInfo::RequestInfo& request_info) {
      return RequestInfo::Utility::formatDownstreamAddressNoPort(
          *request_info.downstreamRemoteAddress());
    };
  } else {
    throw EnvoyException(fmt::format("Not supported field in RequestInfo: {}", field_name));
  }
}

std::string RequestInfoFormatter::format(const Http::HeaderMap&, const Http::HeaderMap&,
                                         const RequestInfo::RequestInfo& request_info) const {
  return field_extractor_(request_info);
}

PlainStringFormatter::PlainStringFormatter(const std::string& str) : str_(str) {}

std::string PlainStringFormatter::format(const Http::HeaderMap&, const Http::HeaderMap&,
                                         const RequestInfo::RequestInfo&) const {
  return str_;
}

HeaderFormatter::HeaderFormatter(const std::string& main_header,
                                 const std::string& alternative_header,
                                 const Optional<size_t>& max_length)
    : main_header_(main_header), alternative_header_(alternative_header), max_length_(max_length) {}

std::string HeaderFormatter::format(const Http::HeaderMap& headers) const {
  const Http::HeaderEntry* header = headers.get(main_header_);

  if (!header && !alternative_header_.get().empty()) {
    header = headers.get(alternative_header_);
  }

  std::string header_value_string;
  if (!header) {
    header_value_string = UnspecifiedValueString;
  } else {
    header_value_string = header->value().c_str();
  }

  if (max_length_.valid() && header_value_string.length() > max_length_.value()) {
    return header_value_string.substr(0, max_length_.value());
  }

  return header_value_string;
}

ResponseHeaderFormatter::ResponseHeaderFormatter(const std::string& main_header,
                                                 const std::string& alternative_header,
                                                 const Optional<size_t>& max_length)
    : HeaderFormatter(main_header, alternative_header, max_length) {}

std::string ResponseHeaderFormatter::format(const Http::HeaderMap&,
                                            const Http::HeaderMap& response_headers,
                                            const RequestInfo::RequestInfo&) const {
  return HeaderFormatter::format(response_headers);
}

RequestHeaderFormatter::RequestHeaderFormatter(const std::string& main_header,
                                               const std::string& alternative_header,
                                               const Optional<size_t>& max_length)
    : HeaderFormatter(main_header, alternative_header, max_length) {}

std::string RequestHeaderFormatter::format(const Http::HeaderMap& request_headers,
                                           const Http::HeaderMap&,
                                           const RequestInfo::RequestInfo&) const {
  return HeaderFormatter::format(request_headers);
}

} // namespace AccessLog
} // namespace Envoy
