#include <chrono>
#include <cstdint>
#include <list>
#include <string>
#include <tuple>
#include <vector>

#include "envoy/api/api.h"
#include "envoy/http/codec.h"
#include "envoy/upstream/cluster_manager.h"

#include "common/config/metadata.h"
#include "common/json/config_schemas.h"
#include "common/json/json_loader.h"
#include "common/network/utility.h"
#include "common/upstream/upstream_impl.h"

#include "test/common/upstream/utility.h"
#include "test/mocks/common.h"
#include "test/mocks/network/mocks.h"
#include "test/mocks/runtime/mocks.h"
#include "test/mocks/ssl/mocks.h"
#include "test/mocks/upstream/mocks.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::ContainerEq;
using testing::Invoke;
using testing::NiceMock;
using testing::_;

namespace Envoy {
namespace Upstream {
namespace {

std::list<std::string> hostListToAddresses(const std::vector<HostSharedPtr>& hosts) {
  std::list<std::string> addresses;
  for (const HostSharedPtr& host : hosts) {
    addresses.push_back(host->address()->asString());
  }

  return addresses;
}

struct ResolverData {
  ResolverData(Network::MockDnsResolver& dns_resolver, Event::MockDispatcher& dispatcher) {
    timer_ = new Event::MockTimer(&dispatcher);
    expectResolve(dns_resolver);
  }

  void expectResolve(Network::MockDnsResolver& dns_resolver) {
    EXPECT_CALL(dns_resolver, resolve(_, _, _))
        .WillOnce(Invoke([&](const std::string&, Network::DnsLookupFamily,
                             Network::DnsResolver::ResolveCb cb) -> Network::ActiveDnsQuery* {
          dns_callback_ = cb;
          return &active_dns_query_;
        }))
        .RetiresOnSaturation();
  }

  Event::MockTimer* timer_;
  Network::DnsResolver::ResolveCb dns_callback_;
  Network::MockActiveDnsQuery active_dns_query_;
};

typedef std::tuple<std::string, Network::DnsLookupFamily, std::list<std::string>>
    StrictDnsConfigTuple;
std::vector<StrictDnsConfigTuple> generateStrictDnsParams() {
  std::vector<StrictDnsConfigTuple> dns_config;
  {
    std::string family_json("");
    Network::DnsLookupFamily family(Network::DnsLookupFamily::V4Only);
    std::list<std::string> dns_response{"127.0.0.1", "127.0.0.2"};
    dns_config.push_back(std::make_tuple(family_json, family, dns_response));
  }
  {
    std::string family_json(R"EOF("dns_lookup_family": "v4_only",)EOF");
    Network::DnsLookupFamily family(Network::DnsLookupFamily::V4Only);
    std::list<std::string> dns_response{"127.0.0.1", "127.0.0.2"};
    dns_config.push_back(std::make_tuple(family_json, family, dns_response));
  }
  {
    std::string family_json(R"EOF("dns_lookup_family": "v6_only",)EOF");
    Network::DnsLookupFamily family(Network::DnsLookupFamily::V6Only);
    std::list<std::string> dns_response{"::1", "::2"};
    dns_config.push_back(std::make_tuple(family_json, family, dns_response));
  }
  {
    std::string family_json(R"EOF("dns_lookup_family": "auto",)EOF");
    Network::DnsLookupFamily family(Network::DnsLookupFamily::Auto);
    std::list<std::string> dns_response{"127.0.0.1", "127.0.0.2"};
    dns_config.push_back(std::make_tuple(family_json, family, dns_response));
  }
  return dns_config;
}

class StrictDnsParamTest : public testing::TestWithParam<StrictDnsConfigTuple> {};

INSTANTIATE_TEST_CASE_P(DnsParam, StrictDnsParamTest, testing::ValuesIn(generateStrictDnsParams()));

TEST_P(StrictDnsParamTest, ImmediateResolve) {
  Stats::IsolatedStoreImpl stats;
  Ssl::MockContextManager ssl_context_manager;
  auto dns_resolver = std::make_shared<NiceMock<Network::MockDnsResolver>>();
  NiceMock<Event::MockDispatcher> dispatcher;
  NiceMock<Runtime::MockLoader> runtime;
  ReadyWatcher initialized;

  const std::string json = R"EOF(
  {
    "name": "name",
    "connect_timeout_ms": 250,
    "type": "strict_dns",
  )EOF" + std::get<0>(GetParam()) +
                           R"EOF(
    "lb_type": "round_robin",
    "hosts": [{"url": "tcp://foo.bar.com:443"}]
  }
  )EOF";
  EXPECT_CALL(initialized, ready());
  EXPECT_CALL(*dns_resolver, resolve("foo.bar.com", std::get<1>(GetParam()), _))
      .WillOnce(Invoke([&](const std::string&, Network::DnsLookupFamily,
                           Network::DnsResolver::ResolveCb cb) -> Network::ActiveDnsQuery* {
        cb(TestUtility::makeDnsResponse(std::get<2>(GetParam())));
        return nullptr;
      }));
  NiceMock<MockClusterManager> cm;
  StrictDnsClusterImpl cluster(parseClusterFromJson(json), runtime, stats, ssl_context_manager,
                               dns_resolver, cm, dispatcher, false);
  cluster.initialize([&]() -> void { initialized.ready(); });
  EXPECT_EQ(2UL, cluster.prioritySet().hostSetsPerPriority()[0]->hosts().size());
  EXPECT_EQ(2UL, cluster.prioritySet().hostSetsPerPriority()[0]->healthyHosts().size());
}

// Resolve zero hosts, while using health checking.
TEST(StrictDnsClusterImplTest, ZeroHostsHealthChecker) {
  Stats::IsolatedStoreImpl stats;
  Ssl::MockContextManager ssl_context_manager;
  auto dns_resolver = std::make_shared<Network::MockDnsResolver>();
  NiceMock<Event::MockDispatcher> dispatcher;
  NiceMock<Runtime::MockLoader> runtime;
  NiceMock<MockClusterManager> cm;
  ReadyWatcher initialized;

  const std::string yaml = R"EOF(
    name: name
    connect_timeout: 0.25s
    type: STRICT_DNS
    lb_policy: ROUND_ROBIN
    hosts: [{ socket_address: { address: foo.bar.com, port_value: 443 }}]
  )EOF";

