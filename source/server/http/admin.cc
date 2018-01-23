#include "server/http/admin.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "envoy/filesystem/filesystem.h"
#include "envoy/runtime/runtime.h"
#include "envoy/server/hot_restart.h"
#include "envoy/server/instance.h"
#include "envoy/server/options.h"
#include "envoy/stats/stats.h"
#include "envoy/upstream/cluster_manager.h"
#include "envoy/upstream/upstream.h"

#include "common/access_log/access_log_formatter.h"
#include "common/access_log/access_log_impl.h"
#include "common/buffer/buffer_impl.h"
#include "common/common/assert.h"
#include "common/common/enum_to_int.h"
#include "common/common/utility.h"
#include "common/common/version.h"
#include "common/html/utility.h"
#include "common/http/codes.h"
#include "common/http/header_map_impl.h"
#include "common/http/headers.h"
#include "common/http/http1/codec_impl.h"
#include "common/json/json_loader.h"
#include "common/network/listen_socket_impl.h"
#include "common/profiler/profiler.h"
#include "common/router/config_impl.h"
#include "common/upstream/host_utility.h"

#include "absl/strings/str_replace.h"
#include "fmt/format.h"

// TODO(mattklein123): Switch to JSON interface methods and remove rapidjson dependency.
#include "rapidjson/document.h"
#include "rapidjson/error/en.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/reader.h"
#include "rapidjson/schema.h"
#include "rapidjson/stream.h"
#include "rapidjson/stringbuffer.h"
#include "spdlog/spdlog.h"

using namespace rapidjson;

namespace Envoy {
namespace Server {

namespace {

/**
 * Favicon base64 image was harvested by screen-capturing the favicon from a Chrome tab
 * while visiting https://www.envoyproxy.io/. The resulting PNG was translated to base64
 * by dropping it into https://www.base64-image.de/ and then pasting the resulting string
 * below.
 *
 * The actual favicon source for that, https://www.envoyproxy.io/img/favicon.ico is nicer
 * because it's transparent, but is also 67646 bytes, which is annoying to inline. We could
 * just reference that rather than inlining it, but then the favicon won't work when visiting
 * the admin page from a network that can't see the internet.
 */
const char EnvoyFavicon[] =
    "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABgAAAAYCAYAAADgdz34AAAAAXNSR0IArs4c6QAAAARnQU1"
    "BAACxjwv8YQUAAAAJcEhZcwAAEnQAABJ0Ad5mH3gAAAH9SURBVEhL7ZRdTttAFIUrUFaAX5w9gIhgUfzshFRK+gIbaVbA"
    "zwaqCly1dSpKk5A485/YCdXpHTB4BsdgVe0bD0cZ3Xsm38yZ8byTUuJ/6g3wqqoBrBhPTzmmLfptMbAzttJTpTKAF2MWC"
    "7ADCdNIwXZpvMMwayiIwwS874CcOc9VuQPR1dBBChPMITpFXXU45hukIIH6kHhzVqkEYB8F5HYGvZ5B7EvwmHt9K/59Cr"
    "U3QbY2RNYaQPYmJc+jPIBICNCcg20ZsAsCPfbcrFlRF+cJZpvXSJt9yMTxO/IAzJrCOfhJXiOgFEX/SbZmezTWxyNk4Q9"
    "anHMmjnzAhEyhAW8LCE6wl26J7ZFHH1FMYQxh567weQBOO1AW8D7P/UXAQySq/QvL8Fu9HfCEw4SKALm5BkC3bwjwhSKr"
    "A5hYAMXTJnPNiMyRBVzVjcgCyHiSm+8P+WGlnmwtP2RzbCMiQJ0d2KtmmmPorRHEhfMROVfTG5/fYrF5iWXzE80tfy9WP"
    "sCqx5Buj7FYH0LvDyHiqd+3otpsr4/fa5+xbEVQPfrYnntylQG5VGeMLBhgEfyE7o6e6qYzwHIjwl0QwXSvvTmrVAY4D5"
    "ddvT64wV0jRrr7FekO/XEjwuwwhuw7Ef7NY+dlfXpLb06EtHUJdVbsxvNUqBrwj/QGeEUSfwBAkmWHn5Bb/gAAAABJRU5"
    "ErkJggg==";

const char AdminHtmlStart[] = R"(
<head>
  <title>Envoy Admin</title>
  <link rel='shortcut icon' type='image/png' href='@FAVICON@'/>
  <style>
    .home-table {
      font-family: sans-serif;
      font-size: medium;
      border-collapse: collapse;
    }

