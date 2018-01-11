#include "common/access_log/grpc_access_log_impl.h"
#include "common/network/address_impl.h"

#include "test/mocks/access_log/mocks.h"
#include "test/mocks/grpc/mocks.h"
#include "test/mocks/local_info/mocks.h"
#include "test/mocks/request_info/mocks.h"
#include "test/mocks/thread_local/mocks.h"

using namespace std::chrono_literals;
using testing::InSequence;
using testing::Invoke;
using testing::NiceMock;
using testing::Return;
using testing::_;

namespace Envoy {
namespace AccessLog {

class GrpcAccessLogStreamerImplTest : public testing::Test {
public:
  struct TestGrpcAccessLogClientFactory : public GrpcAccessLogClientFactory {
    // AccessLog::GrpcAccessLogClientFactory
    GrpcAccessLogClientPtr create() { return GrpcAccessLogClientPtr{async_client_}; }

    Grpc::MockAsyncClient<envoy::api::v2::filter::accesslog::StreamAccessLogsMessage,
                          envoy::api::v2::filter::accesslog::StreamAccessLogsResponse>*

        async_client_{new Grpc::MockAsyncClient<
            envoy::api::v2::filter::accesslog::StreamAccessLogsMessage,
            envoy::api::v2::filter::accesslog::StreamAccessLogsResponse>()};
  };

  typedef Grpc::MockAsyncStream<envoy::api::v2::filter::accesslog::StreamAccessLogsMessage>
      MockAccessLogStream;
  typedef Grpc::AsyncStreamCallbacks<envoy::api::v2::filter::accesslog::StreamAccessLogsResponse>
      AccessLogCallbacks;

  void expectStreamStart(MockAccessLogStream& stream, AccessLogCallbacks** callbacks_to_set) {
    EXPECT_CALL(*factory_->async_client_, start(_, _))
        .WillOnce(Invoke([&stream, callbacks_to_set](const Protobuf::MethodDescriptor&,
                                                     AccessLogCallbacks& callbacks) {
          *callbacks_to_set = &callbacks;
          return &stream;
        }));
  }

  NiceMock<ThreadLocal::MockInstance> tls_;
  LocalInfo::MockLocalInfo local_info_;
  TestGrpcAccessLogClientFactory* factory_{new TestGrpcAccessLogClientFactory};
  GrpcAccessLogStreamerImpl streamer_{GrpcAccessLogClientFactoryPtr{factory_}, tls_, local_info_};
};

// Test basic stream logging flow.
TEST_F(GrpcAccessLogStreamerImplTest, BasicFlow) {
  InSequence s;

  // Start a stream for the first log.
  MockAccessLogStream stream1;
  AccessLogCallbacks* callbacks1;
  expectStreamStart(stream1, &callbacks1);
  EXPECT_CALL(local_info_, node());
  EXPECT_CALL(stream1, sendMessage(_, false));
  envoy::api::v2::filter::accesslog::StreamAccessLogsMessage message_log1;
  streamer_.send(message_log1, "log1");

  message_log1.Clear();
  EXPECT_CALL(stream1, sendMessage(_, false));
  streamer_.send(message_log1, "log1");

  // Start a stream for the second log.
  MockAccessLogStream stream2;
  AccessLogCallbacks* callbacks2;
  expectStreamStart(stream2, &callbacks2);
  EXPECT_CALL(local_info_, node());
  EXPECT_CALL(stream2, sendMessage(_, false));
  envoy::api::v2::filter::accesslog::StreamAccessLogsMessage message_log2;
  streamer_.send(message_log2, "log2");

  // Verify that sending an empty response message doesn't do anything bad.
  callbacks1->onReceiveMessage(
      std::make_unique<envoy::api::v2::filter::accesslog::StreamAccessLogsResponse>());

  // Close stream 2 and make sure we make a new one.
  callbacks2->onRemoteClose(Grpc::Status::Internal, "bad");
  expectStreamStart(stream2, &callbacks2);
  EXPECT_CALL(local_info_, node());
  EXPECT_CALL(stream2, sendMessage(_, false));
  streamer_.send(message_log2, "log2");
}

// Test that stream failure is handled correctly.
TEST_F(GrpcAccessLogStreamerImplTest, StreamFailure) {
  InSequence s;

  EXPECT_CALL(*factory_->async_client_, start(_, _))
      .WillOnce(Invoke([](const Protobuf::MethodDescriptor&, AccessLogCallbacks& callbacks) {
        callbacks.onRemoteClose(Grpc::Status::Internal, "bad");
        return nullptr;
      }));
  EXPECT_CALL(local_info_, node());
  envoy::api::v2::filter::accesslog::StreamAccessLogsMessage message_log1;
  streamer_.send(message_log1, "log1");
}

class MockGrpcAccessLogStreamer : public GrpcAccessLogStreamer {
public:
  // GrpcAccessLogStreamer
  MOCK_METHOD2(send, void(envoy::api::v2::filter::accesslog::StreamAccessLogsMessage& message,
                          const std::string& log_name));
};

class HttpGrpcAccessLogTest : public testing::Test {
public:
  HttpGrpcAccessLogTest() {
    ON_CALL(*filter_, evaluate(_, _)).WillByDefault(Return(true));
    config_.mutable_common_config()->set_log_name("hello_log");
    access_log_.reset(new HttpGrpcAccessLog(FilterPtr{filter_}, config_, streamer_));
  }

  void expectLog(const std::string& expected_request_msg_yaml) {
    envoy::api::v2::filter::accesslog::StreamAccessLogsMessage expected_request_msg;
    MessageUtil::loadFromYaml(expected_request_msg_yaml, expected_request_msg);
    EXPECT_CALL(*streamer_, send(_, "hello_log"))
        .WillOnce(Invoke([expected_request_msg](
                             envoy::api::v2::filter::accesslog::StreamAccessLogsMessage& message,
                             const std::string&) {
          EXPECT_EQ(message.DebugString(), expected_request_msg.DebugString());
        }));
  }