  ResolverData resolver(*dns_resolver, dispatcher);
  StrictDnsClusterImpl cluster(parseClusterFromV2Yaml(yaml), runtime, stats, ssl_context_manager,
                               dns_resolver, cm, dispatcher, false);
  std::shared_ptr<MockHealthChecker> health_checker(new MockHealthChecker());
  EXPECT_CALL(*health_checker, start());
  EXPECT_CALL(*health_checker, addHostCheckCompleteCb(_));
  cluster.setHealthChecker(health_checker);
  cluster.initialize([&]() -> void { initialized.ready(); });

  EXPECT_CALL(*health_checker, addHostCheckCompleteCb(_));
  EXPECT_CALL(initialized, ready());
  EXPECT_CALL(*resolver.timer_, enableTimer(_));
  resolver.dns_callback_({});
  EXPECT_EQ(0UL, cluster.prioritySet().hostSetsPerPriority()[0]->hosts().size());
  EXPECT_EQ(0UL, cluster.prioritySet().hostSetsPerPriority()[0]->healthyHosts().size());
}

TEST(StrictDnsClusterImplTest, Basic) {
  Stats::IsolatedStoreImpl stats;
  Ssl::MockContextManager ssl_context_manager;
  auto dns_resolver = std::make_shared<NiceMock<Network::MockDnsResolver>>();
  NiceMock<Event::MockDispatcher> dispatcher;
  NiceMock<Runtime::MockLoader> runtime;

  // gmock matches in LIFO order which is why these are swapped.
  ResolverData resolver2(*dns_resolver, dispatcher);
  ResolverData resolver1(*dns_resolver, dispatcher);

  const std::string json = R"EOF(
  {
    "name": "name",
    "connect_timeout_ms": 250,
    "type": "strict_dns",
    "dns_refresh_rate_ms": 4000,
    "lb_type": "round_robin",
    "circuit_breakers": {
      "default": {
        "max_connections": 43,
        "max_pending_requests": 57,
        "max_requests": 50,
        "max_retries": 10
      },
      "high": {
        "max_connections": 1,
        "max_pending_requests": 2,
        "max_requests": 3,
        "max_retries": 4
      }
    },
    "max_requests_per_connection": 3,
    "http2_settings": {
       "hpack_table_size": 0
     },
    "hosts": [{"url": "tcp://localhost1:11001"},
              {"url": "tcp://localhost2:11002"}]
  }
  )EOF";

  NiceMock<MockClusterManager> cm;
  StrictDnsClusterImpl cluster(parseClusterFromJson(json), runtime, stats, ssl_context_manager,
                               dns_resolver, cm, dispatcher, false);
  EXPECT_CALL(runtime.snapshot_, getInteger("circuit_breakers.name.default.max_connections", 43));
  EXPECT_EQ(43U, cluster.info()->resourceManager(ResourcePriority::Default).connections().max());
  EXPECT_CALL(runtime.snapshot_,
              getInteger("circuit_breakers.name.default.max_pending_requests", 57));
  EXPECT_EQ(57U,
            cluster.info()->resourceManager(ResourcePriority::Default).pendingRequests().max());
  EXPECT_CALL(runtime.snapshot_, getInteger("circuit_breakers.name.default.max_requests", 50));
  EXPECT_EQ(50U, cluster.info()->resourceManager(ResourcePriority::Default).requests().max());
  EXPECT_CALL(runtime.snapshot_, getInteger("circuit_breakers.name.default.max_retries", 10));
  EXPECT_EQ(10U, cluster.info()->resourceManager(ResourcePriority::Default).retries().max());
  EXPECT_CALL(runtime.snapshot_, getInteger("circuit_breakers.name.high.max_connections", 1));
  EXPECT_EQ(1U, cluster.info()->resourceManager(ResourcePriority::High).connections().max());
  EXPECT_CALL(runtime.snapshot_, getInteger("circuit_breakers.name.high.max_pending_requests", 2));
  EXPECT_EQ(2U, cluster.info()->resourceManager(ResourcePriority::High).pendingRequests().max());
  EXPECT_CALL(runtime.snapshot_, getInteger("circuit_breakers.name.high.max_requests", 3));
  EXPECT_EQ(3U, cluster.info()->resourceManager(ResourcePriority::High).requests().max());
  EXPECT_CALL(runtime.snapshot_, getInteger("circuit_breakers.name.high.max_retries", 4));
  EXPECT_EQ(4U, cluster.info()->resourceManager(ResourcePriority::High).retries().max());
  EXPECT_EQ(3U, cluster.info()->maxRequestsPerConnection());
  EXPECT_EQ(0U, cluster.info()->http2Settings().hpack_table_size_);

  cluster.info()->stats().upstream_rq_total_.inc();
  EXPECT_EQ(1UL, stats.counter("cluster.name.upstream_rq_total").value());

  EXPECT_CALL(runtime.snapshot_, featureEnabled("upstream.maintenance_mode.name", 0));
  EXPECT_FALSE(cluster.info()->maintenanceMode());

  ReadyWatcher membership_updated;
  cluster.prioritySet().addMemberUpdateCb(
      [&](uint32_t, const std::vector<HostSharedPtr>&, const std::vector<HostSharedPtr>&) -> void {
        membership_updated.ready();
      });

  cluster.initialize([] {});

  resolver1.expectResolve(*dns_resolver);
  EXPECT_CALL(*resolver1.timer_, enableTimer(std::chrono::milliseconds(4000)));
  EXPECT_CALL(membership_updated, ready());
  resolver1.dns_callback_(TestUtility::makeDnsResponse({"127.0.0.1", "127.0.0.2"}));
  EXPECT_THAT(
      std::list<std::string>({"127.0.0.1:11001", "127.0.0.2:11001"}),
      ContainerEq(hostListToAddresses(cluster.prioritySet().hostSetsPerPriority()[0]->hosts())));
  EXPECT_EQ("localhost1", cluster.prioritySet().hostSetsPerPriority()[0]->hosts()[0]->hostname());
  EXPECT_EQ("localhost1", cluster.prioritySet().hostSetsPerPriority()[0]->hosts()[1]->hostname());

  resolver1.expectResolve(*dns_resolver);
  resolver1.timer_->callback_();
  EXPECT_CALL(*resolver1.timer_, enableTimer(std::chrono::milliseconds(4000)));
  resolver1.dns_callback_(TestUtility::makeDnsResponse({"127.0.0.2", "127.0.0.1"}));
  EXPECT_THAT(
      std::list<std::string>({"127.0.0.1:11001", "127.0.0.2:11001"}),
      ContainerEq(hostListToAddresses(cluster.prioritySet().hostSetsPerPriority()[0]->hosts())));

  resolver1.expectResolve(*dns_resolver);
  resolver1.timer_->callback_();
  EXPECT_CALL(*resolver1.timer_, enableTimer(std::chrono::milliseconds(4000)));
  resolver1.dns_callback_(TestUtility::makeDnsResponse({"127.0.0.2", "127.0.0.1"}));
  EXPECT_THAT(
      std::list<std::string>({"127.0.0.1:11001", "127.0.0.2:11001"}),
      ContainerEq(hostListToAddresses(cluster.prioritySet().hostSetsPerPriority()[0]->hosts())));

  resolver1.timer_->callback_();
  EXPECT_CALL(*resolver1.timer_, enableTimer(std::chrono::milliseconds(4000)));
  EXPECT_CALL(membership_updated, ready());
  resolver1.dns_callback_(TestUtility::makeDnsResponse({"127.0.0.3"}));
  EXPECT_THAT(
      std::list<std::string>({"127.0.0.3:11001"}),
      ContainerEq(hostListToAddresses(cluster.prioritySet().hostSetsPerPriority()[0]->hosts())));

  // Make sure we de-dup the same address.
  EXPECT_CALL(*resolver2.timer_, enableTimer(std::chrono::milliseconds(4000)));
  EXPECT_CALL(membership_updated, ready());
  resolver2.dns_callback_(TestUtility::makeDnsResponse({"10.0.0.1", "10.0.0.1"}));
  EXPECT_THAT(
      std::list<std::string>({"127.0.0.3:11001", "10.0.0.1:11002"}),
      ContainerEq(hostListToAddresses(cluster.prioritySet().hostSetsPerPriority()[0]->hosts())));

  EXPECT_EQ(2UL, cluster.prioritySet().hostSetsPerPriority()[0]->healthyHosts().size());
  EXPECT_EQ(0UL, cluster.prioritySet().hostSetsPerPriority()[0]->hostsPerLocality().size());
  EXPECT_EQ(0UL, cluster.prioritySet().hostSetsPerPriority()[0]->healthyHostsPerLocality().size());

  for (const HostSharedPtr& host : cluster.prioritySet().hostSetsPerPriority()[0]->hosts()) {
    EXPECT_EQ(cluster.info().get(), &host->cluster());
  }

  // Make sure we cancel.
  resolver1.expectResolve(*dns_resolver);
  resolver1.timer_->callback_();
  resolver2.expectResolve(*dns_resolver);
  resolver2.timer_->callback_();

  EXPECT_CALL(resolver1.active_dns_query_, cancel());
  EXPECT_CALL(resolver2.active_dns_query_, cancel());
}

