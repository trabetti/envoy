#include "common/stats/hystrix_stats.h"
#include <ctime>
#include <chrono>
#include <sstream>
#include <iostream>

namespace Envoy {
namespace Stats {

// add new value to rolling window, in place of oldest one
void HystrixStats::pushNewValue(std::string key, int value){
  // create vector if do not exist
  if (rolling_stats_map_.find(key) == rolling_stats_map_.end()) {
    rolling_stats_map_[key].resize(num_of_buckets_,0);
  }
  rolling_stats_map_[key][current_index_] = value;
}

uint64_t HystrixStats::getRollingValue(std::string cluster_name, std::string stats) {
  std::string key = "cluster." + cluster_name + "." + stats;
  if (rolling_stats_map_.find(key) != rolling_stats_map_.end())
    // TODO: counter may be reset during action
    return rolling_stats_map_[key][current_index_]-
        rolling_stats_map_[key][(current_index_+1)%num_of_buckets_];
  else
    return 0;
}

void HystrixStats::printRollingWindow() {
  for (auto it=rolling_stats_map_.begin(); it!=rolling_stats_map_.end(); ++it) {
    std::cout << it->first<< " | ";
    RollingStats rollingStats = it->second;
    for (int i=0; i< num_of_buckets_; i++) {
      std::cout << rollingStats[i] << " | ";
    }
    std::cout << std::endl;
  }
}

void HystrixStats::resetRollingWindow() {
  rolling_stats_map_.clear();
}

// TODO: addStringToStream and addIntToStream should call addInfoToStream
void HystrixStats::addStringToStream(std::string key, std::string value, std::stringstream& info) {
  if (!info.str().empty())
    info << ", ";
  info << "\"" + key + "\": \"" + value + "\"";
}

void HystrixStats::addIntToStream(std::string key, uint64_t value, std::stringstream& info) {
  if (!info.str().empty())
    info << ", ";
  info << "\"" + key + "\": " + std::to_string(value);
}

void HystrixStats::addInfoToStream(std::string key, std::string value, std::stringstream& info) {
  if (!info.str().empty())
    info << ", ";
  info << "\"" + key + "\": " + value;
}

void HystrixStats::addHystrixCommand(std::stringstream& ss, std::string cluster_name,
    uint64_t max_concurrent_requests, uint64_t reporting_hosts) {
  std::stringstream cluster_info;

  std::time_t currentTime = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  addStringToStream("type",               "HystrixCommand",           cluster_info);
  addStringToStream("name",               cluster_name,               cluster_info);
  addStringToStream("group",              "NA",                       cluster_info);
  addIntToStream("currentTime",           static_cast<uint64_t>(currentTime), cluster_info);
  addInfoToStream("isCircuitBreakerOpen", "false",                    cluster_info);

  // combining timeouts+retries - retries are counted  as separate requests
  // (alternative: each request including the retries counted as 1)
  double timeouts = getRollingValue(cluster_name, "upstream_rq_timeout")
                    + getRollingValue(cluster_name, "upstream_rq_per_try_timeout");

  // combining errors+retry errors - retries are counted as separate requests
  // (alternative: each request including the retries counted as 1)
  // since timeouts are 504 (or 408), deduce them from here. 
  // timeout retries were not counted here anyway.
  double errors = getRollingValue(cluster_name, "upstream_rq_5xx")
                    + getRollingValue(cluster_name, "retry.upstream_rq_5xx")
                    + getRollingValue(cluster_name, "upstream_rq_4xx")
                    + getRollingValue(cluster_name, "retry.upstream_rq_4xx")
                    - getRollingValue(cluster_name, "upstream_rq_timeout");

  double success = getRollingValue(cluster_name, "upstream_rq_2xx");

  double rejected = getRollingValue(cluster_name, "upstream_rq_pending_overflow");

  //double total = getRollingValue("cluster." + cluster_name + ".upstream_rq_total");
  double total = errors + timeouts + success + rejected;

  double error_rate = total == 0 ? 0 : ((errors + timeouts + rejected)/total)*100;

  addIntToStream("errorPercentage",                 error_rate, cluster_info);
  addIntToStream("errorCount",                      errors,     cluster_info);
  addIntToStream("requestCount",                    total,      cluster_info);
  addIntToStream("rollingCountCollapsedRequests",   0,          cluster_info);
  addIntToStream("rollingCountExceptionsThrown",    0,          cluster_info);
  addIntToStream("rollingCountFailure",             errors,     cluster_info);
  addIntToStream("rollingCountFallbackFailure",     0,          cluster_info);
  addIntToStream("rollingCountFallbackRejection",   0,          cluster_info);
  addIntToStream("rollingCountFallbackSuccess",     0,          cluster_info);
  addIntToStream("rollingCountResponsesFromCache",  0,          cluster_info);

  // Envoy's "circuit breaker" has similar meaning to hystrix's isolation
  // so we count upstream_rq_pending_overflow and present it as rejected
  addIntToStream("rollingCountSemaphoreRejected",   rejected,   cluster_info);

  // Hystrix's short circuit is not similar to Envoy's since it is trrigered by 503 responses
  // there is no parallel counter in Envoy since as a result of errors (outlier detection)
  // requests are not rejected, but rather the node is removed from load balancer healthy pool
  addIntToStream("rollingCountShortCircuited",      0,          cluster_info);
  addIntToStream("rollingCountSuccess",             success,    cluster_info);
  addIntToStream("rollingCountThreadPoolRejected",  0,          cluster_info);
  addIntToStream("rollingCountTimeout",             timeouts,   cluster_info);
  addIntToStream("rollingCountBadRequests",         0,          cluster_info);
  addIntToStream("currentConcurrentExecutionCount", 0,          cluster_info);
  addIntToStream("latencyExecute_mean",             0,          cluster_info);
  addInfoToStream("latencyExecute",
      "{\"0\":0,\"25\":0,\"50\":0,\"75\":0,\"90\":0,\"95\":0,\"99\":0,\"99.5\":0,\"100\":0}",
      cluster_info);
  addIntToStream("propertyValue_circuitBreakerRequestVolumeThreshold",
      0, cluster_info);
  addIntToStream("propertyValue_circuitBreakerSleepWindowInMilliseconds", 0, cluster_info);
  addIntToStream("propertyValue_circuitBreakerErrorThresholdPercentage",
      0, cluster_info);
  addInfoToStream("propertyValue_circuitBreakerForceOpen",      "false",    cluster_info);
  addInfoToStream("propertyValue_circuitBreakerForceClosed",    "true",     cluster_info);
  // removed from hystrix(?)    addInfoToStream("propertyValue_circuitBreakerEnabled", "true", cluster_info);
  addStringToStream("propertyValue_executionIsolationStrategy", "SEMAPHORE", cluster_info);
  addIntToStream("propertyValue_executionIsolationThreadTimeoutInMilliseconds",
      0, cluster_info);
  addInfoToStream("propertyValue_executionIsolationThreadInterruptOnTimeout", "false", cluster_info);
  // removed from hystrix(?)        //    "propertyValue_executionIsolationThreadPoolKeyOverride": null,
  addIntToStream("propertyValue_executionIsolationSemaphoreMaxConcurrentRequests",
      max_concurrent_requests, cluster_info);
  addIntToStream("propertyValue_fallbackIsolationSemaphoreMaxConcurrentRequests", 0, cluster_info);
  addInfoToStream("propertyValue_requestCacheEnabled", "false",           cluster_info);
  addInfoToStream("propertyValue_requestLogEnabled",    "true",           cluster_info);
  addIntToStream("reportingHosts",                      reporting_hosts,  cluster_info);
  addIntToStream("propertyValue_metricsRollingStatisticalWindowInMilliseconds", HYSTRIX_ROLLING_WINDOW_IN_MS, cluster_info);

  ss << "data: {" << cluster_info.str() << "}" << std::endl << std::endl;
}

void HystrixStats::addHystrixThreadPool(std::stringstream& ss, std::string cluster_name,
    uint64_t queue_size, uint64_t reporting_hosts) {
  std::stringstream cluster_info;

  addIntToStream("currentPoolSize",                         0,            cluster_info);
  addIntToStream("rollingMaxActiveThreads",                 0,            cluster_info);
  addIntToStream("currentActiveCount",                      0,            cluster_info);
  addIntToStream("currentCompletedTaskCount",               0,            cluster_info);
  addIntToStream("propertyValue_queueSizeRejectionThreshold", queue_size, cluster_info);
  addStringToStream("type",                          "HystrixThreadPool", cluster_info);

  addIntToStream("reportingHosts",                       reporting_hosts, cluster_info);
  addIntToStream("propertyValue_metricsRollingStatisticalWindowInMilliseconds", HYSTRIX_ROLLING_WINDOW_IN_MS, cluster_info);
  addStringToStream("name",                                 cluster_name, cluster_info);
  addIntToStream("currentLargestPoolSize",                  0,            cluster_info);
  addIntToStream("currentCorePoolSize",                     0,            cluster_info);
  addIntToStream("currentQueueSize",                        0,            cluster_info);
  addIntToStream("currentTaskCount",                        0,            cluster_info);
  addIntToStream("rollingCountThreadsExecuted",             0,            cluster_info);
  addIntToStream("currentMaximumPoolSize",                  0,            cluster_info);

  ss << "data: {" << cluster_info.str() << "}" << std::endl << std::endl;
}

void HystrixStats::getHystrixClusterStats(std::stringstream& ss, std::string cluster_name,
    uint64_t max_concurrent_requests, uint64_t reporting_hosts) {
  addHystrixCommand(ss, cluster_name, max_concurrent_requests, reporting_hosts);
  addHystrixThreadPool(ss, cluster_name, max_concurrent_requests, reporting_hosts);
}


} // namespace Stats
} // namespace Envoy
