#include <chrono>
#include <memory>
#include <string>

#include "common/http/header_map_impl.h"
#include "common/http/headers.h"
#include "common/http/message_impl.h"
#include "common/runtime/runtime_impl.h"
#include "common/runtime/uuid_util.h"
#include "common/tracing/http_tracer_impl.h"
#include "common/tracing/zipkin/zipkin_core_constants.h"
#include "common/tracing/zipkin/zipkin_tracer_impl.h"

#include "test/mocks/http/mocks.h"
#include "test/mocks/local_info/mocks.h"
#include "test/mocks/runtime/mocks.h"
#include "test/mocks/stats/mocks.h"
#include "test/mocks/thread_local/mocks.h"
#include "test/mocks/tracing/mocks.h"
#include "test/mocks/upstream/mocks.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::Invoke;
using testing::NiceMock;
using testing::Return;
using testing::ReturnRef;
using testing::Test;
using testing::_;

namespace Envoy {
namespace Zipkin {

class ZipkinDriverTest : public Test {
public:
  void setup(Json::Object& config, bool init_timer) {
    ON_CALL(cm_, httpAsyncClientForCluster("fake_cluster"))
        .WillByDefault(ReturnRef(cm_.async_client_));

    if (init_timer) {
      timer_ = new NiceMock<Event::MockTimer>(&tls_.dispatcher_);
      EXPECT_CALL(*timer_, enableTimer(std::chrono::milliseconds(5000)));
    }

    driver_.reset(new Driver(config, cm_, stats_, tls_, runtime_, local_info_, random_));
  }

  void setupValidDriver() {
    EXPECT_CALL(cm_, get("fake_cluster")).WillRepeatedly(Return(&cm_.thread_local_cluster_));

    std::string valid_config = R"EOF(
      {
       "collector_cluster": "fake_cluster",
       "collector_endpoint": "/api/v1/spans"
       }
    )EOF";
    Json::ObjectSharedPtr loader = Json::Factory::loadFromString(valid_config);

    setup(*loader, true);
  }

  const std::string operation_name_{"test"};
  Http::TestHeaderMapImpl request_headers_{
      {":authority", "api.lyft.com"}, {":path", "/"}, {":method", "GET"}, {"x-request-id", "foo"}};
  SystemTime start_time_;
  RequestInfo::MockRequestInfo request_info_;

  NiceMock<ThreadLocal::MockInstance> tls_;
  std::unique_ptr<Driver> driver_;
  NiceMock<Event::MockTimer>* timer_;
  Stats::IsolatedStoreImpl stats_;
  NiceMock<Upstream::MockClusterManager> cm_;
  NiceMock<Runtime::MockLoader> runtime_;
  NiceMock<LocalInfo::MockLocalInfo> local_info_;
  NiceMock<Runtime::MockRandomGenerator> random_;

  NiceMock<Tracing::MockConfig> config_;
};

TEST_F(ZipkinDriverTest, InitializeDriver) {
  {
    std::string invalid_config = R"EOF(
      {"fake" : "fake"}
    )EOF";
    Json::ObjectSharedPtr loader = Json::Factory::loadFromString(invalid_config);

    EXPECT_THROW(setup(*loader, false), EnvoyException);
  }

  {
    std::string empty_config = "{}";
    Json::ObjectSharedPtr loader = Json::Factory::loadFromString(empty_config);

    EXPECT_THROW(setup(*loader, false), EnvoyException);
  }

  {
    // Valid config but not valid cluster.
    EXPECT_CALL(cm_, get("fake_cluster")).WillOnce(Return(nullptr));

    std::string valid_config = R"EOF(
      {
       "collector_cluster": "fake_cluster",
       "collector_endpoint": "/api/v1/spans"
       }
    )EOF";
    Json::ObjectSharedPtr loader = Json::Factory::loadFromString(valid_config);

    EXPECT_THROW(setup(*loader, false), EnvoyException);
  }

  {
    // valid config
    EXPECT_CALL(cm_, get("fake_cluster")).WillRepeatedly(Return(&cm_.thread_local_cluster_));
    ON_CALL(*cm_.thread_local_cluster_.cluster_.info_, features()).WillByDefault(Return(0));

    std::string valid_config = R"EOF(
      {
       "collector_cluster": "fake_cluster",
       "collector_endpoint": "/api/v1/spans"
       }
    )EOF";
    Json::ObjectSharedPtr loader = Json::Factory::loadFromString(valid_config);

    setup(*loader, true);
  }
}