TEST(HostImplTest, HostCluster) {
  MockCluster cluster;
  HostSharedPtr host = makeTestHost(cluster.info_, "tcp://10.0.0.1:1234", 1);
  EXPECT_EQ(cluster.info_.get(), &host->cluster());
  EXPECT_EQ("", host->hostname());
  EXPECT_FALSE(host->canary());
  EXPECT_EQ("", host->locality().zone());
}

TEST(HostImplTest, Weight) {
  MockCluster cluster;

  EXPECT_EQ(1U, makeTestHost(cluster.info_, "tcp://10.0.0.1:1234", 0)->weight());
  EXPECT_EQ(128U, makeTestHost(cluster.info_, "tcp://10.0.0.1:1234", 128)->weight());
  EXPECT_EQ(128U, makeTestHost(cluster.info_, "tcp://10.0.0.1:1234", 129)->weight());

  HostSharedPtr host = makeTestHost(cluster.info_, "tcp://10.0.0.1:1234", 50);
  EXPECT_EQ(50U, host->weight());
  host->weight(51);
  EXPECT_EQ(51U, host->weight());
  host->weight(0);
  EXPECT_EQ(1U, host->weight());
  host->weight(129);
  EXPECT_EQ(128U, host->weight());
}

TEST(HostImplTest, HostnameCanaryAndLocality) {
  MockCluster cluster;
  envoy::api::v2::Metadata metadata;
  Config::Metadata::mutableMetadataValue(metadata, Config::MetadataFilters::get().ENVOY_LB,
                                         Config::MetadataEnvoyLbKeys::get().CANARY)
      .set_bool_value(true);
  envoy::api::v2::Locality locality;
  locality.set_region("oceania");
  locality.set_zone("hello");
  locality.set_sub_zone("world");
  HostImpl host(cluster.info_, "lyft.com", Network::Utility::resolveUrl("tcp://10.0.0.1:1234"),
                metadata, 1, locality);
  EXPECT_EQ(cluster.info_.get(), &host.cluster());
  EXPECT_EQ("lyft.com", host.hostname());
  EXPECT_TRUE(host.canary());
  EXPECT_EQ("oceania", host.locality().region());
  EXPECT_EQ("hello", host.locality().zone());
  EXPECT_EQ("world", host.locality().sub_zone());
}

