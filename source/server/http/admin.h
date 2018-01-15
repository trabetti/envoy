#pragma once

#include <chrono>
#include <list>
#include <string>

#include "envoy/http/filter.h"
#include "envoy/network/listen_socket.h"
#include "envoy/server/admin.h"
#include "envoy/server/instance.h"
#include "envoy/upstream/outlier_detection.h"
#include "envoy/upstream/resource_manager.h"

#include "common/common/logger.h"
#include "common/common/macros.h"
#include "common/http/conn_manager_impl.h"
#include "common/http/date_provider_impl.h"
#include "common/http/utility.h"

#include "server/config/network/http_connection_manager.h"

namespace Envoy {
namespace Server {

/**
 * Implementation of Server::admin.
 */
class AdminImpl : public Admin,
                  public Network::FilterChainFactory,
                  public Http::FilterChainFactory,
                  public Http::ConnectionManagerConfig,
                  Logger::Loggable<Logger::Id::admin> {
public:
  AdminImpl(const std::string& access_log_path, const std::string& profiler_path,
            const std::string& address_out_path, Network::Address::InstanceConstSharedPtr address,
            Server::Instance& server, Stats::Scope& listener_scope);

  Http::Code runCallback(const std::string& path_and_query, Http::HeaderMap& response_headers,
                         Buffer::Instance& response, FilterData* filter_data);
  const Network::ListenSocket& socket() override { return *socket_; }
  Network::ListenSocket& mutable_socket() { return *socket_; }

  // Server::Admin
  bool addHandler(const std::string& prefix, const std::string& help_text, HandlerCb callback,
                  bool removable, bool mutates_server_state) override;
  bool removeHandler(const std::string& prefix) override;

  // Network::FilterChainFactory
  bool createFilterChain(Network::Connection& connection) override;

  // Http::FilterChainFactory
  void createFilterChain(Http::FilterChainFactoryCallbacks& callbacks) override;

  // Http::ConnectionManagerConfig
  const std::list<AccessLog::InstanceSharedPtr>& accessLogs() override { return access_logs_; }
  Http::ServerConnectionPtr createCodec(Network::Connection& connection,
                                        const Buffer::Instance& data,
                                        Http::ServerConnectionCallbacks& callbacks) override;
  Http::DateProvider& dateProvider() override { return date_provider_; }
  std::chrono::milliseconds drainTimeout() override { return std::chrono::milliseconds(100); }
  Http::FilterChainFactory& filterFactory() override { return *this; }
  bool generateRequestId() override { return false; }
  const Optional<std::chrono::milliseconds>& idleTimeout() override { return idle_timeout_; }
  Router::RouteConfigProvider& routeConfigProvider() override { return route_config_provider_; }
  const std::string& serverName() override {
    return Server::Configuration::HttpConnectionManagerConfig::DEFAULT_SERVER_STRING;
  }
  Http::ConnectionManagerStats& stats() override { return stats_; }
  Http::ConnectionManagerTracingStats& tracingStats() override { return tracing_stats_; }
  bool useRemoteAddress() override { return true; }
  Http::ForwardClientCertType forwardClientCert() override {
    return Http::ForwardClientCertType::Sanitize;
  }
  const std::vector<Http::ClientCertDetailsType>& setCurrentClientCertDetails() const override {
    return set_current_client_cert_details_;
  }
  const Network::Address::Instance& localAddress() override;
  const Optional<std::string>& userAgent() override { return user_agent_; }
  const Http::TracingConnectionManagerConfig* tracingConfig() override { return nullptr; }
  Http::ConnectionManagerListenerStats& listenerStats() override { return listener_stats_; }

private:
  /**
   * Individual admin handler including prefix, help text, and callback.
   */
  struct UrlHandler {
    const std::string prefix_;
    const std::string help_text_;
    const HandlerCb handler_;
    const bool removable_;
    const bool mutates_server_state_;
  };

  /**
   * Implementation of RouteConfigProvider that returns a static null route config.
   */
  struct NullRouteConfigProvider : public Router::RouteConfigProvider {
    NullRouteConfigProvider();

    // Router::RouteConfigProvider
    Router::ConfigConstSharedPtr config() override { return config_; }
    const std::string versionInfo() const override { CONSTRUCT_ON_FIRST_USE(std::string, ""); }

    Router::ConfigConstSharedPtr config_;
  };

  /**
   * Attempt to change the log level of a logger or all loggers
   * @param params supplies the incoming endpoint query params.
   * @return TRUE if level change succeeded, FALSE otherwise.
   */
  bool changeLogLevel(const Http::Utility::QueryParams& params);
  void addCircuitSettings(const std::string& cluster_name, const std::string& priority_str,
                          Upstream::ResourceManager& resource_manager, Buffer::Instance& response);
  void addOutlierInfo(const std::string& cluster_name,
                      const Upstream::Outlier::Detector* outlier_detector,
                      Buffer::Instance& response);
  static std::string statsAsJson(const std::map<std::string, uint64_t>& all_stats);

  std::vector<const UrlHandler*> sortedHandlers() const;

  /**
   * URL handlers.
   */
  Http::Code handlerAdminHome(const std::string& path_and_query, Http::HeaderMap& response_headers,
                              Buffer::Instance& response, FilterData*);
  Http::Code handlerCerts(const std::string& path_and_query, Http::HeaderMap& response_headers,
                          Buffer::Instance& response, FilterData*);
  Http::Code handlerClusters(const std::string& path_and_query, Http::HeaderMap& response_headers,
                             Buffer::Instance& response, FilterData*);
  Http::Code handlerCpuProfiler(const std::string& path_and_query,
                                Http::HeaderMap& response_headers, Buffer::Instance& response,
                                FilterData*);
  Http::Code handlerHealthcheckFail(const std::string& path_and_query,
                                    Http::HeaderMap& response_headers, Buffer::Instance& response,
                                    FilterData*);
  Http::Code handlerHealthcheckOk(const std::string& path_and_query,
                                  Http::HeaderMap& response_headers, Buffer::Instance& response,
                                  FilterData*);
  Http::Code handlerHelp(const std::string& path_and_query, Http::HeaderMap& response_headers,
                         Buffer::Instance& response, FilterData*);
  Http::Code handlerHotRestartVersion(const std::string& path_and_query,
                                      Http::HeaderMap& response_headers,
                                      Buffer::Instance& response, FilterData*);
  Http::Code handlerListenerInfo(const std::string& path_and_query,
                                 Http::HeaderMap& response_headers, Buffer::Instance& response,
                                 FilterData*);
  Http::Code handlerLogging(const std::string& path_and_query, Http::HeaderMap& response_headers,
                            Buffer::Instance& response, FilterData*);
  Http::Code handlerMain(const std::string& path, Buffer::Instance& response, 
                         FilterData*);
  Http::Code handlerQuitQuitQuit(const std::string& path_and_query,
                                 Http::HeaderMap& response_headers, Buffer::Instance& response,
                                 FilterData*);
  Http::Code handlerResetCounters(const std::string& path_and_query,
                                  Http::HeaderMap& response_headers, Buffer::Instance& response,
                                  FilterData*);
  Http::Code handlerServerInfo(const std::string& path_and_query, Http::HeaderMap& response_headers,
                               Buffer::Instance& response, FilterData*);
  Http::Code handlerStats(const std::string& path_and_query, Http::HeaderMap& response_headers,
                          Buffer::Instance& response, FilterData*);
  Http::Code handlerHystrixEventStream(const std::string& path_and_query, Http::HeaderMap& response_headers,
                                       Buffer::Instance& response, FilterData* filter_data);

  Server::Instance& server_;
  std::list<AccessLog::InstanceSharedPtr> access_logs_;
  const std::string profile_path_;
  Network::ListenSocketPtr socket_;
  Http::ConnectionManagerStats stats_;
  Http::ConnectionManagerTracingStats tracing_stats_;
  NullRouteConfigProvider route_config_provider_;
  std::list<UrlHandler> handlers_;
  Optional<std::chrono::milliseconds> idle_timeout_;
  Optional<std::string> user_agent_;
  Http::SlowDateProviderImpl date_provider_;
  std::vector<Http::ClientCertDetailsType> set_current_client_cert_details_;
  Http::ConnectionManagerListenerStats listener_stats_;
};

/**
 * A terminal HTTP filter that implements server admin functionality.
 */
class AdminFilter : public Http::StreamDecoderFilter, Logger::Loggable<Logger::Id::admin> {
public:
  AdminFilter(AdminImpl& parent);