TEST_F(ZipkinDriverTest, FlushSeveralSpans) {
  setupValidDriver();

  Http::MockAsyncClientRequest request(&cm_.async_client_);
  Http::AsyncClient::Callbacks* callback;
  const Optional<std::chrono::milliseconds> timeout(std::chrono::seconds(5));

  EXPECT_CALL(cm_.async_client_, send_(_, _, timeout))
      .WillOnce(
          Invoke([&](Http::MessagePtr& message, Http::AsyncClient::Callbacks& callbacks,
                     const Optional<std::chrono::milliseconds>&) -> Http::AsyncClient::Request* {
            callback = &callbacks;

            EXPECT_STREQ("/api/v1/spans", message->headers().Path()->value().c_str());
            EXPECT_STREQ("fake_cluster", message->headers().Host()->value().c_str());
            EXPECT_STREQ("application/json", message->headers().ContentType()->value().c_str());

            return &request;
          }));

  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.zipkin.min_flush_spans", 5))
      .Times(2)
      .WillRepeatedly(Return(2));
  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.zipkin.request_timeout", 5000U))
      .WillOnce(Return(5000U));

  Tracing::SpanPtr first_span =
      driver_->startSpan(config_, request_headers_, operation_name_, start_time_);
  first_span->finishSpan();

  Tracing::SpanPtr second_span =
      driver_->startSpan(config_, request_headers_, operation_name_, start_time_);
  second_span->finishSpan();

  Http::MessagePtr msg(new Http::ResponseMessageImpl(
      Http::HeaderMapPtr{new Http::TestHeaderMapImpl{{":status", "202"}}}));

  callback->onSuccess(std::move(msg));

  EXPECT_EQ(2U, stats_.counter("tracing.zipkin.spans_sent").value());
  EXPECT_EQ(1U, stats_.counter("tracing.zipkin.reports_sent").value());
  EXPECT_EQ(0U, stats_.counter("tracing.zipkin.reports_dropped").value());
  EXPECT_EQ(0U, stats_.counter("tracing.zipkin.reports_failed").value());

  callback->onFailure(Http::AsyncClient::FailureReason::Reset);

  EXPECT_EQ(1U, stats_.counter("tracing.zipkin.reports_failed").value());
}

TEST_F(ZipkinDriverTest, FlushOneSpanReportFailure) {
  setupValidDriver();

  Http::MockAsyncClientRequest request(&cm_.async_client_);
  Http::AsyncClient::Callbacks* callback;
  const Optional<std::chrono::milliseconds> timeout(std::chrono::seconds(5));

  EXPECT_CALL(cm_.async_client_, send_(_, _, timeout))
      .WillOnce(
          Invoke([&](Http::MessagePtr& message, Http::AsyncClient::Callbacks& callbacks,
                     const Optional<std::chrono::milliseconds>&) -> Http::AsyncClient::Request* {
            callback = &callbacks;

            EXPECT_STREQ("/api/v1/spans", message->headers().Path()->value().c_str());
            EXPECT_STREQ("fake_cluster", message->headers().Host()->value().c_str());
            EXPECT_STREQ("application/json", message->headers().ContentType()->value().c_str());

            return &request;
          }));
  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.zipkin.min_flush_spans", 5))
      .WillOnce(Return(1));
  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.zipkin.request_timeout", 5000U))
      .WillOnce(Return(5000U));

  Tracing::SpanPtr span =
      driver_->startSpan(config_, request_headers_, operation_name_, start_time_);
  span->finishSpan();

  Http::MessagePtr msg(new Http::ResponseMessageImpl(
      Http::HeaderMapPtr{new Http::TestHeaderMapImpl{{":status", "404"}}}));

  // AsyncClient can fail with valid HTTP headers
  callback->onSuccess(std::move(msg));

  EXPECT_EQ(1U, stats_.counter("tracing.zipkin.spans_sent").value());
  EXPECT_EQ(0U, stats_.counter("tracing.zipkin.reports_sent").value());
  EXPECT_EQ(1U, stats_.counter("tracing.zipkin.reports_dropped").value());
  EXPECT_EQ(0U, stats_.counter("tracing.zipkin.reports_failed").value());
}

