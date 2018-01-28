#pragma once

#include "envoy/config/subscription.h"
#include "envoy/event/dispatcher.h"
#include "envoy/grpc/async_client.h"

#include "common/config/grpc_mux_impl.h"
#include "common/config/grpc_mux_subscription_impl.h"

#include "api/base.pb.h"

namespace Envoy {
namespace Config {

template <class ResourceType>
class GrpcSubscriptionImpl : public Config::Subscription<ResourceType> {
public:
  GrpcSubscriptionImpl(const envoy::api::v2::Node& node, Grpc::AsyncClientPtr async_client,
                       Event::Dispatcher& dispatcher,
                       const Protobuf::MethodDescriptor& service_method, SubscriptionStats stats)
      : grpc_mux_(node, std::move(async_client), dispatcher, service_method),
        grpc_mux_subscription_(grpc_mux_, stats) {}

  // Config::Subscription
  void start(const std::vector<std::string>& resources,
             Config::SubscriptionCallbacks<ResourceType>& callbacks) override {
    // Subscribe first, so we get failure callbacks if grpc_mux_.start() fails.
    grpc_mux_subscription_.start(resources, callbacks);
    grpc_mux_.start();
  }

  void updateResources(const std::vector<std::string>& resources) override {
    grpc_mux_subscription_.updateResources(resources);
  }

  const std::string versionInfo() const override { return grpc_mux_subscription_.versionInfo(); }

  GrpcMuxImpl& grpcMux() { return grpc_mux_; }

private:
  GrpcMuxImpl grpc_mux_;
  GrpcMuxSubscriptionImpl<ResourceType> grpc_mux_subscription_;
};

} // namespace Config
} // namespace Envoy
