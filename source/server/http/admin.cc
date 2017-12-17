#include "server/http/admin.h"

#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_set>

#include "envoy/filesystem/filesystem.h"
#include "envoy/server/hot_restart.h"
#include "envoy/server/instance.h"
#include "envoy/server/options.h"
#include "envoy/stats/stats.h"
#include "envoy/upstream/cluster_manager.h"
#include "envoy/upstream/upstream.h"

#include "common/buffer/buffer_impl.h"
#include "common/common/assert.h"
#include "common/common/enum_to_int.h"
#include "common/common/utility.h"
#include "common/common/version.h"
#include "common/http/access_log/access_log_formatter.h"
#include "common/http/access_log/access_log_impl.h"
#include "common/http/codes.h"
#include "common/http/header_map_impl.h"
#include "common/http/headers.h"
#include "common/http/http1/codec_impl.h"
#include "common/json/json_loader.h"
#include "common/network/listen_socket_impl.h"
#include "common/profiler/profiler.h"
#include "common/router/config_impl.h"
#include "common/upstream/host_utility.h"

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

AdminFilter::AdminFilter(AdminImpl& parent) : parent_(parent) {}

Http::FilterHeadersStatus AdminFilter::decodeHeaders(Http::HeaderMap& headers, bool end_stream) {
	request_headers_ = &headers;
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

Http::Code AdminImpl::handlerClusters(const std::string&, Buffer::Instance& response) {
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

		for (auto& host : cluster.second.get().hosts()) {
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
					host->address()->asString(), host->outlierDetector().successRate()));
		}
	}

	return Http::Code::OK;
}

Http::Code AdminImpl::handlerCpuProfiler(const std::string& url, Buffer::Instance& response) {
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

Http::Code AdminImpl::handlerHealthcheckFail(const std::string&, Buffer::Instance& response) {
	server_.failHealthcheck(true);
	response.add("OK\n");
	return Http::Code::OK;
}

Http::Code AdminImpl::handlerHealthcheckOk(const std::string&, Buffer::Instance& response) {
	server_.failHealthcheck(false);
	response.add("OK\n");
	return Http::Code::OK;
}

Http::Code AdminImpl::handlerHotRestartVersion(const std::string&, Buffer::Instance& response) {
	response.add(server_.hotRestart().version());
	return Http::Code::OK;
}

Http::Code AdminImpl::handlerLogging(const std::string& url, Buffer::Instance& response) {
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

Http::Code AdminImpl::handlerResetCounters(const std::string&, Buffer::Instance& response) {
	for (const Stats::CounterSharedPtr& counter : server_.stats().counters()) {
		counter->reset();
	}

	response.add("OK\n");
	return Http::Code::OK;
}

Http::Code AdminImpl::handlerServerInfo(const std::string&, Buffer::Instance& response) {
	time_t current_time = time(nullptr);
	response.add(fmt::format("envoy {} {} {} {} {}\n", VersionInfo::version(),
			server_.healthCheckFailed() ? "draining" : "live",
					current_time - server_.startTimeCurrentEpoch(),
					current_time - server_.startTimeFirstEpoch(),
					server_.options().restartEpoch()));
	return Http::Code::OK;
}

Http::Code AdminImpl::handlerStats(const std::string& url, Buffer::Instance& response) {
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
			response.add(statsAsJson(all_stats));
		} else {
			response.add("usage: /stats?format=json \n");
			response.add("\n");
			rc = Http::Code::NotFound;
		}
	}
	return rc;
}

std::string AdminImpl::getOutlierSuccessRateRequestVolume(const Upstream::Outlier::Detector* outlier_detector) {
	if (outlier_detector) {
		return std::to_string(outlier_detector->successRateRequestVolume());
	}
	else
		return "0";
}

std::string AdminImpl::getOutlierBaseEjectionTimeMs(const Upstream::Outlier::Detector* outlier_detector) {
	if (outlier_detector) {
		return std::to_string(outlier_detector->baseEjectionTimeMs());
	}
	else
		return "0";
}

