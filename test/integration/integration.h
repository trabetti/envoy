#pragma once

#include <functional>
#include <list>
#include <string>
#include <vector>

#include "common/http/codec_client.h"

#include "test/config/utility.h"
#include "test/integration/fake_upstream.h"
#include "test/integration/server.h"
#include "test/integration/utility.h"
#include "test/mocks/buffer/mocks.h"
#include "test/test_common/environment.h"
#include "test/test_common/printers.h"

#include "spdlog/spdlog.h"

namespace Envoy {
/**
 * Stream decoder wrapper used during integration testing.
 */
class IntegrationStreamDecoder : public Http::StreamDecoder, public Http::StreamCallbacks {
public:
  IntegrationStreamDecoder(Event::Dispatcher& dispatcher);

  const std::string& body() { return body_; }
  bool complete() { return saw_end_stream_; }
  bool reset() { return saw_reset_; }
  Http::StreamResetReason reset_reason() { return reset_reason_; }
  const Http::HeaderMap& headers() { return *headers_; }
  const Http::HeaderMapPtr& trailers() { return trailers_; }
  void waitForHeaders();
  void waitForBodyData(uint64_t size);
  void waitForEndStream();
  void waitForReset();

  // Http::StreamDecoder
  void decodeHeaders(Http::HeaderMapPtr&& headers, bool end_stream) override;
  void decodeData(Buffer::Instance& data, bool end_stream) override;
  void decodeTrailers(Http::HeaderMapPtr&& trailers) override;

  // Http::StreamCallbacks
  void onResetStream(Http::StreamResetReason reason) override;
  void onAboveWriteBufferHighWatermark() override {}
  void onBelowWriteBufferLowWatermark() override {}

private:
  Event::Dispatcher& dispatcher_;
  Http::HeaderMapPtr headers_;
  Http::HeaderMapPtr trailers_;
  bool waiting_for_end_stream_{};
  bool saw_end_stream_{};
  std::string body_;
  uint64_t body_data_waiting_length_{};
  bool waiting_for_reset_{};
  bool waiting_for_headers_{};
  bool saw_reset_{};
  Http::StreamResetReason reset_reason_{};
};

typedef std::unique_ptr<IntegrationStreamDecoder> IntegrationStreamDecoderPtr;

/**
 * TCP client used during integration testing.
 */
class IntegrationTcpClient {
public:
  IntegrationTcpClient(Event::Dispatcher& dispatcher, MockBufferFactory& factory, uint32_t port,
                       Network::Address::IpVersion version);

  void close();
  void waitForData(const std::string& data);
  void waitForDisconnect();
  void write(const std::string& data);
  const std::string& data() { return payload_reader_->data(); }

private:
  struct ConnectionCallbacks : public Network::ConnectionCallbacks {
    ConnectionCallbacks(IntegrationTcpClient& parent) : parent_(parent) {}

    // Network::ConnectionCallbacks
    void onEvent(Network::ConnectionEvent event) override;
    void onAboveWriteBufferHighWatermark() override {}
    void onBelowWriteBufferLowWatermark() override {}

    IntegrationTcpClient& parent_;
  };

  std::shared_ptr<WaitForPayloadReader> payload_reader_;
  std::shared_ptr<ConnectionCallbacks> callbacks_;
  Network::ClientConnectionPtr connection_;
  bool disconnected_{};
  MockWatermarkBuffer* client_write_buffer_;
};

typedef std::unique_ptr<IntegrationTcpClient> IntegrationTcpClientPtr;

struct ApiFilesystemConfig {
  std::string bootstrap_path_;
  std::string cds_path_;
  std::string eds_path_;
  std::string lds_path_;
  std::string rds_path_;
};

/**
 * Test fixture for all integration tests.
 */
class BaseIntegrationTest : Logger::Loggable<Logger::Id::testing> {
public:
  BaseIntegrationTest(Network::Address::IpVersion version,
                      const std::string& config = ConfigHelper::HTTP_PROXY_CONFIG);
  virtual ~BaseIntegrationTest() {}

  void SetUp();

  // Initialize the basic proto configuration, create fake upstreams, and start Envoy.
  virtual void initialize();
  // Set up the fake upstream connections. This is called by initialize() and
  // is virtual to allow subclass overrides.
  virtual void createUpstreams();
  // Finalize the config and spin up an Envoy instance.
  virtual void createEnvoy();
  // sets upstream_protocol_ and alters the upstream protocol in the config_helper_
  void setUpstreamProtocol(FakeHttpConnection::Type protocol);

  FakeHttpConnection::Type upstreamProtocol() const { return upstream_protocol_; }

  IntegrationTcpClientPtr makeTcpConnection(uint32_t port);

  // Test-wide port map.
  void registerPort(const std::string& key, uint32_t port);
  uint32_t lookupPort(const std::string& key);

  Network::ClientConnectionPtr makeClientConnection(uint32_t port);

  void registerTestServerPorts(const std::vector<std::string>& port_names);
  void createTestServer(const std::string& json_path, const std::vector<std::string>& port_names);
  void createGeneratedApiTestServer(const std::string& bootstrap_path,
                                    const std::vector<std::string>& port_names);
  void createApiTestServer(const ApiFilesystemConfig& api_filesystem_config,
                           const std::vector<std::string>& port_names);

  Api::ApiPtr api_;
  MockBufferFactory* mock_buffer_factory_; // Will point to the dispatcher's factory.
  Event::DispatcherPtr dispatcher_;
  void sendRawHttpAndWaitForResponse(const char* http, std::string* response);

protected:
  bool initialized() const { return initialized_; }

  // The IpVersion (IPv4, IPv6) to use.
  Network::Address::IpVersion version_;
  // The config for envoy start-up.
  ConfigHelper config_helper_;
  // Steps that should be done prior to the workers starting. E.g., xDS pre-init.
  std::function<void()> pre_worker_start_test_steps_;

  std::vector<std::unique_ptr<FakeUpstream>> fake_upstreams_;
  spdlog::level::level_enum default_log_level_;
  IntegrationTestServerPtr test_server_;
  TestEnvironment::PortMap port_map_;

  // The named ports for createGeneratedApiTestServer. Used mostly for lookupPort.
  std::vector<std::string> named_ports_{{"default_port"}};
  // If true, use AutonomousUpstream for fake upstreams.
  bool autonomous_upstream_{false};

private:
  // The codec type for the client-to-Envoy connection
  Http::CodecClient::Type downstream_protocol_{Http::CodecClient::Type::HTTP1};
  // The type for the Envoy-to-backend connection
  FakeHttpConnection::Type upstream_protocol_{FakeHttpConnection::Type::HTTP1};
  // True if initialized() has been called.
  bool initialized_{};
};

} // namespace Envoy
