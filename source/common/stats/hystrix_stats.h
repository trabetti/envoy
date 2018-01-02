#include <string>
#include <iostream>
#include <vector>
#include <map>

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
	int getRollingValue(std::string key);

	void updateNumOfBuckets(int new_num_of_buckets);
	void printRollingWindow();
	void resetRollingWindow();

private:
	std::map<std::string, RollingStats> rolling_stats_;
	int current_index_;
	int num_of_buckets_;

};


} // namespace Stats
} // namespace Envoy
