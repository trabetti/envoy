#include "common/router/router.h"

#include <chrono>
#include <cstdint>
#include <string>

#include "envoy/event/dispatcher.h"
#include "envoy/event/timer.h"
#include "envoy/grpc/status.h"
#include "envoy/http/conn_pool.h"
#include "envoy/runtime/runtime.h"
#include "envoy/stats/stats.h"
#include "envoy/upstream/cluster_manager.h"
#include "envoy/upstream/upstream.h"

#include "common/common/assert.h"
#include "common/common/empty_string.h"
#include "common/common/enum_to_int.h"
#include "common/common/utility.h"
#include "common/grpc/common.h"
#include "common/http/codes.h"
#include "common/http/header_map_impl.h"
#include "common/http/headers.h"
#include "common/http/message_impl.h"
#include "common/http/utility.h"
#include "common/router/config_impl.h"
#include "common/router/retry_state_impl.h"
#include "common/tracing/http_tracer_impl.h"

namespace Envoy {
namespace Router {
namespace {
uint32_t getLength(const Buffer::Instance* instance) { return instance ? instance->length() : 0; }
} // namespace

void FilterUtility::setUpstreamScheme(Http::HeaderMap& headers,
                                      const Upstream::ClusterInfo& cluster) {
  if (cluster.transportSocketFactory().implementsSecureTransport()) {
    headers.insertScheme().value().setReference(Http::Headers::get().SchemeValues.Https);
  } else {
    headers.insertScheme().value().setReference(Http::Headers::get().SchemeValues.Http);
  }
}

bool FilterUtility::shouldShadow(const ShadowPolicy& policy, Runtime::Loader& runtime,
                                 uint64_t stable_random) {
  if (policy.cluster().empty()) {
    return false;
  }

  if (!policy.runtimeKey().empty() &&
      !runtime.snapshot().featureEnabled(policy.runtimeKey(), 0, stable_random, 10000UL)) {
    return false;
  }

  return true;
}

FilterUtility::TimeoutData FilterUtility::finalTimeout(const RouteEntry& route,
                                                       Http::HeaderMap& request_headers) {
  // See if there is a user supplied timeout in a request header. If there is we take that,
  // otherwise we use the default.
  TimeoutData timeout;
  timeout.global_timeout_ = route.timeout();
  timeout.per_try_timeout_ = route.retryPolicy().perTryTimeout();
  Http::HeaderEntry* header_timeout_entry = request_headers.EnvoyUpstreamRequestTimeoutMs();
  uint64_t header_timeout;
  if (header_timeout_entry) {
    if (StringUtil::atoul(header_timeout_entry->value().c_str(), header_timeout)) {
      timeout.global_timeout_ = std::chrono::milliseconds(header_timeout);
    }
    request_headers.removeEnvoyUpstreamRequestTimeoutMs();
  }

  // See if there is a per try/retry timeout. If it's >= global we just ignore it.
  Http::HeaderEntry* per_try_timeout_entry = request_headers.EnvoyUpstreamRequestPerTryTimeoutMs();
  if (per_try_timeout_entry) {
    if (StringUtil::atoul(per_try_timeout_entry->value().c_str(), header_timeout)) {
      timeout.per_try_timeout_ = std::chrono::milliseconds(header_timeout);
    }
    request_headers.removeEnvoyUpstreamRequestPerTryTimeoutMs();
  }

  if (timeout.per_try_timeout_ >= timeout.global_timeout_) {
    timeout.per_try_timeout_ = std::chrono::milliseconds(0);
  }

  // See if there is any timeout to write in the expected timeout header.
  uint64_t expected_timeout = timeout.per_try_timeout_.count();
  if (expected_timeout == 0) {
    expected_timeout = timeout.global_timeout_.count();
  }

  if (expected_timeout > 0) {
    request_headers.insertEnvoyExpectedRequestTimeoutMs().value(expected_timeout);
  }

  return timeout;
}

Filter::~Filter() {
  // Upstream resources should already have been cleaned.
  ASSERT(!upstream_request_);
  ASSERT(!retry_state_);
}

const std::string Filter::upstreamZone(Upstream::HostDescriptionConstSharedPtr upstream_host) {
  // TODO(PiotrSikora): Switch back to std::string& when string == std::string.
  return upstream_host ? upstream_host->locality().zone() : "";
}

void Filter::chargeUpstreamCode(uint64_t response_status_code,
                                const Http::HeaderMap& response_headers,
                                Upstream::HostDescriptionConstSharedPtr upstream_host,
                                bool dropped) {
  // Passing the response_status_code explicitly is an optimization to avoid
  // multiple calls to slow Http::Utility::getResponseStatus.
  ASSERT(response_status_code == Http::Utility::getResponseStatus(response_headers));
  if (config_.emit_dynamic_stats_ && !callbacks_->requestInfo().healthCheck()) {
    const Http::HeaderEntry* upstream_canary_header = response_headers.EnvoyUpstreamCanary();
    const Http::HeaderEntry* internal_request_header = downstream_headers_->EnvoyInternalRequest();

    const bool is_canary = (upstream_canary_header && upstream_canary_header->value() == "true") ||
                           (upstream_host ? upstream_host->canary() : false);
    const bool internal_request =
        internal_request_header && internal_request_header->value() == "true";

    // TODO(mattklein123): Remove copy when G string compat issues are fixed.
    const std::string zone_name = config_.local_info_.zoneName();
    const std::string upstream_zone = upstreamZone(upstream_host);

    Http::CodeUtility::ResponseStatInfo info{config_.scope_,
                                             cluster_->statsScope(),
                                             EMPTY_STRING,
                                             response_status_code,
                                             internal_request,
                                             route_entry_->virtualHost().name(),
                                             request_vcluster_ ? request_vcluster_->name()
                                                               : EMPTY_STRING,
                                             zone_name,
                                             upstream_zone,
                                             is_canary};

    Http::CodeUtility::chargeResponseStat(info);

    if (!alt_stat_prefix_.empty()) {
      Http::CodeUtility::ResponseStatInfo info{config_.scope_,   cluster_->statsScope(),
                                               alt_stat_prefix_, response_status_code,
                                               internal_request, EMPTY_STRING,
                                               EMPTY_STRING,     zone_name,
                                               upstream_zone,    is_canary};

      Http::CodeUtility::chargeResponseStat(info);
    }

    if (dropped) {
      cluster_->loadReportStats().upstream_rq_dropped_.inc();
    }
    if (upstream_host && Http::CodeUtility::is5xx(response_status_code)) {
      upstream_host->stats().rq_error_.inc();
    }
  }
}

void Filter::chargeUpstreamCode(Http::Code code,
                                Upstream::HostDescriptionConstSharedPtr upstream_host,
                                bool dropped) {
  const uint64_t response_status_code = enumToInt(code);
  Http::HeaderMapImpl fake_response_headers{
      {Http::Headers::get().Status, std::to_string(response_status_code)}};
  chargeUpstreamCode(response_status_code, fake_response_headers, upstream_host, dropped);
}

void Filter::sendLocalReply(Http::Code code, const std::string& body, bool overloaded) {
  // This is a customized version of send local reply that allows us to set the overloaded
  // header.
  Http::Utility::sendLocalReply(
      [this, overloaded](Http::HeaderMapPtr&& headers, bool end_stream) -> void {
        if (overloaded) {
          headers->insertEnvoyOverloaded().value(Http::Headers::get().EnvoyOverloadedValues.True);
        }

        callbacks_->encodeHeaders(std::move(headers), end_stream);
      },
      [this](Buffer::Instance& data, bool end_stream) -> void {
        callbacks_->encodeData(data, end_stream);
      },
      stream_destroyed_, code, body);
}

Http::FilterHeadersStatus Filter::decodeHeaders(Http::HeaderMap& headers, bool end_stream) {
  downstream_headers_ = &headers;

  // Only increment rq total stat if we actually decode headers here. This does not count requests
  // that get handled by earlier filters.
  config_.stats_.rq_total_.inc();

  // Determine if there is a route entry or a redirect for the request.
  route_ = callbacks_->route();
  if (!route_) {
    config_.stats_.no_route_.inc();
    ENVOY_STREAM_LOG(debug, "no cluster match for URL '{}'", *callbacks_,
                     headers.Path()->value().c_str());

    callbacks_->requestInfo().setResponseFlag(RequestInfo::ResponseFlag::NoRouteFound);
    Http::HeaderMapPtr response_headers{new Http::HeaderMapImpl{
        {Http::Headers::get().Status, std::to_string(enumToInt(Http::Code::NotFound))}}};
    callbacks_->encodeHeaders(std::move(response_headers), true);
    return Http::FilterHeadersStatus::StopIteration;
  }

  // Determine if there is a redirect for the request.
  if (route_->redirectEntry()) {
    config_.stats_.rq_redirect_.inc();
    Http::Utility::sendRedirect(*callbacks_, route_->redirectEntry()->newPath(headers),
                                route_->redirectEntry()->redirectResponseCode());
    return Http::FilterHeadersStatus::StopIteration;
  }

  // A route entry matches for the request.
  route_entry_ = route_->routeEntry();
  Upstream::ThreadLocalCluster* cluster = config_.cm_.get(route_entry_->clusterName());
  if (!cluster) {
    config_.stats_.no_cluster_.inc();
    ENVOY_STREAM_LOG(debug, "unknown cluster '{}'", *callbacks_, route_entry_->clusterName());

    callbacks_->requestInfo().setResponseFlag(RequestInfo::ResponseFlag::NoRouteFound);
    Http::HeaderMapPtr response_headers{new Http::HeaderMapImpl{
        {Http::Headers::get().Status,
         std::to_string(enumToInt(route_entry_->clusterNotFoundResponseCode()))}}};
    callbacks_->encodeHeaders(std::move(response_headers), true);
    return Http::FilterHeadersStatus::StopIteration;
  }
  cluster_ = cluster->info();

  // Set up stat prefixes, etc.
  request_vcluster_ = route_entry_->virtualCluster(headers);
  ENVOY_STREAM_LOG(debug, "cluster '{}' match for URL '{}'", *callbacks_,
                   route_entry_->clusterName(), headers.Path()->value().c_str());

  const Http::HeaderEntry* request_alt_name = headers.EnvoyUpstreamAltStatName();
  if (request_alt_name) {
    alt_stat_prefix_ = std::string(request_alt_name->value().c_str()) + ".";
    headers.removeEnvoyUpstreamAltStatName();
  }

  // See if we are supposed to immediately kill some percentage of this cluster's traffic.
  if (cluster_->maintenanceMode()) {
    callbacks_->requestInfo().setResponseFlag(RequestInfo::ResponseFlag::UpstreamOverflow);
    chargeUpstreamCode(Http::Code::ServiceUnavailable, nullptr, true);
    sendLocalReply(Http::Code::ServiceUnavailable, "maintenance mode", true);
    cluster_->stats().upstream_rq_maintenance_mode_.inc();
    return Http::FilterHeadersStatus::StopIteration;
  }

  // Fetch a connection pool for the upstream cluster.
  Http::ConnectionPool::Instance* conn_pool = getConnPool();
  if (!conn_pool) {
    sendNoHealthyUpstreamResponse();
    return Http::FilterHeadersStatus::StopIteration;
  }

  timeout_ = FilterUtility::finalTimeout(*route_entry_, headers);

  // If this header is set with any value, use an alternate response code on timeout
  if (headers.EnvoyUpstreamRequestTimeoutAltResponse()) {
    timeout_response_code_ = Http::Code::NoContent;
    headers.removeEnvoyUpstreamRequestTimeoutAltResponse();
  }

  route_entry_->finalizeRequestHeaders(headers, callbacks_->requestInfo());
  FilterUtility::setUpstreamScheme(headers, *cluster_);
  retry_state_ =
      createRetryState(route_entry_->retryPolicy(), headers, *cluster_, config_.runtime_,
                       config_.random_, callbacks_->dispatcher(), route_entry_->priority());
  do_shadowing_ = FilterUtility::shouldShadow(route_entry_->shadowPolicy(), config_.runtime_,
                                              callbacks_->streamId());

#ifndef NVLOG
  headers.iterate(
      [](const Http::HeaderEntry& header, void* context) -> Http::HeaderMap::Iterate {
        ENVOY_STREAM_LOG(debug, "  '{}':'{}'",
                         *static_cast<Http::StreamDecoderFilterCallbacks*>(context),
                         header.key().c_str(), header.value().c_str());
        return Http::HeaderMap::Iterate::Continue;
      },
      callbacks_);
#endif

  // Do a common header check. We make sure that all outgoing requests have all HTTP/2 headers.
  // These get stripped by HTTP/1 codec where applicable.
  ASSERT(headers.Scheme());
  ASSERT(headers.Method());
  ASSERT(headers.Host());
  ASSERT(headers.Path());

  grpc_request_ = Grpc::Common::hasGrpcContentType(headers);
  upstream_request_.reset(new UpstreamRequest(*this, *conn_pool));
  upstream_request_->encodeHeaders(end_stream);
  if (end_stream) {
    onRequestComplete();
  }

  return Http::FilterHeadersStatus::StopIteration;
}

Http::ConnectionPool::Instance* Filter::getConnPool() {
  return config_.cm_.httpConnPoolForCluster(route_entry_->clusterName(), route_entry_->priority(),
                                            this);
}

void Filter::sendNoHealthyUpstreamResponse() {
  callbacks_->requestInfo().setResponseFlag(RequestInfo::ResponseFlag::NoHealthyUpstream);
  chargeUpstreamCode(Http::Code::ServiceUnavailable, nullptr, false);
  sendLocalReply(Http::Code::ServiceUnavailable, "no healthy upstream", false);
}

Http::FilterDataStatus Filter::decodeData(Buffer::Instance& data, bool end_stream) {
  bool buffering = (retry_state_ && retry_state_->enabled()) || do_shadowing_;
  if (buffering && buffer_limit_ > 0 &&
      getLength(callbacks_->decodingBuffer()) + data.length() > buffer_limit_) {
    // The request is larger than we should buffer. Give up on the retry/shadow
    cluster_->stats().retry_or_shadow_abandoned_.inc();
    retry_state_.reset();
    buffering = false;
    do_shadowing_ = false;
  }

  // If we are going to buffer for retries or shadowing, we need to make a copy before encoding
  // since it's all moves from here on.
  if (buffering) {
    Buffer::OwnedImpl copy(data);
    upstream_request_->encodeData(copy, end_stream);
  } else {
    upstream_request_->encodeData(data, end_stream);
  }

  if (end_stream) {
    onRequestComplete();
  }

  // If we are potentially going to retry or shadow this request we need to buffer.
  // This will not cause the connection manager to 413 because before we hit the
  // buffer limit we give up on retries and buffering.
  return buffering ? Http::FilterDataStatus::StopIterationAndBuffer
                   : Http::FilterDataStatus::StopIterationNoBuffer;
}

Http::FilterTrailersStatus Filter::decodeTrailers(Http::HeaderMap& trailers) {
  downstream_trailers_ = &trailers;
  upstream_request_->encodeTrailers(trailers);
  onRequestComplete();
  return Http::FilterTrailersStatus::StopIteration;
}

void Filter::setDecoderFilterCallbacks(Http::StreamDecoderFilterCallbacks& callbacks) {
  callbacks_ = &callbacks;
  // As the decoder filter only pushes back via watermarks once data has reached
  // it, it can latch the current buffer limit and does not need to update the
  // limit if another filter increases it.
  buffer_limit_ = callbacks_->decoderBufferLimit();
}

void Filter::cleanup() {
  upstream_request_.reset();
  retry_state_.reset();
  if (response_timeout_) {
    response_timeout_->disableTimer();
    response_timeout_.reset();
  }
}

void Filter::maybeDoShadowing() {
  if (!do_shadowing_) {
    return;
  }

  ASSERT(!route_entry_->shadowPolicy().cluster().empty());
  Http::MessagePtr request(new Http::RequestMessageImpl(
      Http::HeaderMapPtr{new Http::HeaderMapImpl(*downstream_headers_)}));
  if (callbacks_->decodingBuffer()) {
    request->body().reset(new Buffer::OwnedImpl(*callbacks_->decodingBuffer()));
  }
  if (downstream_trailers_) {
    request->trailers(Http::HeaderMapPtr{new Http::HeaderMapImpl(*downstream_trailers_)});
  }

  config_.shadowWriter().shadow(route_entry_->shadowPolicy().cluster(), std::move(request),
                                timeout_.global_timeout_);
}

void Filter::onRequestComplete() {
  downstream_end_stream_ = true;
  downstream_request_complete_time_ = std::chrono::steady_clock::now();
  callbacks_->requestInfo().requestReceivedDuration(downstream_request_complete_time_);

  // Possible that we got an immediate reset.
  if (upstream_request_) {
    // Nominally how long it took to send the request.
    upstream_request_->request_info_.requestReceivedDuration(downstream_request_complete_time_);

    // Even if we got an immediate reset, we could still shadow, but that is a riskier change and
    // seems unnecessary right now.
    maybeDoShadowing();

    upstream_request_->setupPerTryTimeout();
    if (timeout_.global_timeout_.count() > 0) {
      response_timeout_ =
          callbacks_->dispatcher().createTimer([this]() -> void { onResponseTimeout(); });
      response_timeout_->enableTimer(timeout_.global_timeout_);
    }
  }
}

void Filter::onDestroy() {
  if (upstream_request_) {
    upstream_request_->resetStream();
  }
  stream_destroyed_ = true;
  cleanup();
}

void Filter::onResponseTimeout() {
  ENVOY_STREAM_LOG(debug, "upstream timeout", *callbacks_);
  cluster_->stats().upstream_rq_timeout_.inc();

  // It's possible to timeout during a retry backoff delay when we have no upstream request. In
  // this case we fake a reset since onUpstreamReset() doesn't care.
  if (upstream_request_) {
    if (upstream_request_->upstream_host_) {
      upstream_request_->upstream_host_->stats().rq_timeout_.inc();
    }
    upstream_request_->resetStream();
  }

  onUpstreamReset(UpstreamResetType::GlobalTimeout, Optional<Http::StreamResetReason>());
}

void Filter::onUpstreamReset(UpstreamResetType type,
                             const Optional<Http::StreamResetReason>& reset_reason) {
  ASSERT(type == UpstreamResetType::GlobalTimeout || upstream_request_);
  if (type == UpstreamResetType::Reset) {
    ENVOY_STREAM_LOG(debug, "upstream reset", *callbacks_);
  }

  Upstream::HostDescriptionConstSharedPtr upstream_host;
  if (upstream_request_) {
    upstream_host = upstream_request_->upstream_host_;
    if (upstream_host) {
      upstream_host->outlierDetector().putHttpResponseCode(
          enumToInt(type == UpstreamResetType::Reset ? Http::Code::ServiceUnavailable
                                                     : timeout_response_code_));
    }
  }

  // We don't retry on a global timeout or if we already started the response.
  if (type != UpstreamResetType::GlobalTimeout && !downstream_response_started_ && retry_state_) {
    RetryStatus retry_status =
        retry_state_->shouldRetry(nullptr, reset_reason, [this]() -> void { doRetry(); });
    if (retry_status == RetryStatus::Yes && setupRetry(true)) {
      if (upstream_host) {
        upstream_host->stats().rq_error_.inc();
      }
      return;
    } else if (retry_status == RetryStatus::NoOverflow) {
      callbacks_->requestInfo().setResponseFlag(RequestInfo::ResponseFlag::UpstreamOverflow);
    }
  }

  // If we have not yet sent anything downstream, send a response with an appropriate status code.
  // Otherwise just reset the ongoing response.
  if (downstream_response_started_) {
    if (upstream_request_ != nullptr && upstream_request_->grpc_rq_success_deferred_) {
      upstream_request_->upstream_host_->stats().rq_error_.inc();
    }
    // This will destroy any created retry timers.
    cleanup();
    callbacks_->resetStream();
  } else {
    // This will destroy any created retry timers.
    cleanup();
    Http::Code code;
    const char* body;
    if (type == UpstreamResetType::GlobalTimeout || type == UpstreamResetType::PerTryTimeout) {
      callbacks_->requestInfo().setResponseFlag(RequestInfo::ResponseFlag::UpstreamRequestTimeout);

      code = timeout_response_code_;
      body = code == Http::Code::GatewayTimeout ? "upstream request timeout" : "";
    } else {
      RequestInfo::ResponseFlag response_flags =
          streamResetReasonToResponseFlag(reset_reason.value());
      callbacks_->requestInfo().setResponseFlag(response_flags);
      code = Http::Code::ServiceUnavailable;
      body = "upstream connect error or disconnect/reset before headers";
    }

    const bool dropped =
        reset_reason.valid() && reset_reason.value() == Http::StreamResetReason::Overflow;
    chargeUpstreamCode(code, upstream_host, dropped);
    // If we had non-5xx but still have been reset by backend or timeout before
    // starting response, we treat this as an error. We only get non-5xx when
    // timeout_response_code_ is used for code above, where this member can
    // assume values such as 204 (NoContent).
    if (upstream_host != nullptr && !Http::CodeUtility::is5xx(enumToInt(code))) {
      upstream_host->stats().rq_error_.inc();
    }
    sendLocalReply(code, body, dropped);
  }
}

RequestInfo::ResponseFlag
Filter::streamResetReasonToResponseFlag(Http::StreamResetReason reset_reason) {
  switch (reset_reason) {
  case Http::StreamResetReason::ConnectionFailure:
    return RequestInfo::ResponseFlag::UpstreamConnectionFailure;
  case Http::StreamResetReason::ConnectionTermination:
    return RequestInfo::ResponseFlag::UpstreamConnectionTermination;
  case Http::StreamResetReason::LocalReset:
  case Http::StreamResetReason::LocalRefusedStreamReset:
    return RequestInfo::ResponseFlag::LocalReset;
  case Http::StreamResetReason::Overflow:
    return RequestInfo::ResponseFlag::UpstreamOverflow;
  case Http::StreamResetReason::RemoteReset:
  case Http::StreamResetReason::RemoteRefusedStreamReset:
    return RequestInfo::ResponseFlag::UpstreamRemoteReset;
  }

  throw std::invalid_argument("Unknown reset_reason");
}

void Filter::handleNon5xxResponseHeaders(const Http::HeaderMap& headers, bool end_stream) {
  // We need to defer gRPC success until after we have processed grpc-status in
  // the trailers.
  if (grpc_request_) {
    if (end_stream) {
      Optional<Grpc::Status::GrpcStatus> grpc_status = Grpc::Common::getGrpcStatus(headers);
      if (grpc_status.valid() &&
          !Http::CodeUtility::is5xx(Grpc::Common::grpcToHttpStatus(grpc_status.value()))) {
        upstream_request_->upstream_host_->stats().rq_success_.inc();
      } else {
        upstream_request_->upstream_host_->stats().rq_error_.inc();
      }
    } else {
      upstream_request_->grpc_rq_success_deferred_ = true;
    }
  } else {
    upstream_request_->upstream_host_->stats().rq_success_.inc();
  }
}

void Filter::onUpstreamHeaders(const uint64_t response_code, Http::HeaderMapPtr&& headers,
                               bool end_stream) {
  ENVOY_STREAM_LOG(debug, "upstream headers complete: end_stream={}", *callbacks_, end_stream);
  ASSERT(!downstream_response_started_);

  upstream_request_->upstream_host_->outlierDetector().putHttpResponseCode(response_code);

  if (headers->EnvoyImmediateHealthCheckFail() != nullptr) {
    upstream_request_->upstream_host_->healthChecker().setUnhealthy();
  }

  if (retry_state_) {
    RetryStatus retry_status = retry_state_->shouldRetry(
        headers.get(), Optional<Http::StreamResetReason>(), [this]() -> void { doRetry(); });
    // Capture upstream_host since setupRetry() in the following line will clear
    // upstream_request_.
    const auto upstream_host = upstream_request_->upstream_host_;
    if (retry_status == RetryStatus::Yes && setupRetry(end_stream)) {
      Http::CodeUtility::chargeBasicResponseStat(cluster_->statsScope(), "retry.",
                                                 static_cast<Http::Code>(response_code));
      upstream_host->stats().rq_error_.inc();
      return;
    } else if (retry_status == RetryStatus::NoOverflow) {
      callbacks_->requestInfo().setResponseFlag(RequestInfo::ResponseFlag::UpstreamOverflow);
    }

    // Make sure any retry timers are destroyed since we may not call cleanup() if end_stream is
    // false.
    retry_state_.reset();
  }

  // Only send upstream service time if we received the complete request and this is not a
  // premature response.
  if (DateUtil::timePointValid(downstream_request_complete_time_)) {
    MonotonicTime response_received_time = std::chrono::steady_clock::now();
    std::chrono::milliseconds ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        response_received_time - downstream_request_complete_time_);
    headers->insertEnvoyUpstreamServiceTime().value(ms.count());
    callbacks_->requestInfo().responseReceivedDuration(response_received_time);
    upstream_request_->request_info_.responseReceivedDuration(response_received_time);
  }