    .home-row:nth-child(even) {
      background-color: #dddddd;
    }

    .home-data {
      border: 1px solid #dddddd;
      text-align: left;
      padding: 8px;
    }

    .home-form {
      margin-bottom: 0;
    }
  </style>
</head>
<body>
  <table class='home-table'>
    <thead>
      <th class='home-data'>Command</th>
      <th class='home-data'>Description</th>
     </thead>
     <tbody>
)";

const char AdminHtmlEnd[] = R"(
    </tbody>
  </table>
</body>
)";

} // namespace

AdminFilter::AdminFilter(AdminImpl& parent) : parent_(parent) {}

Http::FilterHeadersStatus AdminFilter::decodeHeaders(Http::HeaderMap& response_headers,
                                                     bool end_stream) {
  request_headers_ = &response_headers;
  if (end_stream) {
    onComplete();
  }

  return Http::FilterHeadersStatus::StopIteration;
}

Http::FilterDataStatus AdminFilter::decodeData(Buffer::Instance&, bool end_stream) {
  if (end_stream) {
    onComplete();
  }

  return Http::FilterDataStatus::StopIterationNoBuffer;
}

Http::FilterTrailersStatus AdminFilter::decodeTrailers(Http::HeaderMap&) {
  onComplete();
  return Http::FilterTrailersStatus::StopIteration;
}

bool AdminImpl::changeLogLevel(const Http::Utility::QueryParams& params) {
  if (params.size() != 1) {
    return false;
  }

  std::string name = params.begin()->first;
  std::string level = params.begin()->second;

  // First see if the level is valid.
  size_t level_to_use = std::numeric_limits<size_t>::max();
  for (size_t i = 0; i < ARRAY_SIZE(spdlog::level::level_names); i++) {
    if (level == spdlog::level::level_names[i]) {
      level_to_use = i;
      break;
    }
  }

  if (level_to_use == std::numeric_limits<size_t>::max()) {
    return false;
  }

  // Now either change all levels or a single level.
  if (name == "level") {
    ENVOY_LOG(debug, "change all log levels: level='{}'", level);
    for (const Logger::Logger& logger : Logger::Registry::loggers()) {
      logger.setLevel(static_cast<spdlog::level::level_enum>(level_to_use));
    }
  } else {
    ENVOY_LOG(debug, "change log level: name='{}' level='{}'", name, level);
    const Logger::Logger* logger_to_change = nullptr;
    for (const Logger::Logger& logger : Logger::Registry::loggers()) {
      if (logger.name() == name) {
        logger_to_change = &logger;
        break;
      }
    }

    if (!logger_to_change) {
      return false;
    }

    logger_to_change->setLevel(static_cast<spdlog::level::level_enum>(level_to_use));
  }

  return true;
}

void AdminImpl::addOutlierInfo(const std::string& cluster_name,
                               const Upstream::Outlier::Detector* outlier_detector,
                               Buffer::Instance& response) {
  if (outlier_detector) {
    response.add(fmt::format("{}::outlier::success_rate_average::{}\n", cluster_name,
                             outlier_detector->successRateAverage()));
    response.add(fmt::format("{}::outlier::success_rate_ejection_threshold::{}\n", cluster_name,
                             outlier_detector->successRateEjectionThreshold()));
  }
}

void AdminImpl::addCircuitSettings(const std::string& cluster_name, const std::string& priority_str,
                                   Upstream::ResourceManager& resource_manager,
                                   Buffer::Instance& response) {
  response.add(fmt::format("{}::{}_priority::max_connections::{}\n", cluster_name, priority_str,
                           resource_manager.connections().max()));
  response.add(fmt::format("{}::{}_priority::max_pending_requests::{}\n", cluster_name,
                           priority_str, resource_manager.pendingRequests().max()));
  response.add(fmt::format("{}::{}_priority::max_requests::{}\n", cluster_name, priority_str,
                           resource_manager.requests().max()));
  response.add(fmt::format("{}::{}_priority::max_retries::{}\n", cluster_name, priority_str,
                           resource_manager.retries().max()));
}

