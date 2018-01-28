#include "common/network/address_impl.h"
#include "common/network/listener_impl.h"
#include "common/network/utility.h"
#include "common/stats/stats_impl.h"

#include "test/mocks/network/mocks.h"
#include "test/mocks/server/mocks.h"
#include "test/test_common/environment.h"
#include "test/test_common/network_utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::Invoke;
using testing::Return;
using testing::_;

namespace Envoy {
namespace Network {

static void errorCallbackTest(Address::IpVersion version) {
  // Force the error callback to fire by closing the socket under the listener. We run this entire
  // test in the forked process to avoid confusion when the fork happens.
  Stats::IsolatedStoreImpl stats_store;
  Event::DispatcherImpl dispatcher;

  Network::TcpListenSocket socket(Network::Test::getCanonicalLoopbackAddress(version), true);
  Network::MockListenerCallbacks listener_callbacks;
  Network::MockConnectionHandler connection_handler;
  Network::ListenerPtr listener =
      dispatcher.createListener(socket, listener_callbacks, true, false);

  Network::ClientConnectionPtr client_connection = dispatcher.createClientConnection(
      socket.localAddress(), Network::Address::InstanceConstSharedPtr(),
      Network::Test::createRawBufferSocket());
  client_connection->connect();

  EXPECT_CALL(listener_callbacks, onAccept_(_, _))
      .WillOnce(Invoke([&](Network::ConnectionSocketPtr& socket, bool) -> void {
        Network::ConnectionPtr new_connection =
            dispatcher.createServerConnection(std::move(socket), nullptr);
        listener_callbacks.onNewConnection(std::move(new_connection));
      }));
  EXPECT_CALL(listener_callbacks, onNewConnection_(_))
      .WillOnce(Invoke([&](Network::ConnectionPtr& conn) -> void {
        client_connection->close(ConnectionCloseType::NoFlush);
        conn->close(ConnectionCloseType::NoFlush);
        socket.close();
      }));

  dispatcher.run(Event::Dispatcher::RunType::Block);
}

class ListenerImplDeathTest : public testing::TestWithParam<Address::IpVersion> {};
INSTANTIATE_TEST_CASE_P(IpVersions, ListenerImplDeathTest,
                        testing::ValuesIn(TestEnvironment::getIpVersionsForTest()));

TEST_P(ListenerImplDeathTest, ErrorCallback) {
  EXPECT_DEATH(errorCallbackTest(GetParam()), ".*listener accept failure.*");
}

class TestListenerImpl : public ListenerImpl {
public:
  TestListenerImpl(Event::DispatcherImpl& dispatcher, ListenSocket& socket, ListenerCallbacks& cb,
                   bool bind_to_port, bool hand_off_restored_destination_connections)
      : ListenerImpl(dispatcher, socket, cb, bind_to_port,
                     hand_off_restored_destination_connections) {}

  MOCK_METHOD1(getLocalAddress, Address::InstanceConstSharedPtr(int fd));
};

class ListenerImplTest : public testing::TestWithParam<Address::IpVersion> {
protected:
  ListenerImplTest()
      : version_(GetParam()),
        alt_address_(Network::Test::findOrCheckFreePort(
            Network::Test::getCanonicalLoopbackAddress(version_), Address::SocketType::Stream)) {}

  const Address::IpVersion version_;
  const Address::InstanceConstSharedPtr alt_address_;
};
INSTANTIATE_TEST_CASE_P(IpVersions, ListenerImplTest,
                        testing::ValuesIn(TestEnvironment::getIpVersionsForTest()));

TEST_P(ListenerImplTest, UseActualDst) {
  Stats::IsolatedStoreImpl stats_store;
  Event::DispatcherImpl dispatcher;
  Network::TcpListenSocket socket(Network::Test::getCanonicalLoopbackAddress(version_), true);
  Network::TcpListenSocket socketDst(alt_address_, false);
  Network::MockListenerCallbacks listener_callbacks1;
  Network::MockConnectionHandler connection_handler;
  // Do not redirect since use_original_dst is false.
  Network::TestListenerImpl listener(dispatcher, socket, listener_callbacks1, true, true);
  Network::MockListenerCallbacks listener_callbacks2;
  Network::TestListenerImpl listenerDst(dispatcher, socketDst, listener_callbacks2, false, false);

  Network::ClientConnectionPtr client_connection = dispatcher.createClientConnection(
      socket.localAddress(), Network::Address::InstanceConstSharedPtr(),
      Network::Test::createRawBufferSocket());
  client_connection->connect();

  EXPECT_CALL(listener, getLocalAddress(_)).Times(0);

  EXPECT_CALL(listener_callbacks2, onAccept_(_, _)).Times(0);
  EXPECT_CALL(listener_callbacks1, onAccept_(_, _))
      .WillOnce(Invoke([&](Network::ConnectionSocketPtr& socket, bool) -> void {
        Network::ConnectionPtr new_connection =
            dispatcher.createServerConnection(std::move(socket), nullptr);
        listener_callbacks1.onNewConnection(std::move(new_connection));
      }));
  EXPECT_CALL(listener_callbacks1, onNewConnection_(_))
      .WillOnce(Invoke([&](Network::ConnectionPtr& conn) -> void {
        EXPECT_EQ(*conn->localAddress(), *socket.localAddress());
        client_connection->close(ConnectionCloseType::NoFlush);
        conn->close(ConnectionCloseType::NoFlush);
        dispatcher.exit();
      }));

  dispatcher.run(Event::Dispatcher::RunType::Block);
}

TEST_P(ListenerImplTest, WildcardListenerUseActualDst) {
  Stats::IsolatedStoreImpl stats_store;
  Event::DispatcherImpl dispatcher;
  Network::TcpListenSocket socket(Network::Test::getAnyAddress(version_), true);
  Network::MockListenerCallbacks listener_callbacks;
  Network::MockConnectionHandler connection_handler;
  // Do not redirect since use_original_dst is false.
  Network::TestListenerImpl listener(dispatcher, socket, listener_callbacks, true, true);

  auto local_dst_address = Network::Utility::getAddressWithPort(
      *Network::Test::getCanonicalLoopbackAddress(version_), socket.localAddress()->ip()->port());
  Network::ClientConnectionPtr client_connection = dispatcher.createClientConnection(
      local_dst_address, Network::Address::InstanceConstSharedPtr(),
      Network::Test::createRawBufferSocket());
  client_connection->connect();

  EXPECT_CALL(listener, getLocalAddress(_)).WillOnce(Return(local_dst_address));

  EXPECT_CALL(listener_callbacks, onAccept_(_, _))
      .WillOnce(Invoke([&](Network::ConnectionSocketPtr& socket, bool) -> void {
        Network::ConnectionPtr new_connection =
            dispatcher.createServerConnection(std::move(socket), nullptr);
        listener_callbacks.onNewConnection(std::move(new_connection));
      }));
  EXPECT_CALL(listener_callbacks, onNewConnection_(_))
      .WillOnce(Invoke([&](Network::ConnectionPtr& conn) -> void {
        EXPECT_EQ(*conn->localAddress(), *local_dst_address);
        client_connection->close(ConnectionCloseType::NoFlush);
        conn->close(ConnectionCloseType::NoFlush);
        dispatcher.exit();
      }));

  dispatcher.run(Event::Dispatcher::RunType::Block);
}

} // namespace Network
} // namespace Envoy
