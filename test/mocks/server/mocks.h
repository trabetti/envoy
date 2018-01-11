#pragma once

#include <chrono>
#include <cstdint>
#include <list>
#include <string>

#include "envoy/server/admin.h"
#include "envoy/server/configuration.h"
#include "envoy/server/drain_manager.h"
#include "envoy/server/filter_config.h"
#include "envoy/server/instance.h"
#include "envoy/server/options.h"
#include "envoy/server/transport_socket_config.h"
#include "envoy/server/worker.h"
#include "envoy/ssl/context_manager.h"

#include "common/ssl/context_manager_impl.h"
#include "common/stats/stats_impl.h"

#include "test/mocks/access_log/mocks.h"
#include "test/mocks/api/mocks.h"
#include "test/mocks/event/mocks.h"
#include "test/mocks/http/mocks.h"
#include "test/mocks/init/mocks.h"
#include "test/mocks/local_info/mocks.h"
#include "test/mocks/network/mocks.h"
#include "test/mocks/router/mocks.h"
#include "test/mocks/runtime/mocks.h"
#include "test/mocks/thread_local/mocks.h"
#include "test/mocks/tracing/mocks.h"
#include "test/mocks/upstream/mocks.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "spdlog/spdlog.h"

namespace Envoy {
namespace Server {

class MockOptions : public Options {
public:
  MockOptions() : MockOptions(std::string()) {}
  MockOptions(const std::string& config_path);
  ~MockOptions();

  MOCK_METHOD0(baseId, uint64_t());
  MOCK_METHOD0(concurrency, uint32_t());
  MOCK_METHOD0(configPath, const std::string&());
  MOCK_METHOD0(v2ConfigOnly, bool());
  MOCK_METHOD0(adminAddressPath, const std::string&());
  MOCK_METHOD0(localAddressIpVersion, Network::Address::IpVersion());
  MOCK_METHOD0(drainTime, std::chrono::seconds());
  MOCK_METHOD0(logLevel, spdlog::level::level_enum());
  MOCK_METHOD0(logPath, const std::string&());
  MOCK_METHOD0(parentShutdownTime, std::chrono::seconds());
  MOCK_METHOD0(restartEpoch, uint64_t());
  MOCK_METHOD0(fileFlushIntervalMsec, std::chrono::milliseconds());
  MOCK_CONST_METHOD0(mode, Mode());
  MOCK_METHOD0(serviceClusterName, const std::string&());
  MOCK_METHOD0(serviceNodeName, const std::string&());
  MOCK_METHOD0(serviceZone, const std::string&());
  MOCK_METHOD0(maxStats, uint64_t());
  MOCK_METHOD0(maxObjNameLength, uint64_t());

  std::string config_path_;
  bool v2_config_only_{};
  std::string admin_address_path_;
  std::string service_cluster_name_;
  std::string service_node_name_;
  std::string service_zone_name_;
  std::string log_path_;
};

class MockAdmin : public Admin {
public:
  MockAdmin();
  ~MockAdmin();

  // Server::Admin
  MOCK_METHOD5(addHandler, bool(const std::string& prefix, const std::string& help_text,
                                HandlerCb callback, bool removable, bool mutates_server_state));
  MOCK_METHOD1(removeHandler, bool(const std::string& prefix));
  MOCK_METHOD0(socket, Network::ListenSocket&());
};

class MockDrainManager : public DrainManager {
public:
  MockDrainManager();
  ~MockDrainManager();

  // Server::DrainManager
  MOCK_CONST_METHOD0(drainClose, bool());
  MOCK_METHOD1(startDrainSequence, void(std::function<void()> completion));
  MOCK_METHOD0(startParentShutdownSequence, void());

  std::function<void()> drain_sequence_completion_;
};

class MockWatchDog : public WatchDog {
public:
  MockWatchDog();
  ~MockWatchDog();

  // Server::WatchDog
  MOCK_METHOD1(startWatchdog, void(Event::Dispatcher& dispatcher));
  MOCK_METHOD0(touch, void());
  MOCK_CONST_METHOD0(threadId, int32_t());
  MOCK_CONST_METHOD0(lastTouchTime, MonotonicTime());
};

class MockGuardDog : public GuardDog {
public:
  MockGuardDog();
  ~MockGuardDog();