void AdminImpl::addInfoToStream(std::string key, std::string value, std::stringstream& info) {
//void AdminImpl::addInfoToStream(std::string key, std::string value, Buffer::Instance& response) {
	if (info.str().empty())
		info << "data: {";
	else
		info << ", ";
	//info.add(fmt::format("\"{}\": {}",key, value));
	info << "\"" + key + "\": " + value;
}

void accumulateCounters(const Stats::CounterSharedPtr& counter, std::map<std::string, uint64_t>& all_stats) {
	if (all_stats.find(counter->name()) != all_stats.end()) {
		all_stats[counter->name()] += counter->value();
	} else {
		all_stats[counter->name()] = counter->value();
	}
}

void accumulateCounters(const Stats::GaugeSharedPtr& gauge, std::map<std::string, uint64_t>& all_stats) {
	if (all_stats.find(gauge->name()) != all_stats.end()) {
		all_stats[gauge->name()] += gauge->value();
	} else {
		all_stats[gauge->name()] = gauge->value();
	}
}


Http::Code AdminImpl::handlerHystrixEventStream(const std::string& url, Buffer::Instance& response) {
	Http::Code rc = Http::Code::OK;
	const Http::Utility::QueryParams params = Http::Utility::parseQueryString(url);
	std::map<std::string, uint64_t> all_stats;

	Stats::HystrixStats& stats = server_.hystrixStats();
	stats.incCounter();

	// set timer
	//std::chrono::milliseconds window = 5000;
	// starting timer
	server_.setHystrixStreamTimer(std::chrono::milliseconds(5000));

	for (const Stats::CounterSharedPtr& counter : server_.stats().counters()) {
		if (counter->name().find("upstream_rq_") != std::string::npos) {
			std::cout << "counter name: " << counter->name() << ", counter value: " << counter->value() << std::endl;
			accumulateCounters(counter, all_stats);
			stats.pushNewValue(counter->name(), counter->value());
		}
	}

	std::cout << "done reading counters" << std::endl;

	// I think there are no interesting gauges
//	for (const Stats::GaugeSharedPtr& gauge : server_.stats().gauges()) {
//		if (gauge->name().find("rq") != std::string::npos) {
//			std::cout << "gauge name: " << gauge->name() << ", gauge value: " << gauge->value() << std::endl;
//			accumulateCounters(gauge, all_stats);
//		}
//	}

	stats.printRollingWindow();

	for (auto& cluster : server_.clusterManager().clusters()) {
		std::string cluster_name = cluster.second.get().info()->name();
		std::cout << "cluster name: " << cluster_name << std::endl;

//		all_stats.clear();
//		for (const Upstream::HostSharedPtr& host : cluster.second.get().hosts()) {
//			// must go over all counters since counter(name) is not const
//			for (const Stats::CounterSharedPtr& counter : host->counters()) {
//				std::cout << "counter name: " << counter->name() << ", counter value: " << counter->value() << std::endl;
//				accumulateCounters(counter, all_stats);
//			}
//		}
		/*
		 * for (auto& host : cluster.second.get().hosts()) {
			std::map<std::string, uint64_t> all_stats;
			for (const Stats::CounterSharedPtr& counter : host->counters()) {
				all_stats[counter->name()] = counter->value();
		 */

		std::stringstream cluster_info;
		//uint64_t overflowed = server_.stats().counter("upstream_rq_pending_overflow").value();

		std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());	  //  data: {
		addInfoToStream("type", "HystrixCommand", cluster_info);//    "type": "HystrixCommand",
		addInfoToStream("name", cluster.second.get().info()->name(), cluster_info);//    "name": "PlaylistGet",
		addInfoToStream("group", "NA", cluster_info);		//    "group": "PlaylistGet",
		addInfoToStream("currentTime", std::to_string(now), cluster_info);//    "currentTime": 1355239617628,
		addInfoToStream("isCircuitBreakerOpen", "false", cluster_info);//    "isCircuitBreakerOpen": false,
		int errors = stats.getRollingValue("cluster." + cluster_name + ".upstream_rq_5xx");
		int total = stats.getRollingValue("cluster." + cluster_name + ".upstream_rq_total");
		addInfoToStream("errorPercentage", (total == 0 ? "0" : std::to_string(errors/total)) , cluster_info);		//    "errorPercentage": 0,
		addInfoToStream("errorCount", std::to_string(errors)
//				std::to_string(stats.getRollingValue("cluster." + cluster_name + ".upstream_rq_5xx"))
				, cluster_info);//    "errorCount": 0,
//		addInfoToStream("requestCount", std::to_string(all_stats["cluster." + cluster_name + ".upstream_rq_total"]), cluster_info);  //    "requestCount": 121,
//		addInfoToStream("requestCount", std::to_string(stats.getRollingValue("cluster." + cluster_name + ".upstream_rq_total")), cluster_info);  //    "requestCount": 121,
		addInfoToStream("requestCount", std::to_string(total), cluster_info);  //    "requestCount": 121,
		addInfoToStream("rollingCountCollapsedRequests", "0", cluster_info);		//    "rollingCountCollapsedRequests": 0,
		addInfoToStream("rollingCountExceptionsThrown", "0", cluster_info);		//    "rollingCountExceptionsThrown": 0,
//		addInfoToStream("rollingCountFailure", std::to_string(all_stats["cluster." + cluster_name + ".upstream_rq_503"]), cluster_info);  //    "requestCount": 121,
		addInfoToStream("rollingCountFailure", std::to_string(stats.getRollingValue("cluster." + cluster_name + ".upstream_rq_503")), cluster_info);  //    "requestCount": 121,
		addInfoToStream("rollingCountFallbackFailure", "0", cluster_info);//    "rollingCountFallbackFailure": 0,
		addInfoToStream("rollingCountFallbackRejection", "0", cluster_info);//    "rollingCountFallbackRejection": 0,
		addInfoToStream("rollingCountFallbackSuccess", "0", cluster_info);//    "rollingCountFallbackSuccess": 0,
		addInfoToStream("rollingCountclusterInfosFromCache", "0", cluster_info);//    "rollingCountclusterInfosFromCache": 69,
		//    "rollingCountSemaphoreRejected": 0,
		//addInfoToStream("rollingCountShortCircuited", std::to_string(all_stats["cluster." + cluster_name + ".upstream_rq_pending_overflow"]), cluster_info);  //    "requestCount": 121,
		addInfoToStream("rollingCountShortCircuited", std::to_string(stats.getRollingValue("cluster." + cluster_name + ".upstream_rq_pending_overflow")), cluster_info);  //    "requestCount": 121,
		//addInfoToStream("rollingCountSuccess", std::to_string(all_stats["cluster." + cluster_name + ".upstream_rq_2xx"]), cluster_info);  //    "requestCount": 121,
		addInfoToStream("rollingCountSuccess", std::to_string(stats.getRollingValue("cluster." + cluster_name + ".upstream_rq_2xx")), cluster_info);  //    "requestCount": 121,
		//    "rollingCountThreadPoolRejected": 0,
		//addInfoToStream("rollingCountTimeout", std::to_string(all_stats["cluster." + cluster_name + ".upstream_rq_timeout"]), cluster_info);  //    "requestCount": 121,
		addInfoToStream("rollingCountTimeout", std::to_string(stats.getRollingValue("cluster." + cluster_name + ".upstream_rq_timeout")), cluster_info);  //    "requestCount": 121,
		//    "currentConcurrentExecutionCount": 0,
		//    "latencyExecute_mean": 13,
		//    "latencyExecute": {
		//      "0": 3,
		//      "25": 6,
		//      "50": 8,
		//      "75": 14,
		//      "90": 26,
		//      "95": 37,
		//      "99": 75,
		//      "99.5": 92,
		//      "100": 252
		//    },

		addInfoToStream("propertyValue_circuitBreakerRequestVolumeThreshold",
				getOutlierSuccessRateRequestVolume(cluster.second.get().outlierDetector())
				, cluster_info);	//    "propertyValue_circuitBreakerRequestVolumeThreshold": 20,
		addInfoToStream("propertyValue_circuitBreakerErrorThresholdPercentage",
				getOutlierBaseEjectionTimeMs(cluster.second.get().outlierDetector())
				, cluster_info);			//    "propertyValue_circuitBreakerSleepWindowInMilliseconds": 5000,
		//    "propertyValue_circuitBreakerErrorThresholdPercentage": 50,
		addInfoToStream("propertyValue_circuitBreakerForceOpen", "false", cluster_info);//  "propertyValue_circuitBreakerForceOpen": false,
		addInfoToStream("propertyValue_circuitBreakerForceClosed", "false", cluster_info);//  "propertyValue_circuitBreakerForceClosed": false,
// removed from hystrix(?)		addInfoToStream("propertyValue_circuitBreakerEnabled", "true", cluster_info);//    "propertyValue_circuitBreakerEnabled": true,
		//    "propertyValue_executionIsolationStrategy": "THREAD",
		//    "propertyValue_executionIsolationThreadTimeoutInMilliseconds": 800,
		addInfoToStream("propertyValue_executionIsolationThreadInterruptOnTimeout", "false", cluster_info);		//    "propertyValue_executionIsolationThreadInterruptOnTimeout": true,
// removed from hystrix(?)				//    "propertyValue_executionIsolationThreadPoolKeyOverride": null,
		addInfoToStream("propertyValue_executionIsolationSemaphoreMaxConcurrentRequests",
				std::to_string(cluster.second.get().info()->resourceManager(Upstream::ResourcePriority::Default).pendingRequests().max())
		, cluster_info);  //    "propertyValue_executionIsolationSemaphoreMaxConcurrentRequests": 20,
		addInfoToStream("propertyValue_fallbackIsolationSemaphoreMaxConcurrentRequests", "0", cluster_info);//    "propertyValue_fallbackIsolationSemaphoreMaxConcurrentRequests": 10,
		addInfoToStream("propertyValue_metricsRollingStatisticalWindowInMilliseconds",
				std::to_string(cluster.second.get().info()->connectTimeout().count())
						, cluster_info);  //    "propertyValue_metricsRollingStatisticalWindowInMilliseconds": 10000,
		addInfoToStream("propertyValue_requestCacheEnabled", "false", cluster_info);//    "propertyValue_requestCacheEnabled": true,
		//    "propertyValue_requestLogEnabled": true,
		addInfoToStream("reportingHosts", std::to_string(cluster.second.get().hosts().size()), cluster_info);//    "reportingHosts": 1
		//  }

		//  if (params.size() == 0) {
		//    // No Arguments so use the standard.
		//    for (auto stat : all_stats) {
		//      response.add(fmt::format("{}: {}\n", stat.first, stat.second));
		//    }
		//  } else {
		//    const std::string format_key = params.begin()->first;
		//    const std::string format_value = params.begin()->second;
		//    if (format_key == "format" && format_value == "json") {
		//      response.add(BuildEventStream(all_stats));
		//    } else {
		//      response.add("usage: /hystrix_event_stream?format=json \n");
		//      response.add("\n");
		//      rc = Http::Code::NotFound;
		//    }
		//  }
		cluster_info << "}" << std::endl;
		response.add(cluster_info.str());
	}
	return rc;
}