TEST(StaticClusterImplTest, EmptyHostname) {
  Stats::IsolatedStoreImpl stats;
  Ssl::MockContextManager ssl_context_manager;
  NiceMock<Runtime::MockLoader> runtime;
  const std::string json = R"EOF(
  {
    "name": "staticcluster",
    "connect_timeout_ms": 250,
    "type": "static",
    "lb_type": "random",
    "hosts": [{"url": "tcp://10.0.0.1:11001"}]
  }
  )EOF";

  NiceMock<MockClusterManager> cm;
  StaticClusterImpl cluster(parseClusterFromJson(json), runtime, stats, ssl_context_manager, cm,
                            false);
  cluster.initialize([] {});

  EXPECT_EQ(1UL, cluster.prioritySet().hostSetsPerPriority()[0]->healthyHosts().size());
  EXPECT_EQ("", cluster.prioritySet().hostSetsPerPriority()[0]->hosts()[0]->hostname());
  EXPECT_FALSE(cluster.info()->addedViaApi());
}

TEST(StaticClusterImplTest, RingHash) {
  Stats::IsolatedStoreImpl stats;
  Ssl::MockContextManager ssl_context_manager;
  NiceMock<Runtime::MockLoader> runtime;
  const std::string json = R"EOF(
  {
    "name": "staticcluster",
    "connect_timeout_ms": 250,
    "type": "static",
    "lb_type": "ring_hash",
    "hosts": [{"url": "tcp://10.0.0.1:11001"}]
  }
  )EOF";

  NiceMock<MockClusterManager> cm;
  StaticClusterImpl cluster(parseClusterFromJson(json), runtime, stats, ssl_context_manager, cm,
                            true);
  cluster.initialize([] {});

  EXPECT_EQ(1UL, cluster.prioritySet().hostSetsPerPriority()[0]->healthyHosts().size());
  EXPECT_EQ(LoadBalancerType::RingHash, cluster.info()->lbType());
  EXPECT_TRUE(cluster.info()->addedViaApi());
}