Http::Code AdminImpl::handlerClusters(const std::string&, Http::HeaderMap&,
                                      Buffer::Instance& response) {
  response.add(fmt::format("version_info::{}\n", server_.clusterManager().versionInfo()));

  for (auto& cluster : server_.clusterManager().clusters()) {
    addOutlierInfo(cluster.second.get().info()->name(), cluster.second.get().outlierDetector(),
                   response);

    addCircuitSettings(
        cluster.second.get().info()->name(), "default",
        cluster.second.get().info()->resourceManager(Upstream::ResourcePriority::Default),
        response);
    addCircuitSettings(
        cluster.second.get().info()->name(), "high",
        cluster.second.get().info()->resourceManager(Upstream::ResourcePriority::High), response);

    response.add(fmt::format("{}::added_via_api::{}\n", cluster.second.get().info()->name(),
                             cluster.second.get().info()->addedViaApi()));
    for (auto& host_set : cluster.second.get().prioritySet().hostSetsPerPriority()) {
      for (auto& host : host_set->hosts()) {
        std::map<std::string, uint64_t> all_stats;
        for (const Stats::CounterSharedPtr& counter : host->counters()) {
          all_stats[counter->name()] = counter->value();
        }

        for (const Stats::GaugeSharedPtr& gauge : host->gauges()) {
          all_stats[gauge->name()] = gauge->value();
        }

        for (auto stat : all_stats) {
          response.add(fmt::format("{}::{}::{}::{}\n", cluster.second.get().info()->name(),
                                   host->address()->asString(), stat.first, stat.second));
        }

        response.add(fmt::format("{}::{}::health_flags::{}\n", cluster.second.get().info()->name(),
                                 host->address()->asString(),
                                 Upstream::HostUtility::healthFlagsToString(*host)));
        response.add(fmt::format("{}::{}::weight::{}\n", cluster.second.get().info()->name(),
                                 host->address()->asString(), host->weight()));
        response.add(fmt::format("{}::{}::region::{}\n", cluster.second.get().info()->name(),
                                 host->address()->asString(), host->locality().region()));
        response.add(fmt::format("{}::{}::zone::{}\n", cluster.second.get().info()->name(),
                                 host->address()->asString(), host->locality().zone()));
        response.add(fmt::format("{}::{}::sub_zone::{}\n", cluster.second.get().info()->name(),
                                 host->address()->asString(), host->locality().sub_zone()));
        response.add(fmt::format("{}::{}::canary::{}\n", cluster.second.get().info()->name(),
                                 host->address()->asString(), host->canary()));
        response.add(fmt::format("{}::{}::success_rate::{}\n", cluster.second.get().info()->name(),
                                 host->address()->asString(),
                                 host->outlierDetector().successRate()));
      }
    }
  }

  return Http::Code::OK;
}

Http::Code AdminImpl::handlerCpuProfiler(const std::string& url, Http::HeaderMap&,
                                         Buffer::Instance& response) {
  Http::Utility::QueryParams query_params = Http::Utility::parseQueryString(url);
  if (query_params.size() != 1 || query_params.begin()->first != "enable" ||
      (query_params.begin()->second != "y" && query_params.begin()->second != "n")) {
    response.add("?enable=<y|n>\n");
    return Http::Code::BadRequest;
  }

  bool enable = query_params.begin()->second == "y";
  if (enable && !Profiler::Cpu::profilerEnabled()) {
    if (!Profiler::Cpu::startProfiler(profile_path_)) {
      response.add("failure to start the profiler");
      return Http::Code::InternalServerError;
    }

  } else if (!enable && Profiler::Cpu::profilerEnabled()) {
    Profiler::Cpu::stopProfiler();
  }

  response.add("OK\n");
  return Http::Code::OK;
}

Http::Code AdminImpl::handlerHealthcheckFail(const std::string&, Http::HeaderMap&,
                                             Buffer::Instance& response) {
  server_.failHealthcheck(true);
  response.add("OK\n");
  return Http::Code::OK;
}

Http::Code AdminImpl::handlerHealthcheckOk(const std::string&, Http::HeaderMap&,
                                           Buffer::Instance& response) {
  server_.failHealthcheck(false);
  response.add("OK\n");
  return Http::Code::OK;
}