  upstream_request_->upstream_canary_ =
      (headers->EnvoyUpstreamCanary() && headers->EnvoyUpstreamCanary()->value() == "true") ||
      upstream_request_->upstream_host_->canary();
  chargeUpstreamCode(response_code, *headers, upstream_request_->upstream_host_, false);
  if (!Http::CodeUtility::is5xx(response_code)) {
    handleNon5xxResponseHeaders(*headers, end_stream);
  }

  // Append routing cookies
  for (const auto& header_value : downstream_set_cookies_) {
    headers->addReferenceKey(Http::Headers::get().SetCookie, header_value);
  }

  // TODO(zuercher): If access to response_headers_to_add (at any level) is ever needed outside
  // Router::Filter we'll need to find a better location for this work. One possibility is to
  // provide finalizeResponseHeaders functions on the Router::Config and VirtualHost interfaces.
  route_entry_->finalizeResponseHeaders(*headers, callbacks_->requestInfo());

  downstream_response_started_ = true;
  if (end_stream) {
    onUpstreamComplete();
  }

  callbacks_->encodeHeaders(std::move(headers), end_stream);
}

void Filter::onUpstreamData(Buffer::Instance& data, bool end_stream) {
  if (end_stream) {
    // gRPC request termination without trailers is an error.
    if (upstream_request_->grpc_rq_success_deferred_) {
      upstream_request_->upstream_host_->stats().rq_error_.inc();
    }
    onUpstreamComplete();
  }

  callbacks_->encodeData(data, end_stream);
}

void Filter::onUpstreamTrailers(Http::HeaderMapPtr&& trailers) {
  if (upstream_request_->grpc_rq_success_deferred_) {
    Optional<Grpc::Status::GrpcStatus> grpc_status = Grpc::Common::getGrpcStatus(*trailers);
    if (grpc_status.valid() &&
        !Http::CodeUtility::is5xx(Grpc::Common::grpcToHttpStatus(grpc_status.value()))) {
      upstream_request_->upstream_host_->stats().rq_success_.inc();
    } else {
      upstream_request_->upstream_host_->stats().rq_error_.inc();
    }
  }
  onUpstreamComplete();
  callbacks_->encodeTrailers(std::move(trailers));
}

void Filter::onUpstreamComplete() {
  if (!downstream_end_stream_) {
    upstream_request_->resetStream();
  }

  if (config_.emit_dynamic_stats_ && !callbacks_->requestInfo().healthCheck() &&
      DateUtil::timePointValid(downstream_request_complete_time_)) {
    std::chrono::milliseconds response_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - downstream_request_complete_time_);

    upstream_request_->upstream_host_->outlierDetector().putResponseTime(response_time);

    const Http::HeaderEntry* internal_request_header = downstream_headers_->EnvoyInternalRequest();
    const bool internal_request =
        internal_request_header && internal_request_header->value() == "true";

    // TODO(mattklein123): Remove copy when G string compat issues are fixed.
    const std::string zone_name = config_.local_info_.zoneName();

    Http::CodeUtility::ResponseTimingInfo info{config_.scope_,
                                               cluster_->statsScope(),
                                               EMPTY_STRING,
                                               response_time,
                                               upstream_request_->upstream_canary_,
                                               internal_request,
                                               route_entry_->virtualHost().name(),
                                               request_vcluster_ ? request_vcluster_->name()
                                                                 : EMPTY_STRING,
                                               zone_name,
                                               upstreamZone(upstream_request_->upstream_host_)};

    Http::CodeUtility::chargeResponseTiming(info);

    if (!alt_stat_prefix_.empty()) {
      Http::CodeUtility::ResponseTimingInfo info{config_.scope_,
                                                 cluster_->statsScope(),
                                                 alt_stat_prefix_,
                                                 response_time,
                                                 upstream_request_->upstream_canary_,
                                                 internal_request,
                                                 EMPTY_STRING,
                                                 EMPTY_STRING,
                                                 zone_name,
                                                 upstreamZone(upstream_request_->upstream_host_)};

      Http::CodeUtility::chargeResponseTiming(info);
    }
  }