TEST(StaticClusterImplTest, OutlierDetector) {
  Stats::IsolatedStoreImpl stats;
  Ssl::MockContextManager ssl_context_manager;
  NiceMock<Runtime::MockLoader> runtime;
  const std::string json = R"EOF(
  {
    "name": "addressportconfig",
    "connect_timeout_ms": 250,
    "type": "static",
    "lb_type": "random",
    "hosts": [{"url": "tcp://10.0.0.1:11001"},
              {"url": "tcp://10.0.0.2:11002"}]
  }
  )EOF";

  NiceMock<MockClusterManager> cm;
  StaticClusterImpl cluster(parseClusterFromJson(json), runtime, stats, ssl_context_manager, cm,
                            false);

  Outlier::MockDetector* detector = new Outlier::MockDetector();
  EXPECT_CALL(*detector, addChangedStateCb(_));
  cluster.setOutlierDetector(Outlier::DetectorSharedPtr{detector});
  cluster.initialize([] {});

  EXPECT_EQ(2UL, cluster.prioritySet().hostSetsPerPriority()[0]->healthyHosts().size());
  EXPECT_EQ(2UL, cluster.info()->stats().membership_healthy_.value());

  // Set a single host as having failed and fire outlier detector callbacks. This should result
  // in only a single healthy host.
  cluster.prioritySet().hostSetsPerPriority()[0]->hosts()[0]->outlierDetector().putHttpResponseCode(
      503);
  cluster.prioritySet().hostSetsPerPriority()[0]->hosts()[0]->healthFlagSet(
      Host::HealthFlag::FAILED_OUTLIER_CHECK);
  detector->runCallbacks(cluster.prioritySet().hostSetsPerPriority()[0]->hosts()[0]);
  EXPECT_EQ(1UL, cluster.prioritySet().hostSetsPerPriority()[0]->healthyHosts().size());
  EXPECT_EQ(1UL, cluster.info()->stats().membership_healthy_.value());
  EXPECT_NE(cluster.prioritySet().hostSetsPerPriority()[0]->healthyHosts()[0],
            cluster.prioritySet().hostSetsPerPriority()[0]->hosts()[0]);

  // Bring the host back online.
  cluster.prioritySet().hostSetsPerPriority()[0]->hosts()[0]->healthFlagClear(
      Host::HealthFlag::FAILED_OUTLIER_CHECK);
  detector->runCallbacks(cluster.prioritySet().hostSetsPerPriority()[0]->hosts()[0]);
  EXPECT_EQ(2UL, cluster.prioritySet().hostSetsPerPriority()[0]->healthyHosts().size());
  EXPECT_EQ(2UL, cluster.info()->stats().membership_healthy_.value());
}

