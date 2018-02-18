#include <sstream>

#include "common/stats/hystrix.h"

#include "gtest/gtest.h"

namespace Envoy {
namespace Stats {

TEST(Hystrix, CreateDataMessage) {
	Stats::Hystrix hystrix;
	std::stringstream ss;
	std::string clusterName = "clusterName";
	uint64_t expectedQueueSize = 12;
	uint64_t expectedReportingHosts = 16;
	for (uint64_t i=0;i<10;i++)
	{
		hystrix.pushNewValue("cluster.clusterName.upstream_rq_timeout",i+1);
		hystrix.pushNewValue("cluster.clusterName.upstream_rq_per_try_timeout",(i+1)*2);
		hystrix.pushNewValue("cluster.clusterName.upstream_rq_5xx",(i+1)*3);
		hystrix.pushNewValue("cluster.clusterName.retry.upstream_rq_5xx",(i+1)*4);
		hystrix.pushNewValue("cluster.clusterName.upstream_rq_4xx",(i+1)*5);
		hystrix.pushNewValue("cluster.clusterName.retry.upstream_rq_4xx",(i+1)*6);
		hystrix.pushNewValue("cluster.clusterName.upstream_rq_2xx",(i+1)*7);
		hystrix.pushNewValue("cluster.clusterName.upstream_rq_pending_overflow",(i+1)*8);
	}
	hystrix.getClusterStats(ss,clusterName,expectedQueueSize,expectedReportingHosts);
	std::string dataMessage = ss.str();

	std::string actualErrorPercentage = dataMessage.substr(dataMessage.find("errorPercentage"));
	actualErrorPercentage = actualErrorPercentage.substr(actualErrorPercentage.find(" ")+1);
	std::size_t length = actualErrorPercentage.find(",");
	actualErrorPercentage = actualErrorPercentage.substr(0,length);
	EXPECT_EQ(actualErrorPercentage, std::to_string(80));

	std::string actualErrorCount = dataMessage.substr(dataMessage.find("errorCount"));
	actualErrorCount = actualErrorCount.substr(actualErrorCount.find(" ")+1);
	length = actualErrorCount.find(",");
	actualErrorCount = actualErrorCount.substr(0,length);
	EXPECT_EQ(actualErrorCount, std::to_string(153));

	std::string actualRequestCount = dataMessage.substr(dataMessage.find("requestCount"));
	actualRequestCount = actualRequestCount.substr(actualRequestCount.find(" ")+1);
	length = actualRequestCount.find(",");
	actualRequestCount = actualRequestCount.substr(0,length);
	EXPECT_EQ(actualRequestCount, std::to_string(315));

	std::string actualRejected = dataMessage.substr(dataMessage.find("rollingCountSemaphoreRejected"));
	actualRejected = actualRejected.substr(actualRejected.find(" ")+1);
	length = actualRejected.find(",");
	actualRejected = actualRejected.substr(0,length);
	EXPECT_EQ(actualRejected, std::to_string(72));

	std::string actualSuccess = dataMessage.substr(dataMessage.find("rollingCountSuccess"));
	actualSuccess = actualSuccess.substr(actualSuccess.find(" ")+1);
	length = actualSuccess.find(",");
	actualSuccess = actualSuccess.substr(0,length);
	EXPECT_EQ(actualSuccess, std::to_string(63));

	std::string actualTimeouts = dataMessage.substr(dataMessage.find("rollingCountTimeout"));
	actualTimeouts = actualTimeouts.substr(actualTimeouts.find(" ")+1);
	length = actualTimeouts.find(",");
	actualTimeouts = actualTimeouts.substr(0,length);
	EXPECT_EQ(actualTimeouts, std::to_string(27));

	std::string actualQueueSize = dataMessage.substr(dataMessage.find("propertyValue_queueSizeRejectionThreshold"));
	actualQueueSize = actualQueueSize.substr(actualQueueSize.find(" ")+1);
	length = actualQueueSize.find(",");
	actualQueueSize = actualQueueSize.substr(0,length);
	EXPECT_EQ(actualQueueSize, std::to_string(expectedQueueSize));

	std::string actualReportingHosts = dataMessage.substr(dataMessage.find("reportingHosts"));
	actualReportingHosts = actualReportingHosts.substr(actualReportingHosts.find(" ")+1);
	length = actualReportingHosts.find(",");
	actualReportingHosts = actualReportingHosts.substr(0,length);
	EXPECT_EQ(actualReportingHosts, std::to_string(expectedReportingHosts));
}

} // namespace Stats
} // namespace Envoy