//Http::Code AdminImpl::handlerHystrixEventStream(const std::string& url, Buffer::Instance& response) {
//	Http::Code rc = Http::Code::OK;
//	const Http::Utility::QueryParams params = Http::Utility::parseQueryString(url);
//	std::map<std::string, uint64_t> all_stats;
//
//	for (auto& cluster : server_.clusterManager().clusters()) {
//		all_stats.clear();
//		for (const Upstream::HostSharedPtr& host : cluster.second.get().hosts()) {
//			// must go over all counters since counter(name) is not const
//			for (const Stats::CounterSharedPtr& counter : host->counters()) {
//				std::cout << "counter name: " << counter->name() << ", counter value: " << counter->value() << std::endl;
//				accumulateCounters(counter, all_stats);
//			}
//		}
//		/*
//		 * for (auto& host : cluster.second.get().hosts()) {
//			std::map<std::string, uint64_t> all_stats;
//			for (const Stats::CounterSharedPtr& counter : host->counters()) {
//				all_stats[counter->name()] = counter->value();
//		 */
//
//		std::stringstream clusterInfo;
//		//uint64_t overflowed = server_.stats().counter("upstream_rq_pending_overflow").value();
//
//		std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());	  //  data: {
//		addInfoToStream("type", "HystrixCommand", clusterInfo);//    "type": "HystrixCommand",
//		addInfoToStream("name", cluster.second.get().info()->name(), clusterInfo);//    "name": "PlaylistGet",
//		//    "group": "PlaylistGet",
//		addInfoToStream("currentTime", std::to_string(now), clusterInfo);//    "currentTime": 1355239617628,
//		addInfoToStream("isCircuitBreakerOpen", (all_stats["rq_pending_overflow"] > 0 ? "true" : "false"), clusterInfo);//    "isCircuitBreakerOpen": false,
//		//    "errorPercentage": 0,
//		//    "errorCount": 0,
//		//		addInfoToStream("requestCount", std::to_string(server_.stats().counter("upstream_rq_total").value())
//		//		addInfoToStream("requestCount", std::to_string(server_.stats().counter("rq_total").value())
//		addInfoToStream("requestCount", std::to_string(all_stats["rq_total"]), clusterInfo);  //    "requestCount": 121,
//		addInfoToStream("rollingCountCollapsedRequests", "0", clusterInfo);		//    "rollingCountCollapsedRequests": 0,
//		addInfoToStream("rollingCountExceptionsThrown", "0", clusterInfo);		//    "rollingCountExceptionsThrown": 0,
//		//		addInfoToStream("rollingCountFailure", std::to_string(server_.stats().counter("upstream_rq_5xx").value())
//		//		addInfoToStream("rollingCountFailure", std::to_string(server_.stats().counter("rq_error").value())
//		addInfoToStream("rollingCountFailure", std::to_string(all_stats["rq_error"]), clusterInfo);		//    "rollingCountFailure": 0,
//		addInfoToStream("rollingCountFallbackFailure", "0", clusterInfo);//    "rollingCountFallbackFailure": 0,
//		addInfoToStream("rollingCountFallbackRejection", "0", clusterInfo);//    "rollingCountFallbackRejection": 0,
//		addInfoToStream("rollingCountFallbackSuccess", "0", clusterInfo);//    "rollingCountFallbackSuccess": 0,
//		addInfoToStream("rollingCountclusterInfosFromCache", "0", clusterInfo);//    "rollingCountclusterInfosFromCache": 69,
//		//    "rollingCountSemaphoreRejected": 0,
//		addInfoToStream("rollingCountShortCircuited", std::to_string(all_stats["rq_pending_overflow"]), clusterInfo);  //    "rollingCountShortCircuited": 0,
//		//		addInfoToStream("rollingCountFailure", std::to_string(server_.stats().counter("upstream_rq_2xx").value())
//		//		addInfoToStream("rollingCountFailure", std::to_string(server_.stats().counter("rq_success").value())
//		addInfoToStream("rollingCountSuccess", std::to_string(all_stats["rq_success"]), clusterInfo);		//    "rollingCountSuccess": 121,
//		//    "rollingCountThreadPoolRejected": 0,
//		//		addInfoToStream("rollingCountTimeout", std::to_string(server_.stats().counter("upstream_rq_timeout").value())
//		//		addInfoToStream("rollingCountTimeout", std::to_string(server_.stats().counter("rq_timeout").value())
//		addInfoToStream("rollingCountTimeout", std::to_string(all_stats["rq_timeout"]), clusterInfo); //    "rollingCountTimeout": 0,
//		//    "currentConcurrentExecutionCount": 0,
//		//    "latencyExecute_mean": 13,
//		//    "latencyExecute": {
//		//      "0": 3,
//		//      "25": 6,
//		//      "50": 8,
//		//      "75": 14,
//		//      "90": 26,
//		//      "95": 37,
//		//      "99": 75,
//		//      "99.5": 92,
//		//      "100": 252
//		//    },
//		//    "latencyTotal_mean": 15,
//		//    "latencyTotal": {
//		//      "0": 3,
//		//      "25": 7,
//		//      "50": 10,
//		//      "75": 18,
//		//      "90": 32,
//		//      "95": 43,
//		//      "99": 88,
//		//      "99.5": 160,
//		//      "100": 253
//		//    },
//
//		addInfoToStream("propertyValue_circuitBreakerRequestVolumeThreshold",
//				getOutlierSuccessRateRequestVolume(cluster.second.get().outlierDetector())
//				, clusterInfo);	//    "propertyValue_circuitBreakerRequestVolumeThreshold": 20,
//		addInfoToStream("propertyValue_circuitBreakerErrorThresholdPercentage",
//				getOutlierBaseEjectionTimeMs(cluster.second.get().outlierDetector())
//				, clusterInfo);			//    "propertyValue_circuitBreakerSleepWindowInMilliseconds": 5000,
//		//    "propertyValue_circuitBreakerErrorThresholdPercentage": 50,
//		addInfoToStream("propertyValue_circuitBreakerForceOpen", "false", clusterInfo);//  "propertyValue_circuitBreakerForceOpen": false,
//		addInfoToStream("propertyValue_circuitBreakerForceClosed", "false", clusterInfo);//  "propertyValue_circuitBreakerForceClosed": false,
//		addInfoToStream("propertyValue_circuitBreakerEnabled", "true", clusterInfo);//    "propertyValue_circuitBreakerEnabled": true,
//		//    "propertyValue_executionIsolationStrategy": "THREAD",
//		//    "propertyValue_executionIsolationThreadTimeoutInMilliseconds": 800,
//		addInfoToStream("propertyValue_executionIsolationThreadInterruptOnTimeout", "false", clusterInfo);		//    "propertyValue_executionIsolationThreadInterruptOnTimeout": true,
//		//    "propertyValue_executionIsolationThreadPoolKeyOverride": null,
//		addInfoToStream("propertyValue_executionIsolationSemaphoreMaxConcurrentRequests",
//				std::to_string(cluster.second.get().info()->resourceManager(Upstream::ResourcePriority::Default).pendingRequests().max())
//		, clusterInfo);  //    "propertyValue_executionIsolationSemaphoreMaxConcurrentRequests": 20,
//		addInfoToStream("propertyValue_fallbackIsolationSemaphoreMaxConcurrentRequests", "0", clusterInfo);//    "propertyValue_fallbackIsolationSemaphoreMaxConcurrentRequests": 10,
//		//    "propertyValue_metricsRollingStatisticalWindowInMilliseconds": 10000,
//		addInfoToStream("propertyValue_requestCacheEnabled", "false", clusterInfo);//    "propertyValue_requestCacheEnabled": true,
//		//    "propertyValue_requestLogEnabled": true,
//		addInfoToStream("reportingHosts", "1", clusterInfo);//    "reportingHosts": 1
//		//  }
//
//		//  if (params.size() == 0) {
//		//    // No Arguments so use the standard.
//		//    for (auto stat : all_stats) {
//		//      response.add(fmt::format("{}: {}\n", stat.first, stat.second));
//		//    }
//		//  } else {
//		//    const std::string format_key = params.begin()->first;
//		//    const std::string format_value = params.begin()->second;
//		//    if (format_key == "format" && format_value == "json") {
//		//      response.add(BuildEventStream(all_stats));
//		//    } else {
//		//      response.add("usage: /hystrix_event_stream?format=json \n");
//		//      response.add("\n");
//		//      rc = Http::Code::NotFound;
//		//    }
//		//  }
//		clusterInfo << "}" << std::endl;
//		response.add(clusterInfo.str());
//	}
//	return rc;
//}

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