TEST(StaticClusterImplTest, HealthyStat) {
  Stats::IsolatedStoreImpl stats;
  Ssl::MockContextManager ssl_context_manager;
  NiceMock<Runtime::MockLoader> runtime;
  const std::string json = R"EOF(
  {
    "name": "addressportconfig",
    "connect_timeout_ms": 250,
    "type": "static",
    "lb_type": "random",
    "hosts": [{"url": "tcp://10.0.0.1:11001"},
              {"url": "tcp://10.0.0.2:11002"}]
  }
  )EOF";

  NiceMock<MockClusterManager> cm;
  StaticClusterImpl cluster(parseClusterFromJson(json), runtime, stats, ssl_context_manager, cm,
                            false);

  Outlier::MockDetector* outlier_detector = new NiceMock<Outlier::MockDetector>();
  cluster.setOutlierDetector(Outlier::DetectorSharedPtr{outlier_detector});

  std::shared_ptr<MockHealthChecker> health_checker(new NiceMock<MockHealthChecker>());
  cluster.setHealthChecker(health_checker);

  ReadyWatcher initialized;
  cluster.initialize([&initialized] { initialized.ready(); });

  EXPECT_EQ(2UL, cluster.prioritySet().hostSetsPerPriority()[0]->hosts().size());
  EXPECT_EQ(0UL, cluster.prioritySet().hostSetsPerPriority()[0]->healthyHosts().size());
  EXPECT_EQ(0UL, cluster.info()->stats().membership_healthy_.value());

  cluster.prioritySet().hostSetsPerPriority()[0]->hosts()[0]->healthFlagClear(
      Host::HealthFlag::FAILED_ACTIVE_HC);
  health_checker->runCallbacks(cluster.prioritySet().hostSetsPerPriority()[0]->hosts()[0], true);
  cluster.prioritySet().hostSetsPerPriority()[0]->hosts()[1]->healthFlagClear(
      Host::HealthFlag::FAILED_ACTIVE_HC);
  EXPECT_CALL(initialized, ready());
  health_checker->runCallbacks(cluster.prioritySet().hostSetsPerPriority()[0]->hosts()[1], true);

  cluster.prioritySet().hostSetsPerPriority()[0]->hosts()[0]->healthFlagSet(
      Host::HealthFlag::FAILED_OUTLIER_CHECK);
  outlier_detector->runCallbacks(cluster.prioritySet().hostSetsPerPriority()[0]->hosts()[0]);
  EXPECT_EQ(1UL, cluster.prioritySet().hostSetsPerPriority()[0]->healthyHosts().size());
  EXPECT_EQ(1UL, cluster.info()->stats().membership_healthy_.value());

  cluster.prioritySet().hostSetsPerPriority()[0]->hosts()[0]->healthFlagSet(
      Host::HealthFlag::FAILED_ACTIVE_HC);
  health_checker->runCallbacks(cluster.prioritySet().hostSetsPerPriority()[0]->hosts()[0], true);
  EXPECT_EQ(1UL, cluster.prioritySet().hostSetsPerPriority()[0]->healthyHosts().size());
  EXPECT_EQ(1UL, cluster.info()->stats().membership_healthy_.value());

  cluster.prioritySet().hostSetsPerPriority()[0]->hosts()[0]->healthFlagClear(
      Host::HealthFlag::FAILED_OUTLIER_CHECK);
  outlier_detector->runCallbacks(cluster.prioritySet().hostSetsPerPriority()[0]->hosts()[0]);
  EXPECT_EQ(1UL, cluster.prioritySet().hostSetsPerPriority()[0]->healthyHosts().size());
  EXPECT_EQ(1UL, cluster.info()->stats().membership_healthy_.value());

  cluster.prioritySet().hostSetsPerPriority()[0]->hosts()[0]->healthFlagClear(
      Host::HealthFlag::FAILED_ACTIVE_HC);
  health_checker->runCallbacks(cluster.prioritySet().hostSetsPerPriority()[0]->hosts()[0], true);
  EXPECT_EQ(2UL, cluster.prioritySet().hostSetsPerPriority()[0]->healthyHosts().size());
  EXPECT_EQ(2UL, cluster.info()->stats().membership_healthy_.value());

  cluster.prioritySet().hostSetsPerPriority()[0]->hosts()[0]->healthFlagSet(
      Host::HealthFlag::FAILED_OUTLIER_CHECK);
  outlier_detector->runCallbacks(cluster.prioritySet().hostSetsPerPriority()[0]->hosts()[0]);
  EXPECT_EQ(1UL, cluster.prioritySet().hostSetsPerPriority()[0]->healthyHosts().size());
  EXPECT_EQ(1UL, cluster.info()->stats().membership_healthy_.value());

  cluster.prioritySet().hostSetsPerPriority()[0]->hosts()[1]->healthFlagSet(
      Host::HealthFlag::FAILED_ACTIVE_HC);
  health_checker->runCallbacks(cluster.prioritySet().hostSetsPerPriority()[0]->hosts()[1], true);
  EXPECT_EQ(0UL, cluster.prioritySet().hostSetsPerPriority()[0]->healthyHosts().size());
  EXPECT_EQ(0UL, cluster.info()->stats().membership_healthy_.value());
}