  cleanup();
}

bool Filter::setupRetry(bool end_stream) {
  // If we responded before the request was complete we don't bother doing a retry. This may not
  // catch certain cases where we are in full streaming mode and we have a connect timeout or an
  // overflow of some kind. However, in many cases deployments will use the buffer filter before
  // this filter which will make this a non-issue. The implementation of supporting retry in cases
  // where the request is not complete is more complicated so we will start with this for now.
  if (!downstream_end_stream_) {
    return false;
  }

  ENVOY_STREAM_LOG(debug, "performing retry", *callbacks_);
  if (!end_stream) {
    upstream_request_->resetStream();
  }

  upstream_request_.reset();
  return true;
}

void Filter::doRetry() {
  Http::ConnectionPool::Instance* conn_pool = getConnPool();
  if (!conn_pool) {
    sendNoHealthyUpstreamResponse();
    cleanup();
    return;
  }

  ASSERT(response_timeout_ || timeout_.global_timeout_.count() == 0);
  ASSERT(!upstream_request_);
  upstream_request_.reset(new UpstreamRequest(*this, *conn_pool));
  upstream_request_->encodeHeaders(!callbacks_->decodingBuffer() && !downstream_trailers_);
  // It's possible we got immediately reset.
  if (upstream_request_) {
    if (callbacks_->decodingBuffer()) {
      // If we are doing a retry we need to make a copy.
      Buffer::OwnedImpl copy(*callbacks_->decodingBuffer());
      upstream_request_->encodeData(copy, !downstream_trailers_);
    }

    if (downstream_trailers_) {
      upstream_request_->encodeTrailers(*downstream_trailers_);
    }

    upstream_request_->setupPerTryTimeout();
  }
}

