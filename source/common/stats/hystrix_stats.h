#include <vector>
#include <map>

namespace Envoy {
namespace Stats {

typedef std::vector<uint64_t> RollingStats;
typedef std::map<std::string, RollingStats> RollingStatsMap;

// May want to make this configurable via config file
static const uint64_t  HYSTRIX_NUM_OF_BUCKETS = 10;
static const uint64_t  HYSTRIX_ROLLING_WINDOW_IN_MS = 10000;
static const uint64_t  HYSTRIX_PING_INTERVAL_IN_MS = 3000; // what is good value?

//Consider implement the HystrixStats as a sink to have access to hystograms data
class HystrixStats {

public:
  HystrixStats(int num_of_buckets) : current_index_(num_of_buckets-1),
  	  num_of_buckets_(num_of_buckets) {};
  void pushNewValue(std::string key, int value);
  void incCounter() { current_index_ = (current_index_ + 1)% num_of_buckets_;}
  void getHystrixClusterStats(std::stringstream& ss, std::string cluster_name,
      uint64_t max_concurrent_requests, uint64_t reporting_hosts);

private:
  uint64_t getRollingValue(std::string cluster_name, std::string stats);
  void printRollingWindow();
  void resetRollingWindow();
  void addStringToStream(std::string key, std::string value, std::stringstream& info);
  void addIntToStream(std::string key, uint64_t value, std::stringstream& info);
  void addInfoToStream(std::string key, std::string value, std::stringstream& info);
  void addHystrixCommand(std::stringstream& ss, std::string cluster_name,
      uint64_t max_concurrent_requests, uint64_t reporting_hosts);
  void addHystrixThreadPool(std::stringstream& ss, std::string cluster_name,
  uint64_t queue_size, uint64_t reporting_hosts);

  RollingStatsMap rolling_stats_map_;
  int current_index_;
  int num_of_buckets_;

};

typedef std::unique_ptr<HystrixStats> HystrixStatsPtr;

} // namespace Stats
} // namespace Envoy
