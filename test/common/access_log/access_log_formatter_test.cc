#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "common/access_log/access_log_formatter.h"
#include "common/common/utility.h"
#include "common/http/header_map_impl.h"

#include "test/mocks/http/mocks.h"
#include "test/mocks/request_info/mocks.h"
#include "test/mocks/upstream/mocks.h"
#include "test/test_common/printers.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::NiceMock;
using testing::Return;
using testing::ReturnRef;
using testing::_;

namespace Envoy {
namespace AccessLog {

TEST(AccessLogFormatUtilsTest, protocolToString) {
  EXPECT_EQ("HTTP/1.0", AccessLogFormatUtils::protocolToString(Http::Protocol::Http10));
  EXPECT_EQ("HTTP/1.1", AccessLogFormatUtils::protocolToString(Http::Protocol::Http11));
  EXPECT_EQ("HTTP/2", AccessLogFormatUtils::protocolToString(Http::Protocol::Http2));
  EXPECT_EQ("-", AccessLogFormatUtils::protocolToString({}));
}

TEST(AccessLogFormatterTest, plainStringFormatter) {
  PlainStringFormatter formatter("plain");
  Http::TestHeaderMapImpl header{{":method", "GET"}, {":path", "/"}};
  RequestInfo::MockRequestInfo request_info;

  EXPECT_EQ("plain", formatter.format(header, header, request_info));
}

TEST(AccessLogFormatterTest, requestInfoFormatter) {
  EXPECT_THROW(RequestInfoFormatter formatter("unknown_field"), EnvoyException);

  NiceMock<RequestInfo::MockRequestInfo> request_info;
  Http::TestHeaderMapImpl header{{":method", "GET"}, {":path", "/"}};

  {
    RequestInfoFormatter start_time_format("START_TIME");
    SystemTime time;
    EXPECT_CALL(request_info, startTime()).WillOnce(Return(time));
    EXPECT_EQ(AccessLogDateTimeFormatter::fromTime(time),
              start_time_format.format(header, header, request_info));
  }

  {
    RequestInfoFormatter request_duration_format("REQUEST_DURATION");
    Optional<std::chrono::microseconds> duration{std::chrono::microseconds(5000)};
    EXPECT_CALL(request_info, requestReceivedDuration()).WillOnce(ReturnRef(duration));
    EXPECT_EQ("5", request_duration_format.format(header, header, request_info));
  }

  {
    RequestInfoFormatter request_duration_format("REQUEST_DURATION");
    Optional<std::chrono::microseconds> duration;
    EXPECT_CALL(request_info, requestReceivedDuration()).WillOnce(ReturnRef(duration));
    EXPECT_EQ("-", request_duration_format.format(header, header, request_info));
  }

  {
    RequestInfoFormatter response_duration_format("RESPONSE_DURATION");
    Optional<std::chrono::microseconds> duration{std::chrono::microseconds(10000)};
    EXPECT_CALL(request_info, responseReceivedDuration()).WillRepeatedly(ReturnRef(duration));
    EXPECT_EQ("10", response_duration_format.format(header, header, request_info));
  }

  {
    RequestInfoFormatter response_duration_format("RESPONSE_DURATION");
    Optional<std::chrono::microseconds> duration;
    EXPECT_CALL(request_info, responseReceivedDuration()).WillOnce(ReturnRef(duration));
    EXPECT_EQ("-", response_duration_format.format(header, header, request_info));
  }

  {
    RequestInfoFormatter bytes_received_format("BYTES_RECEIVED");
    EXPECT_CALL(request_info, bytesReceived()).WillOnce(Return(1));
    EXPECT_EQ("1", bytes_received_format.format(header, header, request_info));
  }

  {
    RequestInfoFormatter protocol_format("PROTOCOL");
    Optional<Http::Protocol> protocol = Http::Protocol::Http11;
    EXPECT_CALL(request_info, protocol()).WillOnce(ReturnRef(protocol));
    EXPECT_EQ("HTTP/1.1", protocol_format.format(header, header, request_info));
  }

  {
    RequestInfoFormatter response_format("RESPONSE_CODE");
    Optional<uint32_t> response_code{200};
    EXPECT_CALL(request_info, responseCode()).WillRepeatedly(ReturnRef(response_code));
    EXPECT_EQ("200", response_format.format(header, header, request_info));
  }

  {
    RequestInfoFormatter response_code_format("RESPONSE_CODE");
    Optional<uint32_t> response_code;
    EXPECT_CALL(request_info, responseCode()).WillRepeatedly(ReturnRef(response_code));
    EXPECT_EQ("0", response_code_format.format(header, header, request_info));
  }

  {
    RequestInfoFormatter bytes_sent_format("BYTES_SENT");
    EXPECT_CALL(request_info, bytesSent()).WillOnce(Return(1));
    EXPECT_EQ("1", bytes_sent_format.format(header, header, request_info));
  }

  {
    RequestInfoFormatter duration_format("DURATION");
    std::chrono::microseconds time{2000};
    EXPECT_CALL(request_info, duration()).WillOnce(Return(time));
    EXPECT_EQ("2", duration_format.format(header, header, request_info));
  }

  {
    RequestInfoFormatter response_flags_format("RESPONSE_FLAGS");
    ON_CALL(request_info, getResponseFlag(RequestInfo::ResponseFlag::LocalReset))
        .WillByDefault(Return(true));
    EXPECT_EQ("LR", response_flags_format.format(header, header, request_info));
  }

  {
    RequestInfoFormatter upstream_format("UPSTREAM_HOST");
    EXPECT_EQ("10.0.0.1:443", upstream_format.format(header, header, request_info));
  }

  {
    RequestInfoFormatter upstream_format("UPSTREAM_CLUSTER");
    const std::string upstream_cluster_name = "cluster_name";
    EXPECT_CALL(request_info.host_->cluster_, name()).WillOnce(ReturnRef(upstream_cluster_name));
    EXPECT_EQ("cluster_name", upstream_format.format(header, header, request_info));
  }

  {
    RequestInfoFormatter upstream_format("UPSTREAM_HOST");
    EXPECT_CALL(request_info, upstreamHost()).WillOnce(Return(nullptr));
    EXPECT_EQ("-", upstream_format.format(header, header, request_info));
  }

  {
    RequestInfoFormatter upstream_format("UPSTREAM_CLUSTER");
    EXPECT_CALL(request_info, upstreamHost()).WillOnce(Return(nullptr));
    EXPECT_EQ("-", upstream_format.format(header, header, request_info));
  }

  {
    RequestInfoFormatter upstream_format("DOWNSTREAM_LOCAL_ADDRESS");
    EXPECT_EQ("127.0.0.2:0", upstream_format.format(header, header, request_info));
  }

  {
    RequestInfoFormatter upstream_format("DOWNSTREAM_ADDRESS");
    EXPECT_EQ("127.0.0.1", upstream_format.format(header, header, request_info));
  }

  {
    RequestInfoFormatter upstream_format("DOWNSTREAM_REMOTE_ADDRESS_WITHOUT_PORT");
    EXPECT_EQ("127.0.0.1", upstream_format.format(header, header, request_info));
  }

  {
    RequestInfoFormatter upstream_format("DOWNSTREAM_REMOTE_ADDRESS");
    EXPECT_EQ("127.0.0.1:0", upstream_format.format(header, header, request_info));
  }
}

TEST(AccessLogFormatterTest, requestHeaderFormatter) {
  RequestInfo::MockRequestInfo request_info;
  Http::TestHeaderMapImpl request_header{{":method", "GET"}, {":path", "/"}};
  Http::TestHeaderMapImpl response_header{{":method", "PUT"}};

  {
    RequestHeaderFormatter formatter(":Method", "", Optional<size_t>());
    EXPECT_EQ("GET", formatter.format(request_header, response_header, request_info));
  }

  {
    RequestHeaderFormatter formatter(":path", ":method", Optional<size_t>());
    EXPECT_EQ("/", formatter.format(request_header, response_header, request_info));
  }

  {
    RequestHeaderFormatter formatter(":TEST", ":METHOD", Optional<size_t>());
    EXPECT_EQ("GET", formatter.format(request_header, response_header, request_info));
  }

  {
    RequestHeaderFormatter formatter("does_not_exist", "", Optional<size_t>());
    EXPECT_EQ("-", formatter.format(request_header, response_header, request_info));
  }
}

TEST(AccessLogFormatterTest, responseHeaderFormatter) {
  RequestInfo::MockRequestInfo request_info;
  Http::TestHeaderMapImpl request_header{{":method", "GET"}, {":path", "/"}};
  Http::TestHeaderMapImpl response_header{{":method", "PUT"}, {"test", "test"}};

  {
    ResponseHeaderFormatter formatter(":method", "", Optional<size_t>());
    EXPECT_EQ("PUT", formatter.format(request_header, response_header, request_info));
  }

  {
    ResponseHeaderFormatter formatter("test", ":method", Optional<size_t>());
    EXPECT_EQ("test", formatter.format(request_header, response_header, request_info));
  }

  {
    ResponseHeaderFormatter formatter(":path", ":method", Optional<size_t>());
    EXPECT_EQ("PUT", formatter.format(request_header, response_header, request_info));
  }

  {
    ResponseHeaderFormatter formatter("does_not_exist", "", Optional<size_t>());
    EXPECT_EQ("-", formatter.format(request_header, response_header, request_info));
  }
}

TEST(AccessLogFormatterTest, CompositeFormatterSuccess) {
  RequestInfo::MockRequestInfo request_info;
  Http::TestHeaderMapImpl request_header{{"first", "GET"}, {":path", "/"}};
  Http::TestHeaderMapImpl response_header{{"second", "PUT"}, {"test", "test"}};

  {
    const std::string format = "{{%PROTOCOL%}}   %RESP(not exist)%++%RESP(test)% "
                               "%REQ(FIRST?SECOND)% %RESP(FIRST?SECOND)%[]";
    FormatterImpl formatter(format);

    Optional<Http::Protocol> protocol = Http::Protocol::Http11;
    EXPECT_CALL(request_info, protocol()).WillRepeatedly(ReturnRef(protocol));

    EXPECT_EQ("{{HTTP/1.1}}   -++test GET PUT[]",
              formatter.format(request_header, response_header, request_info));
  }

  {
    const std::string format = "{}*JUST PLAIN string]";
    FormatterImpl formatter(format);

    EXPECT_EQ(format, formatter.format(request_header, response_header, request_info));
  }

  {
    const std::string format =
        "%REQ(first):3%|%REQ(first):1%|%RESP(first?second):2%|%REQ(first):10%";
    FormatterImpl formatter(format);

    EXPECT_EQ("GET|G|PU|GET", formatter.format(request_header, response_header, request_info));
  }
}

TEST(AccessLogFormatterTest, ParserFailures) {
  AccessLogFormatParser parser;

  std::vector<std::string> test_cases = {
      "{{%PROTOCOL%}}   ++ %REQ(FIRST?SECOND)% %RESP(FIRST?SECOND)",
      "%REQ(FIRST?SECOND)T%",
      "RESP(FIRST)%",
      "%REQ(valid)% %NOT_VALID%",
      "%REQ(FIRST?SECOND%",
      "%%",
      "%protocol%",
      "%REQ(TEST):%",
      "%REQ(TEST):3q4%",
      "%RESP(TEST):%",
      "%RESP(X?Y):%",
      "%RESP(X?Y):343o24%",
      "%REQ(TEST):10",
      "REQ(:TEST):10%",
      "%REQ(TEST:10%",
      "%REQ("};

  for (const std::string& test_case : test_cases) {
    EXPECT_THROW(parser.parse(test_case), EnvoyException);
  }
}

} // namespace AccessLog
} // namespace Envoy
