#include <map>
#include <memory>
#include <vector>

namespace Envoy {
namespace Stats {

typedef std::vector<uint64_t> RollingStats;
typedef std::map<std::string, RollingStats> RollingStatsMap;

// Consider implement the HystrixStats as a sink to have access to histograms data
class Hystrix {

public:
  Hystrix()
      : current_index_(DEFAULT_NUM_OF_BUCKETS - 1), num_of_buckets_(DEFAULT_NUM_OF_BUCKETS){};

  Hystrix(int num_of_buckets)
      : current_index_(num_of_buckets - 1), num_of_buckets_(num_of_buckets){};

  /**
   * Add new value to top of rolling window, pushing out the oldest value
   */
  void pushNewValue(std::string key, int value);

  /**
   * increment pointer of next value to add to rolling window
   */
  void incCounter() { current_index_ = (current_index_ + 1) % num_of_buckets_; }

  /**
   * Generate the streams to be sent to hystrix dashboard
   */
  void getClusterStats(std::stringstream& ss, std::string cluster_name,
                       uint64_t max_concurrent_requests, uint64_t reporting_hosts);

  static uint64_t GetRollingWindowIntervalInMs() { return static_cast<const uint64_t>(ROLLING_WINDOW_IN_MS / DEFAULT_NUM_OF_BUCKETS); }
  static uint64_t GetPingIntervalInMs() { return PING_INTERVAL_IN_MS; }

  /**
   * clear map
   */
  void resetRollingWindow();

private:
  /**
   * Get the statistic's value change over the rolling window time frame
   */
  uint64_t getRollingValue(std::string cluster_name, std::string stats);

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
  void addHystrixThreadPool(std::stringstream& ss, std::string cluster_name, uint64_t queue_size,
                            uint64_t reporting_hosts);

  RollingStatsMap rolling_stats_map_;
  int current_index_;
  int num_of_buckets_;
  // TODO(trabetti): May want to make this configurable via config file
  static const uint64_t DEFAULT_NUM_OF_BUCKETS = 10;
  static const uint64_t ROLLING_WINDOW_IN_MS = 10000;
  static const uint64_t PING_INTERVAL_IN_MS = 3000; // what is good value?
};

typedef std::unique_ptr<Hystrix> HystrixPtr;

} // namespace Stats
} // namespace Envoy