Http::Code AdminImpl::handlerHotRestartVersion(const std::string&, Http::HeaderMap&,
                                               Buffer::Instance& response) {
  response.add(server_.hotRestart().version());
  return Http::Code::OK;
}

Http::Code AdminImpl::handlerLogging(const std::string& url, Http::HeaderMap&,
                                     Buffer::Instance& response) {
  Http::Utility::QueryParams query_params = Http::Utility::parseQueryString(url);

  Http::Code rc = Http::Code::OK;
  if (!changeLogLevel(query_params)) {
    response.add("usage: /logging?<name>=<level> (change single level)\n");
    response.add("usage: /logging?level=<level> (change all levels)\n");
    response.add("levels: ");
    for (size_t i = 0; i < ARRAY_SIZE(spdlog::level::level_names); i++) {
      response.add(fmt::format("{} ", spdlog::level::level_names[i]));
    }

    response.add("\n");
    rc = Http::Code::NotFound;
  }

  response.add("active loggers:\n");
  for (const Logger::Logger& logger : Logger::Registry::loggers()) {
    response.add(fmt::format("  {}: {}\n", logger.name(), logger.levelString()));
  }

  response.add("\n");
  return rc;
}

Http::Code AdminImpl::handlerResetCounters(const std::string&, Http::HeaderMap&,
                                           Buffer::Instance& response) {
  for (const Stats::CounterSharedPtr& counter : server_.stats().counters()) {
    counter->reset();
  }

  response.add("OK\n");
  return Http::Code::OK;
}

Http::Code AdminImpl::handlerServerInfo(const std::string&, Http::HeaderMap&,
                                        Buffer::Instance& response) {
  time_t current_time = time(nullptr);
  response.add(fmt::format("envoy {} {} {} {} {}\n", VersionInfo::version(),
                           server_.healthCheckFailed() ? "draining" : "live",
                           current_time - server_.startTimeCurrentEpoch(),
                           current_time - server_.startTimeFirstEpoch(),
                           server_.options().restartEpoch()));
  return Http::Code::OK;
}

Http::Code AdminImpl::handlerStats(const std::string& url, Http::HeaderMap& response_headers,
                                   Buffer::Instance& response) {
  // We currently don't support timers locally (only via statsd) so just group all the counters
  // and gauges together, alpha sort them, and spit them out.
  Http::Code rc = Http::Code::OK;
  const Http::Utility::QueryParams params = Http::Utility::parseQueryString(url);
  std::map<std::string, uint64_t> all_stats;
  for (const Stats::CounterSharedPtr& counter : server_.stats().counters()) {
    all_stats.emplace(counter->name(), counter->value());
  }

  for (const Stats::GaugeSharedPtr& gauge : server_.stats().gauges()) {
    all_stats.emplace(gauge->name(), gauge->value());
  }

  if (params.size() == 0) {
    // No Arguments so use the standard.
    for (auto stat : all_stats) {
      response.add(fmt::format("{}: {}\n", stat.first, stat.second));
    }
  } else {
    const std::string format_key = params.begin()->first;
    const std::string format_value = params.begin()->second;
    if (format_key == "format" && format_value == "json") {
      response_headers.insertContentType().value().setReference(
          Http::Headers::get().ContentTypeValues.Json);
      response.add(AdminImpl::statsAsJson(all_stats));
    } else if (format_key == "format" && format_value == "prometheus") {
      PrometheusStatsFormatter::statsAsPrometheus(server_.stats().counters(),
                                                  server_.stats().gauges(), response);
    } else {
      response.add("usage: /stats?format=json \n");
      response.add("\n");
      rc = Http::Code::NotFound;
    }
  }
  return rc;
}

std::string PrometheusStatsFormatter::sanitizeName(const std::string& name) {
  std::string stats_name = name;
  std::replace(stats_name.begin(), stats_name.end(), '.', '_');
  std::replace(stats_name.begin(), stats_name.end(), '-', '_');
  return stats_name;
}

std::string PrometheusStatsFormatter::formattedTags(const std::vector<Stats::Tag>& tags) {
  std::vector<std::string> buf;
  for (const Stats::Tag& tag : tags) {
    buf.push_back(fmt::format("{}=\"{}\"", sanitizeName(tag.name_), tag.value_));
  }
  return StringUtil::join(buf, ",");
}