Filter::UpstreamRequest::UpstreamRequest(Filter& parent, Http::ConnectionPool::Instance& pool)
    : parent_(parent), conn_pool_(pool), grpc_rq_success_deferred_(false),
      request_info_(pool.protocol()), calling_encode_headers_(false), upstream_canary_(false),
      encode_complete_(false), encode_trailers_(false) {

  if (parent_.config_.start_child_span_) {
    span_ = parent_.callbacks_->activeSpan().spawnChild(
        parent_.callbacks_->tracingConfig(), "router " + parent.cluster_->name() + " egress",
        ProdSystemTimeSource::instance_.currentTime());
    span_->setTag(Tracing::Tags::get().COMPONENT, Tracing::Tags::get().PROXY);
  }

  request_info_.healthCheck(parent_.callbacks_->requestInfo().healthCheck());
}

Filter::UpstreamRequest::~UpstreamRequest() {
  if (span_ != nullptr) {
    // TODO(mattklein123): Add tags based on what happened to this request (retries, reset, etc.).
    span_->finishSpan();
  }
  if (per_try_timeout_ != nullptr) {
    // Allows for testing.
    per_try_timeout_->disableTimer();
  }
  clearRequestEncoder();

  for (const auto& upstream_log : parent_.config_.upstream_logs_) {
    upstream_log->log(parent_.downstream_headers_, upstream_headers_, request_info_);
  }
}