TEST(StaticClusterImplTest, UrlConfig) {
  Stats::IsolatedStoreImpl stats;
  Ssl::MockContextManager ssl_context_manager;
  NiceMock<Runtime::MockLoader> runtime;
  const std::string json = R"EOF(
  {
    "name": "addressportconfig",
    "connect_timeout_ms": 250,
    "type": "static",
    "lb_type": "random",
    "hosts": [{"url": "tcp://10.0.0.1:11001"},
              {"url": "tcp://10.0.0.2:11002"}]
  }
  )EOF";

  NiceMock<MockClusterManager> cm;
  StaticClusterImpl cluster(parseClusterFromJson(json), runtime, stats, ssl_context_manager, cm,
                            false);
  cluster.initialize([] {});

  EXPECT_EQ(1024U, cluster.info()->resourceManager(ResourcePriority::Default).connections().max());
  EXPECT_EQ(1024U,
            cluster.info()->resourceManager(ResourcePriority::Default).pendingRequests().max());
  EXPECT_EQ(1024U, cluster.info()->resourceManager(ResourcePriority::Default).requests().max());
  EXPECT_EQ(3U, cluster.info()->resourceManager(ResourcePriority::Default).retries().max());
  EXPECT_EQ(1024U, cluster.info()->resourceManager(ResourcePriority::High).connections().max());
  EXPECT_EQ(1024U, cluster.info()->resourceManager(ResourcePriority::High).pendingRequests().max());
  EXPECT_EQ(1024U, cluster.info()->resourceManager(ResourcePriority::High).requests().max());
  EXPECT_EQ(3U, cluster.info()->resourceManager(ResourcePriority::High).retries().max());
  EXPECT_EQ(0U, cluster.info()->maxRequestsPerConnection());
  EXPECT_EQ(Http::Http2Settings::DEFAULT_HPACK_TABLE_SIZE,
            cluster.info()->http2Settings().hpack_table_size_);
  EXPECT_EQ(LoadBalancerType::Random, cluster.info()->lbType());
  EXPECT_THAT(
      std::list<std::string>({"10.0.0.1:11001", "10.0.0.2:11002"}),
      ContainerEq(hostListToAddresses(cluster.prioritySet().hostSetsPerPriority()[0]->hosts())));
  EXPECT_EQ(2UL, cluster.prioritySet().hostSetsPerPriority()[0]->healthyHosts().size());
  EXPECT_EQ(0UL, cluster.prioritySet().hostSetsPerPriority()[0]->hostsPerLocality().size());
  EXPECT_EQ(0UL, cluster.prioritySet().hostSetsPerPriority()[0]->healthyHostsPerLocality().size());
  cluster.prioritySet().hostSetsPerPriority()[0]->hosts()[0]->healthChecker().setUnhealthy();
}

TEST(StaticClusterImplTest, UnsupportedLBType) {
  Stats::IsolatedStoreImpl stats;
  Ssl::MockContextManager ssl_context_manager;
  NiceMock<Runtime::MockLoader> runtime;
  NiceMock<MockClusterManager> cm;
  const std::string json = R"EOF(
  {
    "name": "addressportconfig",
    "connect_timeout_ms": 250,
    "type": "static",
    "lb_type": "fakelbtype",
    "hosts": [{"url": "tcp://192.168.1.1:22"},
              {"url": "tcp://192.168.1.2:44"}]
  }
  )EOF";

  EXPECT_THROW(
      StaticClusterImpl(parseClusterFromJson(json), runtime, stats, ssl_context_manager, cm, false),
      EnvoyException);
}

TEST(ClusterDefinitionTest, BadClusterConfig) {
  const std::string json = R"EOF(
  {
    "name": "cluster_1",
    "connect_timeout_ms": 250,
    "type": "static",
    "lb_type": "round_robin",
    "fake_type" : "expected_failure",
    "hosts": [{"url": "tcp://127.0.0.1:11001"}]
  }
  )EOF";

  Json::ObjectSharedPtr loader = Json::Factory::loadFromString(json);
  EXPECT_THROW(loader->validateSchema(Json::Schema::CLUSTER_SCHEMA), Json::Exception);
}

TEST(ClusterDefinitionTest, BadDnsClusterConfig) {
  const std::string json = R"EOF(
  {
    "name": "cluster_1",
    "connect_timeout_ms": 250,
    "type": "static",
    "lb_type": "round_robin",
    "hosts": [{"url": "tcp://127.0.0.1:11001"}],
    "dns_lookup_family" : "foo"
  }
  )EOF";

  Json::ObjectSharedPtr loader = Json::Factory::loadFromString(json);
  EXPECT_THROW(loader->validateSchema(Json::Schema::CLUSTER_SCHEMA), Json::Exception);
}