std::string PrometheusStatsFormatter::metricName(const std::string& extractedName) {
  // Add namespacing prefix to avoid conflicts, as per best practice:
  // https://prometheus.io/docs/practices/naming/#metric-names
  // Also, naming conventions on https://prometheus.io/docs/concepts/data_model/
  return fmt::format("envoy_{0}", sanitizeName(extractedName));
}

void PrometheusStatsFormatter::statsAsPrometheus(const std::list<Stats::CounterSharedPtr>& counters,
                                                 const std::list<Stats::GaugeSharedPtr>& gauges,
                                                 Buffer::Instance& response) {
  for (const auto& counter : counters) {
    const std::string tags = formattedTags(counter->tags());
    const std::string metric_name = metricName(counter->tagExtractedName());
    response.add(fmt::format("# TYPE {0} counter\n", metric_name));
    response.add(fmt::format("{0}{{{1}}} {2}\n", metric_name, tags, counter->value()));
  }

  for (const auto& gauge : gauges) {
    const std::string tags = formattedTags(gauge->tags());
    const std::string metric_name = metricName(gauge->tagExtractedName());
    response.add(fmt::format("# TYPE {0} gauge\n", metric_name));
    response.add(fmt::format("{0}{{{1}}} {2}\n", metric_name, tags, gauge->value()));
  }
}

std::string AdminImpl::statsAsJson(const std::map<std::string, uint64_t>& all_stats) {
  rapidjson::Document document;
  document.SetObject();
  rapidjson::Value stats_array(rapidjson::kArrayType);
  rapidjson::Document::AllocatorType& allocator = document.GetAllocator();
  for (auto stat : all_stats) {
    Value stat_obj;
    stat_obj.SetObject();
    Value stat_name;
    stat_name.SetString(stat.first.c_str(), allocator);
    stat_obj.AddMember("name", stat_name, allocator);
    Value stat_value;
    stat_value.SetInt(stat.second);
    stat_obj.AddMember("value", stat_value, allocator);
    stats_array.PushBack(stat_obj, allocator);
  }
  document.AddMember("stats", stats_array, allocator);
  rapidjson::StringBuffer strbuf;
  rapidjson::PrettyWriter<StringBuffer> writer(strbuf);
  document.Accept(writer);
  return strbuf.GetString();
}

Http::Code AdminImpl::handlerQuitQuitQuit(const std::string&, Http::HeaderMap&,
                                          Buffer::Instance& response) {
  server_.shutdown();
  response.add("OK\n");
  return Http::Code::OK;
}

Http::Code AdminImpl::handlerListenerInfo(const std::string&, Http::HeaderMap& response_headers,
                                          Buffer::Instance& response) {
  response_headers.insertContentType().value().setReference(
      Http::Headers::get().ContentTypeValues.Json);
  std::list<std::string> listeners;
  for (auto listener : server_.listenerManager().listeners()) {
    listeners.push_back(listener.get().socket().localAddress()->asString());
  }
  response.add(Json::Factory::listAsJsonString(listeners));
  return Http::Code::OK;
}

Http::Code AdminImpl::handlerCerts(const std::string&, Http::HeaderMap&,
                                   Buffer::Instance& response) {
  // This set is used to track distinct certificates. We may have multiple listeners, upstreams, etc
  // using the same cert.
  std::unordered_set<std::string> context_info_set;
  std::string context_format = "{{\n\t\"ca_cert\": \"{}\",\n\t\"cert_chain\": \"{}\"\n}}\n";
  server_.sslContextManager().iterateContexts([&](const Ssl::Context& context) -> void {
    context_info_set.insert(fmt::format(context_format, context.getCaCertInformation(),
                                        context.getCertChainInformation()));
  });

  std::string cert_result_string;
  for (const std::string& context_info : context_info_set) {
    cert_result_string += context_info;
  }
  response.add(cert_result_string);
  return Http::Code::OK;
}