std::string AdminImpl::BuildEventStream(const std::map<std::string, std::string>& all_stats) {
	//  rapidjson::Document document;
	//  document.SetObject();
	// // rapidjson::Value stats_array(rapidjson::kArrayType);
	//  rapidjson::Value stats_array(rapidjson::kStringType);
	// rapidjson::Document::AllocatorType& allocator = document.GetAllocator();
	//  for (auto stat : all_stats) {
	//    Value stat_obj;
	//    stat_obj.SetObject();
	//    Value stat_name;
	//    stat_name.SetString(stat.first.c_str(), allocator);
	//    stat_obj.AddMember("name", stat_name, allocator);
	//    Value stat_value;
	//    stat_value.SetString(stat.second.c_str(), allocator);
	//    stat_obj.AddMember("value", stat_value, allocator);
	//    stats_array.PushBack(stat_obj, allocator);
	//  }
	//  document.AddMember("stats", stats_array, allocator);
	//  rapidjson::StringBuffer strbuf;
	//  rapidjson::PrettyWriter<StringBuffer> writer(strbuf);
	//  document.Accept(writer);
	//  return strbuf.GetString();



	rapidjson::StringBuffer s;
	rapidjson::Writer<rapidjson::StringBuffer> writer(s);
	writer.StartObject();
	for (auto stat : all_stats) {
		writer.Key(stat.first.c_str());
		writer.String(stat.second.c_str());
	}
	writer.EndObject();
	std::string json_string = s.GetString();

	return json_string;

}

