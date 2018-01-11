#include "server/config/network/ssl_socket.h"

#include "envoy/registry/registry.h"

#include "common/network/raw_buffer_socket.h"
#include "common/protobuf/utility.h"
#include "common/ssl/context_config_impl.h"
#include "common/ssl/ssl_socket.h"

#include "api/sds.pb.h"
#include "api/sds.pb.validate.h"

namespace Envoy {
namespace Server {
namespace Configuration {

Network::TransportSocketFactoryPtr
UpstreamSslSocketFactory::createTransportSocketFactory(const Protobuf::Message& message,
                                                       TransportSocketFactoryContext& context) {
  return std::make_unique<Ssl::ClientSslSocketFactory>(
      Ssl::ClientContextConfigImpl(
          MessageUtil::downcastAndValidate<const envoy::api::v2::UpstreamTlsContext&>(message)),
      context.sslContextManager(), context.statsScope());
}

ProtobufTypes::MessagePtr UpstreamSslSocketFactory::createEmptyConfigProto() {
  return std::make_unique<envoy::api::v2::UpstreamTlsContext>();
}

static Registry::RegisterFactory<UpstreamSslSocketFactory, UpstreamTransportSocketConfigFactory>
    upstream_registered_;

} // namespace Configuration
} // namespace Server
} // namespace Envoy