Http::Code AdminImpl::handlerRuntime(const std::string& url, Http::HeaderMap& response_headers,
                                     Buffer::Instance& response) {
  Http::Code rc = Http::Code::OK;
  const Http::Utility::QueryParams params = Http::Utility::parseQueryString(url);
  const auto& entries = server_.runtime().snapshot().getAll();
  const auto pairs = sortedRuntime(entries);

  if (params.size() == 0) {
    for (const auto& entry : pairs) {
      response.add(fmt::format("{}: {}\n", entry.first, entry.second.string_value_));
    }
  } else {
    if (params.begin()->first == "format" && params.begin()->second == "json") {
      response_headers.insertContentType().value().setReference(
          Http::Headers::get().ContentTypeValues.Json);
      response.add(runtimeAsJson(pairs));
      response.add("\n");
    } else {
      response.add("usage: /runtime?format=json\n");
      rc = Http::Code::BadRequest;
    }
  }

  return rc;
}

const std::vector<std::pair<std::string, Runtime::Snapshot::Entry>> AdminImpl::sortedRuntime(
    const std::unordered_map<std::string, const Runtime::Snapshot::Entry>& entries) {
  std::vector<std::pair<std::string, Runtime::Snapshot::Entry>> pairs(entries.begin(),
                                                                      entries.end());

  std::sort(pairs.begin(), pairs.end(),
            [](const std::pair<std::string, const Runtime::Snapshot::Entry>& a,
               const std::pair<std::string, const Runtime::Snapshot::Entry>& b) -> bool {
              return a.first < b.first;
            });

  return pairs;
}

std::string AdminImpl::runtimeAsJson(
    const std::vector<std::pair<std::string, Runtime::Snapshot::Entry>>& entries) {
  rapidjson::Document document;
  document.SetObject();
  rapidjson::Value entries_array(rapidjson::kArrayType);
  rapidjson::Document::AllocatorType& allocator = document.GetAllocator();
  for (const auto& entry : entries) {
    Value entry_obj;
    entry_obj.SetObject();

    entry_obj.AddMember("name", {entry.first.c_str(), allocator}, allocator);

    Value entry_value;
    if (entry.second.uint_value_.valid()) {
      entry_value.SetUint64(entry.second.uint_value_.value());
    } else {
      entry_value.SetString(entry.second.string_value_.c_str(), allocator);
    }
    entry_obj.AddMember("value", entry_value, allocator);

    entries_array.PushBack(entry_obj, allocator);
  }
  document.AddMember("runtime", entries_array, allocator);
  rapidjson::StringBuffer strbuf;
  rapidjson::PrettyWriter<StringBuffer> writer(strbuf);
  document.Accept(writer);
  return strbuf.GetString();
}

void AdminFilter::onComplete() {
  std::string path = request_headers_->Path()->value().c_str();
  ENVOY_STREAM_LOG(debug, "request complete: path: {}", *callbacks_, path);

  Buffer::OwnedImpl response;
  Http::HeaderMapPtr header_map{new Http::HeaderMapImpl};
  Http::Code code = parent_.runCallback(path, *header_map, response);
  header_map->insertStatus().value(std::to_string(enumToInt(code)));
  const auto& headers = Http::Headers::get();
  if (header_map->ContentType() == nullptr) {
    // Default to text-plain if unset.
    header_map->insertContentType().value().setReference(headers.ContentTypeValues.TextUtf8);
  }
  // Default to 'no-cache' if unset, but not 'no-store' which may break the back button.
  if (header_map->CacheControl() == nullptr) {
    header_map->insertCacheControl().value().setReference(
        headers.CacheControlValues.NoCacheMaxAge0);
  }

  // Under no circumstance should browsers sniff content-type.
  header_map->addReference(headers.XContentTypeOptions, headers.XContentTypeOptionValues.Nosniff);
  callbacks_->encodeHeaders(std::move(header_map), response.length() == 0);

  if (response.length() > 0) {
    callbacks_->encodeData(response, true);
  }
}

AdminImpl::NullRouteConfigProvider::NullRouteConfigProvider()
    : config_(new Router::NullConfigImpl()) {}