  // Server::GuardDog
  MOCK_METHOD1(createWatchDog, WatchDogSharedPtr(int32_t thread_id));
  MOCK_METHOD1(stopWatching, void(WatchDogSharedPtr wd));

  std::shared_ptr<MockWatchDog> watch_dog_;
};

class MockHotRestart : public HotRestart {
public:
  MockHotRestart();
  ~MockHotRestart();

  // Server::HotRestart
  MOCK_METHOD0(drainParentListeners, void());
  MOCK_METHOD1(duplicateParentListenSocket, int(const std::string& address));
  MOCK_METHOD1(getParentStats, void(GetParentStatsInfo& info));
  MOCK_METHOD2(initialize, void(Event::Dispatcher& dispatcher, Server::Instance& server));
  MOCK_METHOD1(shutdownParentAdmin, void(ShutdownParentAdminInfo& info));
  MOCK_METHOD0(terminateParent, void());
  MOCK_METHOD0(shutdown, void());
  MOCK_METHOD0(version, std::string());
};

class MockListenerComponentFactory : public ListenerComponentFactory {
public:
  MockListenerComponentFactory();
  ~MockListenerComponentFactory();

  DrainManagerPtr createDrainManager(envoy::api::v2::Listener::DrainType drain_type) override {
    return DrainManagerPtr{createDrainManager_(drain_type)};
  }

  MOCK_METHOD2(createFilterFactoryList,
               std::vector<Configuration::NetworkFilterFactoryCb>(
                   const Protobuf::RepeatedPtrField<envoy::api::v2::Filter>& filters,
                   Configuration::FactoryContext& context));
  MOCK_METHOD2(createListenSocket,
               Network::ListenSocketSharedPtr(Network::Address::InstanceConstSharedPtr address,
                                              bool bind_to_port));
  MOCK_METHOD1(createDrainManager_, DrainManager*(envoy::api::v2::Listener::DrainType drain_type));
  MOCK_METHOD0(nextListenerTag, uint64_t());

  std::shared_ptr<Network::MockListenSocket> socket_;
};

class MockListenerManager : public ListenerManager {
public:
  MockListenerManager();
  ~MockListenerManager();

  MOCK_METHOD2(addOrUpdateListener, bool(const envoy::api::v2::Listener& config, bool modifiable));
  MOCK_METHOD0(listeners, std::vector<std::reference_wrapper<Listener>>());
  MOCK_METHOD0(numConnections, uint64_t());
  MOCK_METHOD1(removeListener, bool(const std::string& listener_name));
  MOCK_METHOD1(startWorkers, void(GuardDog& guard_dog));
  MOCK_METHOD0(stopListeners, void());
  MOCK_METHOD0(stopWorkers, void());
};

class MockListener : public Listener {
public:
  MockListener();
  ~MockListener();

  MOCK_METHOD0(filterChainFactory, Network::FilterChainFactory&());
  MOCK_METHOD0(socket, Network::ListenSocket&());
  MOCK_METHOD0(defaultSslContext, Ssl::ServerContext*());
  MOCK_METHOD0(useProxyProto, bool());
  MOCK_METHOD0(bindToPort, bool());
  MOCK_METHOD0(useOriginalDst, bool());
  MOCK_METHOD0(perConnectionBufferLimitBytes, uint32_t());
  MOCK_METHOD0(listenerScope, Stats::Scope&());
  MOCK_METHOD0(listenerTag, uint64_t());
  MOCK_CONST_METHOD0(name, const std::string&());

  testing::NiceMock<Network::MockFilterChainFactory> filter_chain_factory_;
  testing::NiceMock<Network::MockListenSocket> socket_;
  Stats::IsolatedStoreImpl scope_;
  std::string name_;
};

class MockWorkerFactory : public WorkerFactory {
public:
  MockWorkerFactory();
  ~MockWorkerFactory();

  // Server::WorkerFactory
  WorkerPtr createWorker() override { return WorkerPtr{createWorker_()}; }

  MOCK_METHOD0(createWorker_, Worker*());
};

class MockWorker : public Worker {
public:
  MockWorker();
  ~MockWorker();

  void callAddCompletion(bool success) {
    EXPECT_NE(nullptr, add_listener_completion_);
    add_listener_completion_(success);
    add_listener_completion_ = nullptr;
  }

