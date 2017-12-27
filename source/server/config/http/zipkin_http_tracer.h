#pragma once

#include <string>

#include "envoy/server/instance.h"

#include "server/configuration_impl.h"

namespace Envoy {
namespace Server {
namespace Configuration {

/**
 * Config registration for the zipkin http tracer. @see HttpTracerFactory.
 */
class ZipkinHttpTracerFactory : public HttpTracerFactory {
public:
  // HttpTracerFactory
  Tracing::HttpTracerPtr createHttpTracer(const Json::Object& json_config, Server::Instance& server,
                                          Upstream::ClusterManager& cluster_manager) override;
  std::string name() override;
};

} // namespace Configuration
} // namespace Server
} // namespace Envoy