AdminImpl::AdminImpl(const std::string& access_log_path, const std::string& profile_path,
                     const std::string& address_out_path,
                     Network::Address::InstanceConstSharedPtr address, Server::Instance& server,
                     Stats::Scope& listener_scope)
    : server_(server), profile_path_(profile_path),
      socket_(new Network::TcpListenSocket(address, true)),
      stats_(Http::ConnectionManagerImpl::generateStats("http.admin.", server_.stats())),
      tracing_stats_(Http::ConnectionManagerImpl::generateTracingStats("http.admin.tracing.",
                                                                       server_.stats())),
      handlers_{
          {"/", "Admin home page", MAKE_ADMIN_HANDLER(handlerAdminHome), false, false},
          {"/certs", "print certs on machine", MAKE_ADMIN_HANDLER(handlerCerts), false, false},
          {"/clusters", "upstream cluster status", MAKE_ADMIN_HANDLER(handlerClusters), false,
           false},
          {"/cpuprofiler", "enable/disable the CPU profiler",
           MAKE_ADMIN_HANDLER(handlerCpuProfiler), false, true},
          {"/healthcheck/fail", "cause the server to fail health checks",
           MAKE_ADMIN_HANDLER(handlerHealthcheckFail), false, true},
          {"/healthcheck/ok", "cause the server to pass health checks",
           MAKE_ADMIN_HANDLER(handlerHealthcheckOk), false, true},
          {"/help", "print out list of admin commands", MAKE_ADMIN_HANDLER(handlerHelp), false,
           false},
          {"/hot_restart_version", "print the hot restart compatability version",
           MAKE_ADMIN_HANDLER(handlerHotRestartVersion), false, false},
          {"/logging", "query/change logging levels", MAKE_ADMIN_HANDLER(handlerLogging), false,
           true},
          {"/quitquitquit", "exit the server", MAKE_ADMIN_HANDLER(handlerQuitQuitQuit), false,
           true},
          {"/reset_counters", "reset all counters to zero",
           MAKE_ADMIN_HANDLER(handlerResetCounters), false, true},
          {"/server_info", "print server version/status information",
           MAKE_ADMIN_HANDLER(handlerServerInfo), false, false},
          {"/stats", "print server stats", MAKE_ADMIN_HANDLER(handlerStats), false, false},
          {"/listeners", "print listener addresses", MAKE_ADMIN_HANDLER(handlerListenerInfo), false,
           false},
          {"/runtime", "print runtime values", MAKE_ADMIN_HANDLER(handlerRuntime), false, false}},
      listener_stats_(
          Http::ConnectionManagerImpl::generateListenerStats("http.admin.", listener_scope)) {

  if (!address_out_path.empty()) {
    std::ofstream address_out_file(address_out_path);
    if (!address_out_file) {
      ENVOY_LOG(critical, "cannot open admin address output file {} for writing.",
                address_out_path);
    } else {
      address_out_file << socket_->localAddress()->asString();
    }
  }

  access_logs_.emplace_back(new AccessLog::FileAccessLog(
      access_log_path, {}, AccessLog::AccessLogFormatUtils::defaultAccessLogFormatter(),
      server.accessLogManager()));
}

Http::ServerConnectionPtr AdminImpl::createCodec(Network::Connection& connection,
                                                 const Buffer::Instance&,
                                                 Http::ServerConnectionCallbacks& callbacks) {
  return Http::ServerConnectionPtr{
      new Http::Http1::ServerConnectionImpl(connection, callbacks, Http::Http1Settings())};
}

bool AdminImpl::createFilterChain(Network::Connection& connection) {
  connection.addReadFilter(Network::ReadFilterSharedPtr{new Http::ConnectionManagerImpl(
      *this, server_.drainManager(), server_.random(), server_.httpTracer(), server_.runtime(),
      server_.localInfo(), server_.clusterManager())});
  return true;
}

void AdminImpl::createFilterChain(Http::FilterChainFactoryCallbacks& callbacks) {
  callbacks.addStreamDecoderFilter(Http::StreamDecoderFilterSharedPtr{new AdminFilter(*this)});
}

Http::Code AdminImpl::runCallback(const std::string& path_and_query,
                                  Http::HeaderMap& response_headers, Buffer::Instance& response) {
  Http::Code code = Http::Code::OK;
  bool found_handler = false;

  std::string::size_type query_index = path_and_query.find('?');
  if (query_index == std::string::npos) {
    query_index = path_and_query.size();
  }

  for (const UrlHandler& handler : handlers_) {
    if (path_and_query.compare(0, query_index, handler.prefix_) == 0) {
      code = handler.handler_(path_and_query, response_headers, response);
      found_handler = true;
      break;
    }
  }

  if (!found_handler) {
    // Extra space is emitted below to have "invalid path." be a separate sentence in the
    // 404 output from "admin commands are:" in handlerHelp.
    response.add("invalid path. ");
    handlerHelp(path_and_query, response_headers, response);
    code = Http::Code::NotFound;
  }

  return code;
}