  void callRemovalCompletion() {
    EXPECT_NE(nullptr, remove_listener_completion_);
    remove_listener_completion_();
    remove_listener_completion_ = nullptr;
  }

  // Server::Worker
  MOCK_METHOD2(addListener, void(Listener& listener, AddListenerCompletion completion));
  MOCK_METHOD0(numConnections, uint64_t());
  MOCK_METHOD2(removeListener, void(Listener& listener, std::function<void()> completion));
  MOCK_METHOD1(start, void(GuardDog& guard_dog));
  MOCK_METHOD0(stop, void());
  MOCK_METHOD1(stopListener, void(Listener& listener));
  MOCK_METHOD0(stopListeners, void());

  AddListenerCompletion add_listener_completion_;
  std::function<void()> remove_listener_completion_;
};

class MockInstance : public Instance {
public:
  MockInstance();
  ~MockInstance();

  // Server::Instance
  RateLimit::ClientPtr rateLimitClient(const Optional<std::chrono::milliseconds>&) override {
    return RateLimit::ClientPtr{rateLimitClient_()};
  }

  MOCK_METHOD0(admin, Admin&());
  MOCK_METHOD0(api, Api::Api&());
  MOCK_METHOD0(clusterManager, Upstream::ClusterManager&());
  MOCK_METHOD0(sslContextManager, Ssl::ContextManager&());
  MOCK_METHOD0(dispatcher, Event::Dispatcher&());
  MOCK_METHOD0(dnsResolver, Network::DnsResolverSharedPtr());
  MOCK_METHOD0(drainListeners, void());
  MOCK_METHOD0(drainManager, DrainManager&());
  MOCK_METHOD0(accessLogManager, AccessLog::AccessLogManager&());
  MOCK_METHOD1(failHealthcheck, void(bool fail));
  MOCK_METHOD1(getParentStats, void(HotRestart::GetParentStatsInfo&));
  MOCK_METHOD0(healthCheckFailed, bool());
  MOCK_METHOD0(hotRestart, HotRestart&());
  MOCK_METHOD0(initManager, Init::Manager&());
  MOCK_METHOD0(listenerManager, ListenerManager&());
  MOCK_METHOD0(options, Options&());
  MOCK_METHOD0(random, Runtime::RandomGenerator&());
  MOCK_METHOD0(rateLimitClient_, RateLimit::Client*());
  MOCK_METHOD0(runtime, Runtime::Loader&());
  MOCK_METHOD0(shutdown, void());
  MOCK_METHOD0(shutdownAdmin, void());
  MOCK_METHOD0(singletonManager, Singleton::Manager&());
  MOCK_METHOD0(startTimeCurrentEpoch, time_t());
  MOCK_METHOD0(startTimeFirstEpoch, time_t());
  MOCK_METHOD0(stats, Stats::Store&());
  MOCK_METHOD0(httpTracer, Tracing::HttpTracer&());
  MOCK_METHOD0(threadLocal, ThreadLocal::Instance&());
  MOCK_METHOD0(localInfo, const LocalInfo::LocalInfo&());

  testing::NiceMock<ThreadLocal::MockInstance> thread_local_;
  Stats::IsolatedStoreImpl stats_store_;
  testing::NiceMock<Tracing::MockHttpTracer> http_tracer_;
  std::shared_ptr<testing::NiceMock<Network::MockDnsResolver>> dns_resolver_{
      new testing::NiceMock<Network::MockDnsResolver>()};
  testing::NiceMock<Api::MockApi> api_;
  testing::NiceMock<MockAdmin> admin_;
  testing::NiceMock<Upstream::MockClusterManager> cluster_manager_;
  Thread::MutexBasicLockable access_log_lock_;
  testing::NiceMock<Runtime::MockLoader> runtime_loader_;
  Ssl::ContextManagerImpl ssl_context_manager_;
  testing::NiceMock<Event::MockDispatcher> dispatcher_;
  testing::NiceMock<MockDrainManager> drain_manager_;
  testing::NiceMock<AccessLog::MockAccessLogManager> access_log_manager_;
  testing::NiceMock<MockHotRestart> hot_restart_;
  testing::NiceMock<MockOptions> options_;
  testing::NiceMock<Runtime::MockRandomGenerator> random_;
  testing::NiceMock<LocalInfo::MockLocalInfo> local_info_;
  testing::NiceMock<Init::MockManager> init_manager_;
  testing::NiceMock<MockListenerManager> listener_manager_;
  Singleton::ManagerPtr singleton_manager_;
};

namespace Configuration {

class MockMain : public Main {
public:
  MockMain() : MockMain(0, 0, 0, 0) {}
  MockMain(int wd_miss, int wd_megamiss, int wd_kill, int wd_multikill);

