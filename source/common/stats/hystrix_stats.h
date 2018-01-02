#include <string>
#include <iostream>
#include <vector>
#include <map>

namespace Envoy {
namespace Stats {

typedef std::vector<int> RollingStats;

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

private:
	std::map<std::string, RollingStats> rolling_stats_;
	int current_index_;
	int num_of_buckets_;

};


} // namespace Stats
} // namespace Envoy
