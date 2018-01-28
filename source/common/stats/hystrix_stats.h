#include <vector>
#include <map>
#include <memory>

namespace Envoy {
namespace Stats {

typedef std::vector<uint64_t> RollingStats;
typedef std::map<std::string, RollingStats> RollingStatsMap;

// May want to make this configurable via config file
static const uint64_t  HYSTRIX_NUM_OF_BUCKETS = 10;
static const uint64_t  HYSTRIX_ROLLING_WINDOW_IN_MS = 10000;
static const uint64_t  HYSTRIX_PING_INTERVAL_IN_MS = 3000; // what is good value?

//Consider implement the HystrixStats as a sink to have access to histograms data
class HystrixStats {

public:
  HystrixStats(int num_of_buckets) : current_index_(num_of_buckets-1), num_of_buckets_(num_of_buckets) {};

  /**
   * Add new value to top of rolling window, pushing out the oldest value
   */
  void pushNewValue(std::string key, int value);

  /**
   * increment pointer of next value to add to rolling window
   */
  void incCounter() { current_index_ = (current_index_ + 1)% num_of_buckets_;}


  /**
   * Generate the streams to be sent to hystrix dashboard
   */
  void getHystrixClusterStats(std::stringstream& ss, std::string cluster_name,
      uint64_t max_concurrent_requests, uint64_t reporting_hosts);

private:
  /**
   * Get the statistic's value change over the rolling window time frame
   */
  uint64_t getRollingValue(std::string cluster_name, std::string stats);

  /**
   * for debug. should be removed
   */
  void printRollingWindow();

  /**
   * clear map
   */
  void resetRollingWindow();

  /**
   * format a string to match stream
   */
  void addStringToStream(std::string key, std::string value, std::stringstream& info);


  /**
   * format a uint64_t to match stream
   */
  void addIntToStream(std::string key, uint64_t value, std::stringstream& info);

  /**
   * format a value to match stream
   */
  void addInfoToStream(std::string key, std::string value, std::stringstream& info);

  /**
   * generate HystrixCommand stream
   */
  void addHystrixCommand(std::stringstream& ss, std::string cluster_name,
      uint64_t max_concurrent_requests, uint64_t reporting_hosts);

  /**
   * generate HystrixThreadPool stream
   */
  void addHystrixThreadPool(std::stringstream& ss, std::string cluster_name,
      uint64_t queue_size, uint64_t reporting_hosts);

  RollingStatsMap rolling_stats_map_;
  int current_index_;
  int num_of_buckets_;

};

typedef std::unique_ptr<HystrixStats> HystrixStatsPtr;

} // namespace Stats
} // namespace Envoy