void Filter::UpstreamRequest::decodeHeaders(Http::HeaderMapPtr&& headers, bool end_stream) {
  upstream_headers_ = headers.get();
  const uint64_t response_code = Http::Utility::getResponseStatus(*headers);
  request_info_.response_code_.value(static_cast<uint32_t>(response_code));
  parent_.onUpstreamHeaders(response_code, std::move(headers), end_stream);
}

void Filter::UpstreamRequest::decodeData(Buffer::Instance& data, bool end_stream) {
  request_info_.bytes_received_ += data.length();
  parent_.onUpstreamData(data, end_stream);
}

void Filter::UpstreamRequest::decodeTrailers(Http::HeaderMapPtr&& trailers) {
  parent_.onUpstreamTrailers(std::move(trailers));
}

void Filter::UpstreamRequest::encodeHeaders(bool end_stream) {
  ASSERT(!encode_complete_);
  encode_complete_ = end_stream;

  // It's possible for a reset to happen inline within the newStream() call. In this case, we might
  // get deleted inline as well. Only write the returned handle out if it is not nullptr to deal
  // with this case.
  Http::ConnectionPool::Cancellable* handle = conn_pool_.newStream(*this, *this);
  if (handle) {
    conn_pool_stream_handle_ = handle;
  }
}

