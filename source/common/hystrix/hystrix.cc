#include "common/hystrix/hystrix.h"
#include "common/buffer/buffer_impl.h"

namespace Envoy {
namespace Hystrix {


void HystrixData::destroy()
{
  if (data_timer_)
    data_timer_->disableTimer();
  if (ping_timer_)
    ping_timer_->disableTimer();
}

void HystrixHandlers::updateHystrixRollingWindow(HystrixData* hystrix_data, Server::Instance& server) {
	hystrix_data->stats_->incCounter();

  for (const Stats::CounterSharedPtr& counter : server.stats().counters()) {
    if (counter->name().find("upstream_rq_") != std::string::npos) {
    	hystrix_data->stats_->pushNewValue(counter->name(), counter->value());
    }
  }
}

void HystrixHandlers::sendKeepAlivePing(HystrixData* hystrix_data) {
  Buffer::OwnedImpl data;
  data.add(":\n\n");

  // using write() since we are sending network level
  (const_cast<Network::Connection*>((hystrix_data->callbacks_)->connection()))->write(data);
  const auto sleepInterval = std::chrono::milliseconds(Stats::HYSTRIX_PING_INTERVAL_IN_MS);

  hystrix_data->ping_timer_->enableTimer(sleepInterval);
}

void HystrixHandlers::prepareAndSendHystrixStream(HystrixData* hystrix_data, Server::Instance& server) {
  updateHystrixRollingWindow(hystrix_data,server);

  std::stringstream ss;

  for (auto& cluster : server.clusterManager().clusters()) {
	hystrix_data->stats_-> getHystrixClusterStats(ss, cluster.second.get().info()->name(),
        cluster.second.get().info()->resourceManager(Upstream::ResourcePriority::Default).pendingRequests().max(),
        cluster.second.get().prioritySet().hostSetsPerPriority().size());
  }

  Buffer::OwnedImpl data;
  data.add(ss.str());

  // using write() since we are sending network level
  (const_cast<Network::Connection*>((hystrix_data->callbacks_)->connection()))->write(data);

  const auto sleepInterval = std::chrono::milliseconds(Stats::HYSTRIX_ROLLING_WINDOW_IN_MS/Stats::HYSTRIX_NUM_OF_BUCKETS);

  hystrix_data->data_timer_->enableTimer(sleepInterval);

}

} // namespace Hystrix
} // namespace Envoy