  // Http::StreamFilterBase
  void onDestroy() override {
	  filter_data_->Destroy();
  }

  // Http::StreamDecoderFilter
  Http::FilterHeadersStatus decodeHeaders(Http::HeaderMap& response_headers,
                                          bool end_stream) override;
  Http::FilterDataStatus decodeData(Buffer::Instance& data, bool end_stream) override;
  Http::FilterTrailersStatus decodeTrailers(Http::HeaderMap& trailers) override;
  void setDecoderFilterCallbacks(Http::StreamDecoderFilterCallbacks& callbacks) override {
    callbacks_ = &callbacks;
  }

private:
  /**
   * Called when an admin request has been completely received.
   */
  void onComplete();

  AdminImpl& parent_;
  Http::StreamDecoderFilterCallbacks* callbacks_{};
  Http::HeaderMap* request_headers_{};
  FilterData* filter_data_;
};

/**
 * Formatter for metric/labels exported to Prometheus.
 *
 * See: https://prometheus.io/docs/concepts/data_model
 */
class PrometheusStatsFormatter {
public:
  /**
   * Extracts counters and gauges and relevant tags, appending them to
   * the response buffer after sanitizing the metric / label names.
   */
  static void statsAsPrometheus(const std::list<Stats::CounterSharedPtr>& counters,
                                const std::list<Stats::GaugeSharedPtr>& gauges,
                                Buffer::Instance& response);
  /**
   * Format the given tags, returning a string as a comma-separated list
   * of <tag_name>="<tag_value>" pairs.
   */
  static std::string formattedTags(const std::vector<Stats::Tag>& tags);
  /**
   * Format the given metric name, prefixed with "envoy_".
   */
  static std::string metricName(const std::string& extractedName);

private:
  /**
   * Take a string and sanitize it according to Prometheus conventions.
   */
  static std::string sanitizeName(const std::string& name);
};

} // namespace Server
} // namespace Envoy
