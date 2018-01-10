#include <vector>
#include <map>

#include "envoy/event/timer.h"

namespace Envoy {
namespace Stats {

typedef std::vector<int> RollingStats;

// May want to make this configurable via config file
static const uint64_t  HYSTRIX_NUM_OF_BUCKETS = 10;
static const uint64_t  HYSTRIX_ROLLING_WINDOW_IN_MS = 10000;
static const uint64_t  HYSTRIX_PING_INTERVAL_IN_MS = 3000; // what is good value?


class HystrixStats {

public:
  HystrixStats();
  HystrixStats(int num_of_buckets);
	//~HystrixStats();

	void pushNewValue(std::string key, int value);
	void incCounter() { current_index_ = (current_index_ + 1)% num_of_buckets_;}
	int getRollingValue(std::string cluster_name, std::string stats);

	void updateNumOfBuckets(int new_num_of_buckets);
	void printRollingWindow();
	void resetRollingWindow();

  Event::TimerPtr hystrix_data_timer_;
  Event::TimerPtr hystrix_ping_timer_;

  void addStringToStream(std::string key, std::string value, std::stringstream& info);
  void addIntToStream(std::string key, uint64_t value, std::stringstream& info);
  void addInfoToStream(std::string key, std::string value, std::stringstream& info);
  void getHystrixClusterStats(std::stringstream& ss, std::string cluster_name,
      uint64_t max_concurrent_requests, uint64_t reporting_hosts);
  void addHystrixCommand(std::stringstream& ss, std::string cluster_name,
      uint64_t max_concurrent_requests, uint64_t reporting_hosts);
  void addHystrixThreadPool(std::stringstream& ss, std::string cluster_name,
  uint64_t queue_size, uint64_t reporting_hosts);

private:
	std::map<std::string, RollingStats> rolling_stats_;
	int current_index_;
	int num_of_buckets_;

};


} // namespace Stats
} // namespace Envoy
