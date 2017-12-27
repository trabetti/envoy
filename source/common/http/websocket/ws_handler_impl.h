#pragma once

#include <string>

#include "envoy/http/codec.h"
#include "envoy/http/header_map.h"
#include "envoy/http/websocket.h"
#include "envoy/network/filter.h"
#include "envoy/router/router.h"
#include "envoy/upstream/cluster_manager.h"

#include "common/filter/tcp_proxy.h"

namespace Envoy {
namespace Http {
namespace WebSocket {

/**
 * An implementation of a WebSocket proxy based on TCP proxy. This will be used for
 * handling client connection only after a WebSocket upgrade request succeeds
 * (i.e, it is requested by client and allowed by config). This implementation will
 * instantiate a new outgoing TCP connection for the configured upstream cluster.
 * All data will be proxied back and forth between the two connections, without any
 * knowledge of the underlying WebSocket protocol.
 */
class WsHandlerImpl : public Envoy::Filter::TcpProxy {
public:
  WsHandlerImpl(HeaderMap& request_headers, const RequestInfo::RequestInfo& request_info,
                const Router::RouteEntry& route_entry, WsHandlerCallbacks& callbacks,
                Upstream::ClusterManager& cluster_manager,
                Network::ReadFilterCallbacks* read_callbacks);

protected:
  // Filter::TcpProxy
  const std::string& getUpstreamCluster() override { return route_entry_.clusterName(); }
  void onInitFailure(UpstreamFailureReason failure_reason) override;
  void onConnectionSuccess() override;

private:
  struct NullHttpConnectionCallbacks : public ConnectionCallbacks {
    // Http::ConnectionCallbacks
    void onGoAway() override {}
  };

  HeaderMap& request_headers_;
  const RequestInfo::RequestInfo& request_info_;
  const Router::RouteEntry& route_entry_;
  WsHandlerCallbacks& ws_callbacks_;
  NullHttpConnectionCallbacks http_conn_callbacks_;
};

typedef std::unique_ptr<WsHandlerImpl> WsHandlerImplPtr;

} // namespace WebSocket
} // namespace Http
} // namespace Envoy