std::vector<const AdminImpl::UrlHandler*> AdminImpl::sortedHandlers() const {
  std::vector<const UrlHandler*> sorted_handlers;
  for (const UrlHandler& handler : handlers_) {
    sorted_handlers.push_back(&handler);
  }
  // Note: it's generally faster to sort a vector with std::vector than to construct a std::map.
  std::sort(sorted_handlers.begin(), sorted_handlers.end(),
            [&](const UrlHandler* h1, const UrlHandler* h2) { return h1->prefix_ < h2->prefix_; });
  return sorted_handlers;
}

Http::Code AdminImpl::handlerHelp(const std::string&, Http::HeaderMap&,
                                  Buffer::Instance& response) {
  response.add("admin commands are:\n");

  // Prefix order is used during searching, but for printing do them in alpha order.
  for (const UrlHandler* handler : sortedHandlers()) {
    response.add(fmt::format("  {}: {}\n", handler->prefix_, handler->help_text_));
  }
  return Http::Code::OK;
}

Http::Code AdminImpl::handlerAdminHome(const std::string&, Http::HeaderMap& response_headers,
                                       Buffer::Instance& response) {
  response_headers.insertContentType().value().setReference(
      Http::Headers::get().ContentTypeValues.Html);

  response.add(absl::StrReplaceAll(AdminHtmlStart, {{"@FAVICON@", EnvoyFavicon}}));

  // Prefix order is used during searching, but for printing do them in alpha order.
  for (const UrlHandler* handler : sortedHandlers()) {
    const std::string& url = handler->prefix_;

    // For handlers that mutate state, render the link as a button in a POST form,
    // rather than an anchor tag. This should discourage crawlers that find the /
    // page from accidentally mutating all the server state by GETting all the hrefs.
    const char* link_format =
        handler->mutates_server_state_
            ? "<form action='{}' method='post' class='home-form'><button>{}</button></form>"
            : "<a href='{}'>{}</a>";
    const std::string link = fmt::format(link_format, url, url);

    // Handlers are all specified by statically above, and are thus trusted and do
    // not require escaping.
    response.add(fmt::format("<tr class='home-row'><td class='home-data'>{}</td>"
                             "<td class='home-data'>{}</td></tr>\n",
                             link, Html::Utility::sanitize(handler->help_text_)));
  }
  response.add(AdminHtmlEnd);
  return Http::Code::OK;
}

const Network::Address::Instance& AdminImpl::localAddress() {
  return *server_.localInfo().address();
}

bool AdminImpl::addHandler(const std::string& prefix, const std::string& help_text,
                           HandlerCb callback, bool removable, bool mutates_state) {
  // Sanitize prefix and help_text to ensure no XSS can be injected, as
  // we are injecting these strings into HTML that runs in a domain that
  // can mutate Envoy server state. Also rule out some characters that
  // make no sense as part of a URL path: ? and :.
  const std::string::size_type pos = prefix.find_first_of("&\"'<>?:");
  if (pos != std::string::npos) {
    ENVOY_LOG(error, "filter \"{}\" contains invalid character '{}'", prefix[pos]);
    return false;
  }

  auto it = std::find_if(handlers_.cbegin(), handlers_.cend(),
                         [&prefix](const UrlHandler& entry) { return prefix == entry.prefix_; });
  if (it == handlers_.end()) {
    handlers_.push_back({prefix, help_text, callback, removable, mutates_state});
    return true;
  }
  return false;
}

bool AdminImpl::removeHandler(const std::string& prefix) {
  const uint size_before_removal = handlers_.size();
  handlers_.remove_if(
      [&prefix](const UrlHandler& entry) { return prefix == entry.prefix_ && entry.removable_; });
  if (handlers_.size() != size_before_removal) {
    return true;
  }
  return false;
}

} // namespace Server
} // namespace Envoy
