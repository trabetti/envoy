#include "server/config/stats/metrics_service.h"

#include <string>

#include "envoy/registry/registry.h"

#include "common/config/well_known_names.h"
#include "common/grpc/async_client_impl.h"
#include "common/network/resolver_impl.h"
#include "common/stats/grpc_metrics_service_impl.h"

#include "api/metrics_service.pb.h"
#include "api/metrics_service.pb.validate.h"

namespace Envoy {
namespace Server {
namespace Configuration {

Stats::SinkPtr MetricsServiceSinkFactory::createStatsSink(const Protobuf::Message& config,
                                                          Server::Instance& server) {
  const auto& sink_config =
      MessageUtil::downcastAndValidate<const envoy::api::v2::MetricsServiceConfig&>(config);
  const auto& grpc_service = sink_config.grpc_service();
  ENVOY_LOG(debug, "Metrics Service gRPC service configuration: {}", grpc_service.DebugString());

  std::shared_ptr<Stats::Metrics::GrpcMetricsStreamer> grpc_metrics_streamer =
      std::make_shared<Stats::Metrics::GrpcMetricsStreamerImpl>(
          server.clusterManager().grpcAsyncClientManager().factoryForGrpcService(grpc_service,
                                                                                 server.stats()),
          server.threadLocal(), server.localInfo());

  return Stats::SinkPtr(
      std::make_unique<Stats::Metrics::MetricsServiceSink>(grpc_metrics_streamer));
}

ProtobufTypes::MessagePtr MetricsServiceSinkFactory::createEmptyConfigProto() {
  return std::unique_ptr<envoy::api::v2::MetricsServiceConfig>(
      std::make_unique<envoy::api::v2::MetricsServiceConfig>());
}

std::string MetricsServiceSinkFactory::name() {
  return Config::StatsSinkNames::get().METRICS_SERVICE;
}

/**
 * Static registration for the this sink factory. @see RegisterFactory.
 */
static Registry::RegisterFactory<MetricsServiceSinkFactory, StatsSinkFactory> register_;

} // namespace Configuration
} // namespace Server
} // namespace Envoy
