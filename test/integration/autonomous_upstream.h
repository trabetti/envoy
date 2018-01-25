#include "test/integration/fake_upstream.h"

namespace Envoy {

// A stream which automatically responds when the downstream request is
// completely read. By default the response is 200: OK with 10 bytes of
// payload. This behavior can be overriden with custom request headers defined below.
class AutonomousStream : public FakeStream {
public:
  // The number of response bytes to send. Payload is randomized.
  static const char RESPONSE_SIZE_BYTES[];
  // If set to an integer, the AutonomousStream will expect the response body to
  // be this large.
  static const char EXPECT_REQUEST_SIZE_BYTES[];
  // If set, the stream will reset when the request is complete, rather than
  // sending a response.
  static const char RESET_AFTER_REQUEST[];

  AutonomousStream(FakeHttpConnection& parent, Http::StreamEncoder& encoder)
      : FakeStream(parent, encoder) {}
  ~AutonomousStream();

  void setEndStream(bool set) override;

private:
  void sendResponse();
};

// An upstream which creates AutonomousStreams for new incoming streams.
class AutonomousHttpConnection : public FakeHttpConnection {
public:
  AutonomousHttpConnection(QueuedConnectionWrapperPtr connection_wrapper, Stats::Store& store,
                           Type type)
      : FakeHttpConnection(std::move(connection_wrapper), store, type) {}

  Http::StreamDecoder& newStream(Http::StreamEncoder& response_encoder) override;

private:
  std::vector<FakeStreamPtr> streams_;
};

typedef std::unique_ptr<AutonomousHttpConnection> AutonomousHttpConnectionPtr;

// An upstream which creates AutonomousHttpConnection for new incoming connections.
class AutonomousUpstream : public FakeUpstream {
public:
  AutonomousUpstream(uint32_t port, FakeHttpConnection::Type type,
                     Network::Address::IpVersion version)
      : FakeUpstream(port, type, version) {}
  ~AutonomousUpstream();
  bool createNetworkFilterChain(Network::Connection& connection) override;
  bool createListenerFilterChain(Network::ListenerFilterManager& listener) override;

private:
  std::vector<AutonomousHttpConnectionPtr> http_connections_;
};

} // namespace Envoy
