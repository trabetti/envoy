#include "server/config_validation/cluster_manager.h"

namespace Envoy {
namespace Upstream {

ValidationClusterManagerFactory::ValidationClusterManagerFactory(
    Runtime::Loader& runtime, Stats::Store& stats, ThreadLocal::Instance& tls,
    Runtime::RandomGenerator& random, Network::DnsResolverSharedPtr dns_resolver,
    Ssl::ContextManager& ssl_context_manager, Event::Dispatcher& primary_dispatcher,
    const LocalInfo::LocalInfo& local_info)
    : ProdClusterManagerFactory(runtime, stats, tls, random, dns_resolver, ssl_context_manager,
                                primary_dispatcher, local_info) {}

ClusterManagerPtr ValidationClusterManagerFactory::clusterManagerFromProto(
    const envoy::api::v2::Bootstrap& bootstrap, Stats::Store& stats, ThreadLocal::Instance& tls,
    Runtime::Loader& runtime, Runtime::RandomGenerator& random,
    const LocalInfo::LocalInfo& local_info, AccessLog::AccessLogManager& log_manager) {
  return ClusterManagerPtr{new ValidationClusterManager(
      bootstrap, *this, stats, tls, runtime, random, local_info, log_manager, primary_dispatcher_)};
}

CdsApiPtr
ValidationClusterManagerFactory::createCds(const envoy::api::v2::ConfigSource& cds_config,
                                           const Optional<envoy::api::v2::ConfigSource>& eds_config,
                                           ClusterManager& cm) {
  // Create the CdsApiImpl...
  ProdClusterManagerFactory::createCds(cds_config, eds_config, cm);
  // ... and then throw it away, so that we don't actually connect to it.
  return nullptr;
}

ValidationClusterManager::ValidationClusterManager(
    const envoy::api::v2::Bootstrap& bootstrap, ClusterManagerFactory& factory, Stats::Store& stats,
    ThreadLocal::Instance& tls, Runtime::Loader& runtime, Runtime::RandomGenerator& random,
    const LocalInfo::LocalInfo& local_info, AccessLog::AccessLogManager& log_manager,
    Event::Dispatcher& primary_dispatcher)
    : ClusterManagerImpl(bootstrap, factory, stats, tls, runtime, random, local_info, log_manager,
                         primary_dispatcher) {}

Http::ConnectionPool::Instance*
ValidationClusterManager::httpConnPoolForCluster(const std::string&, ResourcePriority,
                                                 Http::Protocol, LoadBalancerContext*) {
  return nullptr;
}

Host::CreateConnectionData ValidationClusterManager::tcpConnForCluster(const std::string&,
                                                                       LoadBalancerContext*) {
  return Host::CreateConnectionData{nullptr, nullptr};
}

Http::AsyncClient& ValidationClusterManager::httpAsyncClientForCluster(const std::string&) {
  return async_client_;
}

} // namespace Upstream
} // namespace Envoy