  MockFilter* filter_{new NiceMock<MockFilter>()};
  envoy::api::v2::filter::accesslog::HttpGrpcAccessLogConfig config_;
  std::shared_ptr<MockGrpcAccessLogStreamer> streamer_{new MockGrpcAccessLogStreamer()};
  std::unique_ptr<HttpGrpcAccessLog> access_log_;
};

// Test HTTP log marshalling.
TEST_F(HttpGrpcAccessLogTest, Marshalling) {
  InSequence s;

  {
    NiceMock<RequestInfo::MockRequestInfo> request_info;
    request_info.host_ = nullptr;
    request_info.start_time_ = SystemTime(1h);
    request_info.duration_ = 2ms;
    expectLog(R"EOF(
http_logs:
  log_entry:
    common_properties:
      downstream_remote_address:
        socket_address:
          address: "127.0.0.1"
          port_value: 0
      downstream_local_address:
        socket_address:
          address: "127.0.0.2"
          port_value: 0
      start_time:
        seconds: 3600
      time_to_last_downstream_tx_byte:
        nanos: 2000000
    request: {}
    response: {}

)EOF");
    access_log_->log(nullptr, nullptr, request_info);
  }

  {
    NiceMock<RequestInfo::MockRequestInfo> request_info;
    request_info.start_time_ = SystemTime(1h);
    request_info.request_received_duration_ = 2ms;
    request_info.response_received_duration_ = 4ms;
    request_info.duration_ = 6ms;
    request_info.upstream_local_address_ =
        std::make_shared<Network::Address::Ipv4Instance>("10.0.0.2");
    request_info.protocol_ = Http::Protocol::Http10;
    request_info.bytes_received_ = 10;
    request_info.bytes_sent_ = 20;
    request_info.response_code_.value(200);
    ON_CALL(request_info, getResponseFlag(RequestInfo::ResponseFlag::FaultInjected))
        .WillByDefault(Return(true));

    Http::TestHeaderMapImpl request_headers{
        {":scheme", "scheme_value"},
        {":authority", "authority_value"},
        {":path", "path_value"},
        {"user-agent", "user-agent_value"},
        {"referer", "referer_value"},
        {"x-forwarded-for", "x-forwarded-for_value"},
        {"x-request-id", "x-request-id_value"},
        {"x-envoy-original-path", "x-envoy-original-path_value"},
    };
    Http::TestHeaderMapImpl response_headers{{":status", "200"}};

    expectLog(R"EOF(
http_logs:
  log_entry:
    common_properties:
      downstream_remote_address:
        socket_address:
          address: "127.0.0.1"
          port_value: 0
      downstream_local_address:
        socket_address:
          address: "127.0.0.2"
          port_value: 0
      start_time:
        seconds: 3600
      time_to_last_rx_byte:
        nanos: 2000000
      time_to_first_upstream_rx_byte:
        nanos: 4000000
      time_to_last_downstream_tx_byte:
        nanos: 6000000
      upstream_remote_address:
        socket_address:
          address: "10.0.0.1"
          port_value: 443
      upstream_local_address:
        socket_address:
          address: "10.0.0.2"
          port_value: 0
      upstream_cluster: "fake_cluster"
      response_flags:
        fault_injected: true
    protocol_version: HTTP10
    request:
      scheme: "scheme_value"
      authority: "authority_value"
      path: "path_value"
      user_agent: "user-agent_value"
      referer: "referer_value"
      forwarded_for: "x-forwarded-for_value"
      request_id: "x-request-id_value"
      original_path: "x-envoy-original-path_value"
      request_headers_bytes: 219
      request_body_bytes: 10
    response:
      response_code:
        value: 200
      response_headers_bytes: 10
      response_body_bytes: 20
)EOF");
    access_log_->log(&request_headers, &response_headers, request_info);
  }
}

TEST(responseFlagsToAccessLogResponseFlagsTest, All) {
  NiceMock<RequestInfo::MockRequestInfo> request_info;
  ON_CALL(request_info, getResponseFlag(_)).WillByDefault(Return(true));
  envoy::api::v2::filter::accesslog::AccessLogCommon common_access_log;
  HttpGrpcAccessLog::responseFlagsToAccessLogResponseFlags(common_access_log, request_info);

  envoy::api::v2::filter::accesslog::AccessLogCommon common_access_log_expected;
  common_access_log_expected.mutable_response_flags()->set_failed_local_healthcheck(true);
  common_access_log_expected.mutable_response_flags()->set_no_healthy_upstream(true);
  common_access_log_expected.mutable_response_flags()->set_upstream_request_timeout(true);
  common_access_log_expected.mutable_response_flags()->set_local_reset(true);
  common_access_log_expected.mutable_response_flags()->set_upstream_remote_reset(true);
  common_access_log_expected.mutable_response_flags()->set_upstream_connection_failure(true);
  common_access_log_expected.mutable_response_flags()->set_upstream_connection_termination(true);
  common_access_log_expected.mutable_response_flags()->set_upstream_overflow(true);
  common_access_log_expected.mutable_response_flags()->set_no_route_found(true);
  common_access_log_expected.mutable_response_flags()->set_delay_injected(true);
  common_access_log_expected.mutable_response_flags()->set_fault_injected(true);
  common_access_log_expected.mutable_response_flags()->set_rate_limited(true);

  EXPECT_EQ(common_access_log_expected.DebugString(), common_access_log.DebugString());
}

} // namespace AccessLog
} // namespace Envoy
