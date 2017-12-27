#include "common/stats/hystrix_stats_impl.h"

namespace Envoy {
namespace Stats {

// defaultconstructor
HystrixStatsImpl::HystrixStatsImpl() {
	HystrixStatsImpl(0);
}

// constructor
HystrixStatsImpl::HystrixStatsImpl(int num_of_buckets) {
	num_of_buckets_ = num_of_buckets;
	current_index_ = num_of_buckets-1;
}

// add new value to rolling window, in place of oldest one
void HystrixStatsImpl::pushNewValue(std::string key, int value){
	// create vector if do not exist
	if (rolling_stats_.find(key) == rolling_stats_.end()) {
		rolling_stats_[key].resize(num_of_buckets_,0);
	}
	rolling_stats_[key][current_index_] = value;
}

int HystrixStatsImpl::getRollingValue(std::string key) {
	if (rolling_stats_.find(key) != rolling_stats_.end())
		// TODO: counter may be reset during action
		return rolling_stats_[key][current_index_]-
			rolling_stats_[key][(current_index_+1)%num_of_buckets_];
	else
		return 0;
}

void HystrixStatsImpl::updateNumOfBuckets(int new_num_of_buckets) {
	// TODO: move data - especially if new size is smaller than original size
	for (std::map<std::string, RollingStats>::iterator it=rolling_stats_.begin(); it!=rolling_stats_.end(); ++it) {
	    it->second.resize(new_num_of_buckets);
	}
	num_of_buckets_ = new_num_of_buckets;
}

void HystrixStatsImpl::printRollingWindow() {
	for (std::map<std::string, RollingStats>::iterator it=rolling_stats_.begin(); it!=rolling_stats_.end(); ++it) {
		std::cout << it->first<< " | ";
	    RollingStats rollingStats = it->second;
	    for (int i=0; i< num_of_buckets_; i++) {
	    	std::cout << rollingStats[i] << " | ";
	    }
	    std::cout << std::endl;

	}
}

} // namespace Stats
} // namespace Envoy