void Filter::UpstreamRequest::encodeData(Buffer::Instance& data, bool end_stream) {
  ASSERT(!encode_complete_);
  encode_complete_ = end_stream;

  if (!request_encoder_) {
    ENVOY_STREAM_LOG(trace, "buffering {} bytes", *parent_.callbacks_, data.length());
    if (!buffered_request_body_) {
      buffered_request_body_.reset(
          new Buffer::WatermarkBuffer([this]() -> void { this->enableDataFromDownstream(); },
                                      [this]() -> void { this->disableDataFromDownstream(); }));
      buffered_request_body_->setWatermarks(parent_.buffer_limit_);
    }

    buffered_request_body_->move(data);
  } else {
    ENVOY_STREAM_LOG(trace, "proxying {} bytes", *parent_.callbacks_, data.length());
    request_info_.bytes_sent_ += data.length();
    request_encoder_->encodeData(data, end_stream);
  }
}

void Filter::UpstreamRequest::encodeTrailers(const Http::HeaderMap& trailers) {
  ASSERT(!encode_complete_);
  encode_complete_ = true;
  encode_trailers_ = true;

  if (!request_encoder_) {
    ENVOY_STREAM_LOG(trace, "buffering trailers", *parent_.callbacks_);
  } else {
    ENVOY_STREAM_LOG(trace, "proxying trailers", *parent_.callbacks_);
    request_encoder_->encodeTrailers(trailers);
  }
}

