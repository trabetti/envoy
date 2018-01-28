#include "envoy/upstream/resource_manager.h"
#include "envoy/upstream/upstream.h"

#include "common/ssl/context_manager_impl.h"
#include "common/stats/stats_impl.h"

#include "server/config_validation/cluster_manager.h"

#include "test/mocks/access_log/mocks.h"
#include "test/mocks/event/mocks.h"
#include "test/mocks/http/mocks.h"
#include "test/mocks/local_info/mocks.h"
#include "test/mocks/network/mocks.h"
#include "test/mocks/runtime/mocks.h"
#include "test/mocks/thread_local/mocks.h"
#include "test/mocks/upstream/mocks.h"
#include "test/test_common/utility.h"

namespace Envoy {
namespace Upstream {

TEST(ValidationClusterManagerTest, MockedMethods) {
  NiceMock<Runtime::MockLoader> runtime;
  Stats::IsolatedStoreImpl stats;
  NiceMock<ThreadLocal::MockInstance> tls;
  NiceMock<Runtime::MockRandomGenerator> random;
  auto dns_resolver = std::make_shared<NiceMock<Network::MockDnsResolver>>();
  Ssl::ContextManagerImpl ssl_context_manager{runtime};
  NiceMock<Event::MockDispatcher> dispatcher;
  LocalInfo::MockLocalInfo local_info;

  ValidationClusterManagerFactory factory(runtime, stats, tls, random, dns_resolver,
                                          ssl_context_manager, dispatcher, local_info);

  AccessLog::MockAccessLogManager log_manager;
  const envoy::api::v2::Bootstrap bootstrap;
  ClusterManagerPtr cluster_manager = factory.clusterManagerFromProto(
      bootstrap, stats, tls, runtime, random, local_info, log_manager);
  EXPECT_EQ(nullptr, cluster_manager->httpConnPoolForCluster("cluster", ResourcePriority::Default,
                                                             Http::Protocol::Http11, nullptr));
  Host::CreateConnectionData data = cluster_manager->tcpConnForCluster("cluster", nullptr);
  EXPECT_EQ(nullptr, data.connection_);
  EXPECT_EQ(nullptr, data.host_description_);

  Http::AsyncClient& client = cluster_manager->httpAsyncClientForCluster("cluster");
  Http::MockAsyncClientStreamCallbacks stream_callbacks;
  EXPECT_EQ(nullptr, client.start(stream_callbacks, Optional<std::chrono::milliseconds>(), false));
}

} // namespace Upstream
} // namespace Envoy
