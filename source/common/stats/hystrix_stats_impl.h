#include <string>
#include <iostream>
#include <vector>
#include <map>

#include "envoy/stats/hystrix_stats.h"

namespace Envoy {
namespace Stats {

typedef std::vector<int> RollingStats;

class HystrixStatsImpl : public HystrixStats {

public:
	HystrixStatsImpl();
	HystrixStatsImpl(int num_of_buckets);
	//~HystrixStatsImpl();

	void pushNewValue(std::string key, int value) override;
	void incCounter() override { current_index_ = (current_index_ + 1)% num_of_buckets_;}
	int getRollingValue(std::string key) override;

	void updateNumOfBuckets(int new_num_of_buckets) override;
	void printRollingWindow() override;

private:
	std::map<std::string, RollingStats> rolling_stats_;
	int current_index_;
	int num_of_buckets_;

};


} // namespace Stats
} // namespace Envoy