Http::Code AdminImpl::handlerQuitQuitQuit(const std::string&, Buffer::Instance& response) {
	server_.shutdown();
	response.add("OK\n");
	return Http::Code::OK;
}

Http::Code AdminImpl::handlerListenerInfo(const std::string&, Buffer::Instance& response) {
	std::list<std::string> listeners;
	for (auto listener : server_.listenerManager().listeners()) {
		listeners.push_back(listener.get().socket().localAddress()->asString());
	}
	response.add(Json::Factory::listAsJsonString(listeners));
	return Http::Code::OK;
}

Http::Code AdminImpl::handlerCerts(const std::string&, Buffer::Instance& response) {
	// This set is used to track distinct certificates. We may have multiple listeners, upstreams, etc
	// using the same cert.
	std::unordered_set<std::string> context_info_set;
	std::string context_format = "{{\n\t\"ca_cert\": \"{}\",\n\t\"cert_chain\": \"{}\"\n}}\n";
	server_.sslContextManager().iterateContexts([&](Ssl::Context& context) -> void {
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

void AdminFilter::onComplete() {
	std::string path = request_headers_->Path()->value().c_str();
	ENVOY_STREAM_LOG(info, "request complete: path: {}", *callbacks_, path);

	Buffer::OwnedImpl response;
	Http::Code code = parent_.runCallback(path, response);

	Http::HeaderMapPtr headers{
		new Http::HeaderMapImpl{{Http::Headers::get().Status, std::to_string(enumToInt(code))}}};
	callbacks_->encodeHeaders(std::move(headers), response.length() == 0);

	if (response.length() > 0) {
		callbacks_->encodeData(response, true);
	}
}

AdminImpl::NullRouteConfigProvider::NullRouteConfigProvider()
: config_(new Router::NullConfigImpl()) {}

AdminImpl::AdminImpl(const std::string& access_log_path, const std::string& profile_path,
		const std::string& address_out_path,
		Network::Address::InstanceConstSharedPtr address, Server::Instance& server)
: server_(server), profile_path_(profile_path),
  socket_(new Network::TcpListenSocket(address, true)),
  stats_(Http::ConnectionManagerImpl::generateStats("http.admin.", server_.stats())),
  tracing_stats_(Http::ConnectionManagerImpl::generateTracingStats("http.admin.tracing.",
		  server_.stats())),
		  handlers_{
	{"/certs", "print certs on machine", MAKE_ADMIN_HANDLER(handlerCerts), false},
	{"/clusters", "upstream cluster status", MAKE_ADMIN_HANDLER(handlerClusters), false},
	{"/cpuprofiler", "enable/disable the CPU profiler",
			MAKE_ADMIN_HANDLER(handlerCpuProfiler), false},
			{"/healthcheck/fail", "cause the server to fail health checks",
					MAKE_ADMIN_HANDLER(handlerHealthcheckFail), false},
					{"/healthcheck/ok", "cause the server to pass health checks",
							MAKE_ADMIN_HANDLER(handlerHealthcheckOk), false},
							{"/hot_restart_version", "print the hot restart compatability version",
									MAKE_ADMIN_HANDLER(handlerHotRestartVersion), false},
									{"/logging", "query/change logging levels", MAKE_ADMIN_HANDLER(handlerLogging), false},
									{"/quitquitquit", "exit the server", MAKE_ADMIN_HANDLER(handlerQuitQuitQuit), false},
									{"/reset_counters", "reset all counters to zero",
											MAKE_ADMIN_HANDLER(handlerResetCounters), false},
											{"/server_info", "print server version/status information",
													MAKE_ADMIN_HANDLER(handlerServerInfo), false},
													{"/stats", "print server stats", MAKE_ADMIN_HANDLER(handlerStats), false},
													{"/hystrix_event_stream", "print hystrix event stream", MAKE_ADMIN_HANDLER(handlerHystrixEventStream), false},
													{"/listeners", "print listener addresses", MAKE_ADMIN_HANDLER(handlerListenerInfo),
															false}},
															listener_stats_(
																	Http::ConnectionManagerImpl::generateListenerStats("http.admin.", server_.stats())) {

																if (!address_out_path.empty()) {
																	std::ofstream address_out_file(address_out_path);
																	if (!address_out_file) {
																		ENVOY_LOG(critical, "cannot open admin address output file {} for writing.",
																				address_out_path);
																	} else {
																		address_out_file << socket_->localAddress()->asString();
																	}
																}

																access_logs_.emplace_back(new Http::AccessLog::FileAccessLog(
																		access_log_path, {}, Http::AccessLog::AccessLogFormatUtils::defaultAccessLogFormatter(),
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

															Http::Code AdminImpl::runCallback(const std::string& path, Buffer::Instance& response) {
																Http::Code code = Http::Code::OK;
																bool found_handler = false;
																for (const UrlHandler& handler : handlers_) {
																	if (path.find(handler.prefix_) == 0) {
																		code = handler.handler_(path, response);
																		found_handler = true;
																		break;
																	}
																}

																if (!found_handler) {
																	code = Http::Code::NotFound;
																	response.add("envoy admin commands:\n");

																	// Prefix order is used during searching, but for printing do them in alpha order.
																	std::map<std::string, const UrlHandler*> sorted_handlers;
																	for (const UrlHandler& handler : handlers_) {
																		sorted_handlers[handler.prefix_] = &handler;
																	}

																	for (auto handler : sorted_handlers) {
																		response.add(fmt::format("  {}: {}\n", handler.first, handler.second->help_text_));
																	}
																}

																return code;
															}

															const Network::Address::Instance& AdminImpl::localAddress() {
																return *server_.localInfo().address();
															}

															bool AdminImpl::addHandler(const std::string& prefix, const std::string& help_text,
																	HandlerCb callback, bool removable) {
																auto it = std::find_if(handlers_.cbegin(), handlers_.cend(),
																		[&prefix](const UrlHandler& entry) { return prefix == entry.prefix_; });
																if (it == handlers_.end()) {
																	handlers_.push_back({prefix, help_text, callback, removable});
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