void Filter::UpstreamRequest::onResetStream(Http::StreamResetReason reason) {
  clearRequestEncoder();
  if (!calling_encode_headers_) {
    request_info_.setResponseFlag(parent_.streamResetReasonToResponseFlag(reason));
    parent_.onUpstreamReset(UpstreamResetType::Reset, Optional<Http::StreamResetReason>(reason));
  } else {
    deferred_reset_reason_ = reason;
  }
}

void Filter::UpstreamRequest::resetStream() {
  if (conn_pool_stream_handle_) {
    ENVOY_STREAM_LOG(debug, "cancelling pool request", *parent_.callbacks_);
    ASSERT(!request_encoder_);
    conn_pool_stream_handle_->cancel();
    conn_pool_stream_handle_ = nullptr;
  }

  if (request_encoder_) {
    ENVOY_STREAM_LOG(debug, "resetting pool request", *parent_.callbacks_);
    request_encoder_->getStream().removeCallbacks(*this);
    request_encoder_->getStream().resetStream(Http::StreamResetReason::LocalReset);
  }
}

void Filter::UpstreamRequest::setupPerTryTimeout() {
  ASSERT(!per_try_timeout_);
  if (parent_.timeout_.per_try_timeout_.count() > 0) {
    per_try_timeout_ =
        parent_.callbacks_->dispatcher().createTimer([this]() -> void { onPerTryTimeout(); });
    per_try_timeout_->enableTimer(parent_.timeout_.per_try_timeout_);
  }
}