  MOCK_METHOD0(clusterManager, Upstream::ClusterManager&());
  MOCK_METHOD0(httpTracer, Tracing::HttpTracer&());
  MOCK_METHOD0(rateLimitClientFactory, RateLimit::ClientFactory&());
  MOCK_METHOD0(statsSinks, std::list<Stats::SinkPtr>&());
  MOCK_METHOD0(statsFlushInterval, std::chrono::milliseconds());
  MOCK_CONST_METHOD0(wdMissTimeout, std::chrono::milliseconds());
  MOCK_CONST_METHOD0(wdMegaMissTimeout, std::chrono::milliseconds());
  MOCK_CONST_METHOD0(wdKillTimeout, std::chrono::milliseconds());
  MOCK_CONST_METHOD0(wdMultiKillTimeout, std::chrono::milliseconds());

  std::chrono::milliseconds wd_miss_;
  std::chrono::milliseconds wd_megamiss_;
  std::chrono::milliseconds wd_kill_;
  std::chrono::milliseconds wd_multikill_;
};

class MockFactoryContext : public FactoryContext {
public:
  MockFactoryContext();
  ~MockFactoryContext();

  RateLimit::ClientPtr rateLimitClient(const Optional<std::chrono::milliseconds>&) override {
    return RateLimit::ClientPtr{rateLimitClient_()};
  }

  MOCK_METHOD0(accessLogManager, AccessLog::AccessLogManager&());
  MOCK_METHOD0(clusterManager, Upstream::ClusterManager&());
  MOCK_METHOD0(dispatcher, Event::Dispatcher&());
  MOCK_METHOD0(drainDecision, const Network::DrainDecision&());
  MOCK_METHOD0(healthCheckFailed, bool());
  MOCK_METHOD0(httpTracer, Tracing::HttpTracer&());
  MOCK_METHOD0(initManager, Init::Manager&());
  MOCK_METHOD0(localInfo, const LocalInfo::LocalInfo&());
  MOCK_METHOD0(random, Envoy::Runtime::RandomGenerator&());
  MOCK_METHOD0(rateLimitClient_, RateLimit::Client*());
  MOCK_METHOD0(runtime, Envoy::Runtime::Loader&());
  MOCK_METHOD0(scope, Stats::Scope&());
  MOCK_METHOD0(singletonManager, Singleton::Manager&());
  MOCK_METHOD0(threadLocal, ThreadLocal::Instance&());
  MOCK_METHOD0(admin, Server::Admin&());
  MOCK_METHOD0(listenerScope, Stats::Scope&());

  testing::NiceMock<AccessLog::MockAccessLogManager> access_log_manager_;
  testing::NiceMock<Upstream::MockClusterManager> cluster_manager_;
  testing::NiceMock<Event::MockDispatcher> dispatcher_;
  testing::NiceMock<MockDrainManager> drain_manager_;
  testing::NiceMock<Tracing::MockHttpTracer> http_tracer_;
  testing::NiceMock<Init::MockManager> init_manager_;
  testing::NiceMock<LocalInfo::MockLocalInfo> local_info_;
  testing::NiceMock<Envoy::Runtime::MockRandomGenerator> random_;
  testing::NiceMock<Envoy::Runtime::MockLoader> runtime_loader_;
  Stats::IsolatedStoreImpl scope_;
  testing::NiceMock<ThreadLocal::MockInstance> thread_local_;
  Singleton::ManagerPtr singleton_manager_;
  testing::NiceMock<MockAdmin> admin_;
  Stats::IsolatedStoreImpl listener_scope_;
};

class MockTransportSocketFactoryContext : public TransportSocketFactoryContext {
public:
  MockTransportSocketFactoryContext();
  ~MockTransportSocketFactoryContext();

  MOCK_METHOD0(sslContextManager, Ssl::ContextManager&());
  MOCK_CONST_METHOD0(statsScope, Stats::Scope&());
};

} // namespace Configuration
} // namespace Server
} // namespace Envoy