TEST_F(ZipkinDriverTest, FlushSpansTimer) {
  setupValidDriver();

  const Optional<std::chrono::milliseconds> timeout(std::chrono::seconds(5));
  EXPECT_CALL(cm_.async_client_, send_(_, _, timeout));

  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.zipkin.min_flush_spans", 5))
      .WillOnce(Return(5));

  Tracing::SpanPtr span =
      driver_->startSpan(config_, request_headers_, operation_name_, start_time_);
  span->finishSpan();

  // Timer should be re-enabled.
  EXPECT_CALL(*timer_, enableTimer(std::chrono::milliseconds(5000)));
  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.zipkin.request_timeout", 5000U))
      .WillOnce(Return(5000U));
  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.zipkin.flush_interval_ms", 5000U))
      .WillOnce(Return(5000U));

  timer_->callback_();

  EXPECT_EQ(1U, stats_.counter("tracing.zipkin.timer_flushed").value());
  EXPECT_EQ(1U, stats_.counter("tracing.zipkin.spans_sent").value());
}

TEST_F(ZipkinDriverTest, SerializeAndDeserializeContext) {
  setupValidDriver();

  // Supply bogus context, that will be simply ignored.
  const std::string invalid_context = "notvalidcontext";
  request_headers_.insertOtSpanContext().value(invalid_context);
  driver_->startSpan(config_, request_headers_, operation_name_, start_time_);

  std::string injected_ctx = request_headers_.OtSpanContext()->value().c_str();
  EXPECT_FALSE(injected_ctx.empty());

  // Supply empty context.
  request_headers_.removeOtSpanContext();
  Tracing::SpanPtr span =
      driver_->startSpan(config_, request_headers_, operation_name_, start_time_);

  EXPECT_EQ(nullptr, request_headers_.OtSpanContext());
  span->injectContext(request_headers_);

  injected_ctx = request_headers_.OtSpanContext()->value().c_str();
  EXPECT_FALSE(injected_ctx.empty());

  // Supply parent context, request_headers has properly populated x-ot-span-context.
  Tracing::SpanPtr span_with_parent =
      driver_->startSpan(config_, request_headers_, operation_name_, start_time_);
  injected_ctx = request_headers_.OtSpanContext()->value().c_str();
  EXPECT_FALSE(injected_ctx.empty());
}

TEST_F(ZipkinDriverTest, ZipkinSpanTest) {
  setupValidDriver();

  // ====
  // Test effective setTag()
  // ====

  request_headers_.removeOtSpanContext();

  // New span will have a CS annotation
  Tracing::SpanPtr span =
      driver_->startSpan(config_, request_headers_, operation_name_, start_time_);

  ZipkinSpanPtr zipkin_span(dynamic_cast<ZipkinSpan*>(span.release()));
  zipkin_span->setTag("key", "value");

  Span& zipkin_zipkin_span = zipkin_span->span();
  EXPECT_EQ(1ULL, zipkin_zipkin_span.binaryAnnotations().size());
  EXPECT_EQ("key", zipkin_zipkin_span.binaryAnnotations()[0].key());
  EXPECT_EQ("value", zipkin_zipkin_span.binaryAnnotations()[0].value());

  // ====
  // Test setTag() with SR annotated span
  // ====

  const std::string trace_id = Hex::uint64ToHex(Util::generateRandom64());
  const std::string span_id = Hex::uint64ToHex(Util::generateRandom64());
  const std::string parent_id = Hex::uint64ToHex(Util::generateRandom64());
  const std::string context =
      trace_id + ";" + span_id + ";" + parent_id + ";" + ZipkinCoreConstants::get().CLIENT_SEND;

  request_headers_.insertOtSpanContext().value(context);

  // New span will have an SR annotation
  Tracing::SpanPtr span2 =
      driver_->startSpan(config_, request_headers_, operation_name_, start_time_);

  ZipkinSpanPtr zipkin_span2(dynamic_cast<ZipkinSpan*>(span2.release()));
  zipkin_span2->setTag("key2", "value2");

  Span& zipkin_zipkin_span2 = zipkin_span2->span();
  EXPECT_EQ(1ULL, zipkin_zipkin_span2.binaryAnnotations().size());
  EXPECT_EQ("key2", zipkin_zipkin_span2.binaryAnnotations()[0].key());
  EXPECT_EQ("value2", zipkin_zipkin_span2.binaryAnnotations()[0].value());

  // ====
  // Test setTag() with empty annotations vector
  // ====
  Tracing::SpanPtr span3 =
      driver_->startSpan(config_, request_headers_, operation_name_, start_time_);
  ZipkinSpanPtr zipkin_span3(dynamic_cast<ZipkinSpan*>(span3.release()));
  Span& zipkin_zipkin_span3 = zipkin_span3->span();

  std::vector<Annotation> annotations;
  zipkin_zipkin_span3.setAnnotations(annotations);

  zipkin_span3->setTag("key3", "value3");
  EXPECT_EQ(1ULL, zipkin_zipkin_span3.binaryAnnotations().size());
  EXPECT_EQ("key3", zipkin_zipkin_span3.binaryAnnotations()[0].key());
  EXPECT_EQ("value3", zipkin_zipkin_span3.binaryAnnotations()[0].value());
}