void Filter::UpstreamRequest::onPerTryTimeout() {
  ENVOY_STREAM_LOG(debug, "upstream per try timeout", *parent_.callbacks_);
  parent_.cluster_->stats().upstream_rq_per_try_timeout_.inc();
  if (upstream_host_) {
    upstream_host_->stats().rq_timeout_.inc();
  }
  resetStream();
  request_info_.setResponseFlag(RequestInfo::ResponseFlag::UpstreamRequestTimeout);
  parent_.onUpstreamReset(UpstreamResetType::PerTryTimeout,
                          Optional<Http::StreamResetReason>(Http::StreamResetReason::LocalReset));
}

void Filter::UpstreamRequest::onPoolFailure(Http::ConnectionPool::PoolFailureReason reason,
                                            Upstream::HostDescriptionConstSharedPtr host) {
  Http::StreamResetReason reset_reason = Http::StreamResetReason::ConnectionFailure;
  switch (reason) {
  case Http::ConnectionPool::PoolFailureReason::Overflow:
    reset_reason = Http::StreamResetReason::Overflow;
    break;
  case Http::ConnectionPool::PoolFailureReason::ConnectionFailure:
    reset_reason = Http::StreamResetReason::ConnectionFailure;
    break;
  }

  // Mimic an upstream reset.
  onUpstreamHostSelected(host);
  onResetStream(reset_reason);
}

void Filter::UpstreamRequest::onPoolReady(Http::StreamEncoder& request_encoder,
                                          Upstream::HostDescriptionConstSharedPtr host) {
  ENVOY_STREAM_LOG(debug, "pool ready", *parent_.callbacks_);

  // TODO(ggreenway): set upstream local address in the RequestInfo.
  onUpstreamHostSelected(host);
  request_encoder.getStream().addCallbacks(*this);

  conn_pool_stream_handle_ = nullptr;
  setRequestEncoder(request_encoder);
  calling_encode_headers_ = true;
  if (parent_.route_entry_->autoHostRewrite() && !host->hostname().empty()) {
    parent_.downstream_headers_->Host()->value(host->hostname());
  }

  if (span_ != nullptr) {
    span_->injectContext(*parent_.downstream_headers_);
  }

  request_encoder.encodeHeaders(*parent_.downstream_headers_,
                                !buffered_request_body_ && encode_complete_ && !encode_trailers_);
  calling_encode_headers_ = false;

  // It is possible to get reset in the middle of an encodeHeaders() call. This happens for example
  // in the HTTP/2 codec if the frame cannot be encoded for some reason. This should never happen
  // but it's unclear if we have covered all cases so protect against it and test for it. One
  // specific example of a case where this happens is if we try to encode a total header size that
  // is too big in HTTP/2 (64K currently).
  if (deferred_reset_reason_.valid()) {
    onResetStream(deferred_reset_reason_.value());
  } else {
    if (buffered_request_body_) {
      request_info_.bytes_sent_ += buffered_request_body_->length();
      request_encoder.encodeData(*buffered_request_body_, encode_complete_ && !encode_trailers_);
    }

    if (encode_trailers_) {
      request_encoder.encodeTrailers(*parent_.downstream_trailers_);
    }
  }
}

RetryStatePtr
ProdFilter::createRetryState(const RetryPolicy& policy, Http::HeaderMap& request_headers,
                             const Upstream::ClusterInfo& cluster, Runtime::Loader& runtime,
                             Runtime::RandomGenerator& random, Event::Dispatcher& dispatcher,
                             Upstream::ResourcePriority priority) {
  return RetryStateImpl::create(policy, request_headers, cluster, runtime, random, dispatcher,
                                priority);
}

void Filter::UpstreamRequest::setRequestEncoder(Http::StreamEncoder& request_encoder) {
  request_encoder_ = &request_encoder;
  // Now that there is an encoder, have the connection manager inform the manager when the
  // downstream buffers are overrun. This may result in immediate watermark callbacks referencing
  // the encoder.
  parent_.callbacks_->addDownstreamWatermarkCallbacks(downstream_watermark_manager_);
}

void Filter::UpstreamRequest::clearRequestEncoder() {
  // Before clearing the encoder, unsubscribe from callbacks.
  if (request_encoder_) {
    parent_.callbacks_->removeDownstreamWatermarkCallbacks(downstream_watermark_manager_);
  }
  request_encoder_ = nullptr;
}

void Filter::UpstreamRequest::DownstreamWatermarkManager::onAboveWriteBufferHighWatermark() {
  ASSERT(parent_.request_encoder_);
  // The downstream connection is overrun. Pause reads from upstream.
  parent_.parent_.cluster_->stats().upstream_flow_control_paused_reading_total_.inc();
  parent_.request_encoder_->getStream().readDisable(true);
}

void Filter::UpstreamRequest::DownstreamWatermarkManager::onBelowWriteBufferLowWatermark() {
  ASSERT(parent_.request_encoder_);
  // The downstream connection has buffer available. Resume reads from upstream.
  parent_.parent_.cluster_->stats().upstream_flow_control_resumed_reading_total_.inc();
  parent_.request_encoder_->getStream().readDisable(false);
}

} // namespace Router
} // namespace Envoy
