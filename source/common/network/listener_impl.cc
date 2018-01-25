#include "common/network/listener_impl.h"

#include <sys/un.h>

#include "envoy/common/exception.h"

#include "common/common/assert.h"
#include "common/common/empty_string.h"
#include "common/event/dispatcher_impl.h"
#include "common/event/file_event_impl.h"
#include "common/network/address_impl.h"

#include "event2/listener.h"
#include "fmt/format.h"

namespace Envoy {
namespace Network {

Address::InstanceConstSharedPtr ListenerImpl::getLocalAddress(int fd) {
  return Address::addressFromFd(fd);
}

void ListenerImpl::listenCallback(evconnlistener*, evutil_socket_t fd, sockaddr* remote_addr,
                                  int remote_addr_len, void* arg) {
  ListenerImpl* listener = static_cast<ListenerImpl*>(arg);
  ConnectionSocketPtr socket(new AcceptedSocketImpl(
      fd,
      // Get the local address from the new socket if the listener is listening on IP ANY
      // (e.g., 0.0.0.0 for IPv4) (local_address_ is nullptr in this case).
      !listener->local_address_ ? listener->getLocalAddress(fd) : listener->local_address_,
      // The accept() call that filled in remote_addr doesn't fill in more than the sa_family field
      // for Unix domain sockets; apparently there isn't a mechanism in the kernel to get the
      // sockaddr_un associated with the client socket when starting from the server socket.
      // We work around this by using our own name for the socket in this case.
      (remote_addr->sa_family == AF_UNIX)
          ? Address::peerAddressFromFd(fd)
          : Address::addressFromSockAddr(*reinterpret_cast<const sockaddr_storage*>(remote_addr),
                                         remote_addr_len)));
  listener->cb_.onAccept(std::move(socket), listener->hand_off_restored_destination_connections_);
}

ListenerImpl::ListenerImpl(Event::DispatcherImpl& dispatcher, ListenSocket& socket,
                           ListenerCallbacks& cb, bool bind_to_port,
                           bool hand_off_restored_destination_connections)
    : local_address_(nullptr), cb_(cb),
      hand_off_restored_destination_connections_(hand_off_restored_destination_connections),
      listener_(nullptr) {
  const auto ip = socket.localAddress()->ip();

  // Only use the listen socket's local address for new connections if it is not the all hosts
  // address (e.g., 0.0.0.0 for IPv4).
  if (!(ip && ip->isAnyAddress())) {
    local_address_ = socket.localAddress();
  }

  if (bind_to_port) {
    listener_.reset(
        evconnlistener_new(&dispatcher.base(), listenCallback, this, 0, -1, socket.fd()));

    if (!listener_) {
      throw CreateListenerException(
          fmt::format("cannot listen on socket: {}", socket.localAddress()->asString()));
    }

    evconnlistener_set_error_cb(listener_.get(), errorCallback);
  }
}

void ListenerImpl::errorCallback(evconnlistener*, void*) {
  // We should never get an error callback. This can happen if we run out of FDs or memory. In those
  // cases just crash.
  PANIC(fmt::format("listener accept failure: {}", strerror(errno)));
}

} // namespace Network
} // namespace Envoy
