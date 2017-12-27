#pragma once

#include <cstdint>
#include <string>

#include "envoy/network/transport_socket.h"

#include "common/common/logger.h"
#include "common/ssl/context_impl.h"

#include "openssl/ssl.h"

namespace Envoy {
namespace Ssl {

enum class InitialState { Client, Server };

class SslSocket : public Network::TransportSocket,
                  public Connection,
                  protected Logger::Loggable<Logger::Id::connection> {
public:
  SslSocket(Context& ctx, InitialState state);

  // Ssl::Connection
  bool peerCertificatePresented() const override;
  std::string uriSanLocalCertificate() override;
  std::string sha256PeerCertificateDigest() override;
  std::string subjectPeerCertificate() const override;
  std::string uriSanPeerCertificate() override;

  // Network::TransportSocket
  void setTransportSocketCallbacks(Network::TransportSocketCallbacks& callbacks) override;
  std::string protocol() const override;
  bool canFlushClose() override { return handshake_complete_; }
  void closeSocket(Network::ConnectionEvent close_type) override;
  Network::IoResult doRead(Buffer::Instance& read_buffer) override;
  Network::IoResult doWrite(Buffer::Instance& write_buffer) override;
  void onConnected() override;

  SSL* rawSslForTest() { return ssl_.get(); }

private:
  Network::PostIoAction doHandshake();
  void drainErrorQueue();
  std::string getUriSanFromCertificate(X509* cert);

  Network::TransportSocketCallbacks* callbacks_{};
  ContextImpl& ctx_;
  bssl::UniquePtr<SSL> ssl_;
  bool handshake_complete_{};
};

} // namespace Ssl
} // namespace Envoy