TEST(StaticClusterImplTest, SourceAddressPriority) {
  Stats::IsolatedStoreImpl stats;
  Ssl::MockContextManager ssl_context_manager;
  NiceMock<Runtime::MockLoader> runtime;
  envoy::api::v2::Cluster config;
  config.set_name("staticcluster");
  config.mutable_connect_timeout();

  Network::Address::InstanceConstSharedPtr bootstrap_address =
      Network::Utility::parseInternetAddress("1.2.3.5");
  {
    // If the cluster manager gets a source address from the bootstrap proto, use it.
    NiceMock<MockClusterManager> cm;
    cm.source_address_ = bootstrap_address;
    StaticClusterImpl cluster(config, runtime, stats, ssl_context_manager, cm, false);
    EXPECT_EQ(bootstrap_address->asString(), cluster.info()->sourceAddress()->asString());
  }

  const std::string cluster_address = "5.6.7.8";
  config.mutable_upstream_bind_config()->mutable_source_address()->set_address(cluster_address);
  {
    // Verify source address from cluster config is used when present.
    NiceMock<MockClusterManager> cm;
    StaticClusterImpl cluster(config, runtime, stats, ssl_context_manager, cm, false);
    EXPECT_EQ(cluster_address, cluster.info()->sourceAddress()->ip()->addressAsString());
  }

  {
    // The source address from cluster config takes precedence over one from the bootstrap proto.
    NiceMock<MockClusterManager> cm;
    cm.source_address_ = bootstrap_address;
    StaticClusterImpl cluster(config, runtime, stats, ssl_context_manager, cm, false);
    EXPECT_EQ(cluster_address, cluster.info()->sourceAddress()->ip()->addressAsString());
  }
}

// Test creating and extending a priority set.
TEST(PrioritySet, Extend) {
  PrioritySetImpl priority_set;
  priority_set.getOrCreateHostSet(0);

  uint32_t changes = 0;
  uint32_t last_priority = 0;
  priority_set.addMemberUpdateCb([&](uint32_t priority, const std::vector<HostSharedPtr>&,
                                     const std::vector<HostSharedPtr>&) -> void { // fIXME
    last_priority = priority;
    ++changes;
  });

  // The initial priority set starts with priority level 0..
  EXPECT_EQ(1, priority_set.hostSetsPerPriority().size());
  EXPECT_EQ(0, priority_set.hostSetsPerPriority()[0]->hosts().size());
  EXPECT_EQ(0, priority_set.hostSetsPerPriority()[0]->priority());

  // Add priorities 1 and 2, ensure the callback is called, and that the new
  // host sets are created with the correct priority.
  EXPECT_EQ(0, changes);
  EXPECT_EQ(0, priority_set.getOrCreateHostSet(2).hosts().size());
  EXPECT_EQ(3, priority_set.hostSetsPerPriority().size());
  // No-op host set creation does not trigger callbacks.
  EXPECT_EQ(0, changes);
  EXPECT_EQ(last_priority, 0);
  EXPECT_EQ(1, priority_set.hostSetsPerPriority()[1]->priority());
  EXPECT_EQ(2, priority_set.hostSetsPerPriority()[2]->priority());

  // Now add hosts for priority 1, and ensure they're added and subscribers are notified.
  std::shared_ptr<MockClusterInfo> info{new NiceMock<MockClusterInfo>()};
  HostVectorSharedPtr hosts(
      new std::vector<HostSharedPtr>({makeTestHost(info, "tcp://127.0.0.1:80")}));
  HostListsSharedPtr hosts_per_locality(new std::vector<std::vector<HostSharedPtr>>({}));
  std::vector<HostSharedPtr> hosts_added{hosts->front()};
  std::vector<HostSharedPtr> hosts_removed{};

  priority_set.hostSetsPerPriority()[1]->updateHosts(
      hosts, hosts, hosts_per_locality, hosts_per_locality, hosts_added, hosts_removed);
  EXPECT_EQ(1, changes);
  EXPECT_EQ(last_priority, 1);
  EXPECT_EQ(1, priority_set.hostSetsPerPriority()[1]->hosts().size());

  // Test iteration.
  int i = 0;
  for (auto& host_set : priority_set.hostSetsPerPriority()) {
    EXPECT_EQ(host_set.get(), priority_set.hostSetsPerPriority()[i++].get());
  }
}

// Cluster metadata retrieval.
TEST(ClusterMetadataTest, Metadata) {
  Stats::IsolatedStoreImpl stats;
  Ssl::MockContextManager ssl_context_manager;
  auto dns_resolver = std::make_shared<Network::MockDnsResolver>();
  NiceMock<Event::MockDispatcher> dispatcher;
  NiceMock<Runtime::MockLoader> runtime;
  NiceMock<MockClusterManager> cm;
  ReadyWatcher initialized;

  const std::string yaml = R"EOF(
    name: name
    connect_timeout: 0.25s
    type: STRICT_DNS
    lb_policy: ROUND_ROBIN
    hosts: [{ socket_address: { address: foo.bar.com, port_value: 443 }}]
    metadata: { filter_metadata: { com.bar.foo: { baz: test_value } } }
  )EOF";

  StrictDnsClusterImpl cluster(parseClusterFromV2Yaml(yaml), runtime, stats, ssl_context_manager,
                               dns_resolver, cm, dispatcher, false);
  EXPECT_EQ("test_value",
            Config::Metadata::metadataValue(cluster.info()->metadata(), "com.bar.foo", "baz")
                .string_value());
}

} // namespace
} // namespace Upstream
} // namespace Envoy
