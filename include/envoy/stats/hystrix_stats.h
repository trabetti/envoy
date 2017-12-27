#pragma once

#include <string>
#include <iostream>
#include <vector>

#include "envoy/common/pure.h"

namespace Envoy {
namespace Stats {

//typedef std::vector<int> rollingStats;

class HystrixStats {

public:
	virtual ~HystrixStats() {}

	virtual void pushNewValue(std::string key, int value) PURE;
	virtual void incCounter() PURE;
	virtual int getRollingValue(std::string key) PURE;

	virtual void updateNumOfBuckets(int new_num_of_buckets) PURE;
	virtual void printRollingWindow() PURE;

};


} // namespace Stats
} // namespace Envoy
