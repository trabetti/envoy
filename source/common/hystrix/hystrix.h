#pragma once

#include "common/stats/hystrix_stats.h"
#include "envoy/server/admin.h"
#include "envoy/server/instance.h"

namespace Envoy {
namespace Hystrix {

/**
 * Data moved to hystrix_event_stream handler of from admin filter
 */

class HystrixData : public Server::FilterData {
public:
	HystrixData(Http::StreamDecoderFilterCallbacks* callbacks) :  stats_(new Stats::HystrixStats(Stats::HYSTRIX_NUM_OF_BUCKETS)),
									data_timer_(nullptr), ping_timer_(nullptr), callbacks_(callbacks) {}
	void destroy();
	Stats::HystrixStatsPtr stats_;
	Event::TimerPtr data_timer_;
	Event::TimerPtr ping_timer_;
	Http::StreamDecoderFilterCallbacks* callbacks_{};
};

/**
 * Handlers used by hystrix_event_stream handler
 */

class HystrixHandlers {
public:
  static void updateHystrixRollingWindow(HystrixData* hystrix_data, Server::Instance& server);
  static void prepareAndSendHystrixStream(HystrixData* hystrix_data, Server::Instance& server);
  static void sendKeepAlivePing(HystrixData* hystrix_data);
};

} // namespace Lua
} // namespace Envoy