TEST_F(ZipkinDriverTest, ZipkinSpanContextFromOtSpanContextHeaderTest) {
  setupValidDriver();

  const std::string trace_id = Hex::uint64ToHex(Util::generateRandom64());
  const std::string span_id = Hex::uint64ToHex(Util::generateRandom64());
  const std::string parent_id = Hex::uint64ToHex(Util::generateRandom64());
  const std::string context =
      trace_id + ";" + span_id + ";" + parent_id + ";" + ZipkinCoreConstants::get().CLIENT_SEND;

  request_headers_.insertOtSpanContext().value(context);

  // New span will have an SR annotation - so its span and parent ids will be
  // the same as the supplied span context (i.e. shared context)
  Tracing::SpanPtr span =
      driver_->startSpan(config_, request_headers_, operation_name_, start_time_);

  ZipkinSpanPtr zipkin_span(dynamic_cast<ZipkinSpan*>(span.release()));

  EXPECT_EQ(trace_id, zipkin_span->span().traceIdAsHexString());
  EXPECT_EQ(span_id, zipkin_span->span().idAsHexString());
  EXPECT_EQ(parent_id, zipkin_span->span().parentIdAsHexString());
}

TEST_F(ZipkinDriverTest, ZipkinSpanContextFromB3HeadersTest) {
  setupValidDriver();

  const std::string trace_id = Hex::uint64ToHex(Util::generateRandom64());
  const std::string span_id = Hex::uint64ToHex(Util::generateRandom64());
  const std::string parent_id = Hex::uint64ToHex(Util::generateRandom64());

  request_headers_.insertXB3TraceId().value(trace_id);
  request_headers_.insertXB3SpanId().value(span_id);
  request_headers_.insertXB3ParentSpanId().value(parent_id);

  // New span will have an SR annotation - so its span and parent ids will be
  // the same as the supplied span context (i.e. shared context)
  Tracing::SpanPtr span =
      driver_->startSpan(config_, request_headers_, operation_name_, start_time_);

  ZipkinSpanPtr zipkin_span(dynamic_cast<ZipkinSpan*>(span.release()));

  EXPECT_EQ(trace_id, zipkin_span->span().traceIdAsHexString());
  EXPECT_EQ(span_id, zipkin_span->span().idAsHexString());
  EXPECT_EQ(parent_id, zipkin_span->span().parentIdAsHexString());
}

TEST_F(ZipkinDriverTest, ZipkinSpanContextFromInvalidTraceIdB3HeadersTest) {
  setupValidDriver();

  request_headers_.insertXB3TraceId().value(std::string("xyz"));
  request_headers_.insertXB3SpanId().value(Hex::uint64ToHex(Util::generateRandom64()));
  request_headers_.insertXB3ParentSpanId().value(Hex::uint64ToHex(Util::generateRandom64()));

  Tracing::SpanPtr span =
      driver_->startSpan(config_, request_headers_, operation_name_, start_time_);
  EXPECT_NE(nullptr, dynamic_cast<Tracing::NullSpan*>(span.get()));
}

TEST_F(ZipkinDriverTest, ZipkinSpanContextFromInvalidSpanIdB3HeadersTest) {
  setupValidDriver();

  request_headers_.insertXB3TraceId().value(Hex::uint64ToHex(Util::generateRandom64()));
  request_headers_.insertXB3SpanId().value(std::string("xyz"));
  request_headers_.insertXB3ParentSpanId().value(Hex::uint64ToHex(Util::generateRandom64()));

  Tracing::SpanPtr span =
      driver_->startSpan(config_, request_headers_, operation_name_, start_time_);
  EXPECT_NE(nullptr, dynamic_cast<Tracing::NullSpan*>(span.get()));
}

TEST_F(ZipkinDriverTest, ZipkinSpanContextFromInvalidParentIdB3HeadersTest) {
  setupValidDriver();

  request_headers_.insertXB3TraceId().value(Hex::uint64ToHex(Util::generateRandom64()));
  request_headers_.insertXB3SpanId().value(Hex::uint64ToHex(Util::generateRandom64()));
  request_headers_.insertXB3ParentSpanId().value(std::string("xyz"));

  Tracing::SpanPtr span =
      driver_->startSpan(config_, request_headers_, operation_name_, start_time_);
  EXPECT_NE(nullptr, dynamic_cast<Tracing::NullSpan*>(span.get()));
}
} // namespace Zipkin
} // namespace Envoy
