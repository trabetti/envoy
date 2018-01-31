#pragma once

#include <functional>
#include <string>

#include "envoy/buffer/buffer.h"
#include "envoy/common/pure.h"
#include "envoy/http/codes.h"
#include "envoy/http/filter.h"
#include "envoy/http/header_map.h"
#include "envoy/network/listen_socket.h"

#include "common/stats/hystrix.h"

namespace Envoy {
namespace Server {

/**
 * This macro is used to add handlers to the Admin HTTP Endpoint. It builds
 * a callback that executes X when the specified admin handler is hit. This macro can be
 * used to add static handlers as in source/server/http/admin.cc and also dynamic handlers as
 * done in the RouteConfigProviderManagerImpl constructor in source/common/router/rds_impl.cc.
 */
#define MAKE_ADMIN_HANDLER(X)                                                                      \
  [this](const std::string& url, Http::HeaderMap& response_headers, Buffer::Instance& data,        \
         Server::HandlerInfo* handler_info) -> Http::Code {                                          \
    return X(url, response_headers, data, handler_info);                                            \
  }

/**
 * This class is a base class for data which will be sent from admin filter to a handler
 * in admin impl. Each handler which needs to receive data from admin filter can inherit from
 * HandlerInfo and build a class which contains the relevant data.
 */
class HandlerInfo {
public:
  HandlerInfo(){};
  virtual ~HandlerInfo(){};
  virtual void Destroy(){};
};

/**
 * This class contains data which will be sent from admin filter to a hystrix_event_stream handler
 * and build a class which contains the relevant data.
 */
class HystrixHandlerInfo : public HandlerInfo {
public:
  HystrixHandlerInfo(Http::StreamDecoderFilterCallbacks* callbacks)
      : stats_(new Stats::Hystrix()), data_timer_(nullptr),
        ping_timer_(nullptr), callbacks_(callbacks) {}
  virtual ~HystrixHandlerInfo(){};
  void Destroy() {
    if (data_timer_)
      data_timer_->disableTimer();
    if (ping_timer_)
      ping_timer_->disableTimer();
  }

  /**
   * Hystrix data includes statistics for hystrix API,timer for build (and send) data and keep
   * alive messages and the handler's callback
   */
  Stats::HystrixPtr stats_;
  Event::TimerPtr data_timer_;
  Event::TimerPtr ping_timer_;
  Http::StreamDecoderFilterCallbacks* callbacks_{};
};

/**
 * Global admin HTTP endpoint for the server.
 */
class Admin {
public:
  virtual ~Admin() {}

  /**
   * Callback for admin URL handlers.
   * @param url supplies the URL prefix to install the handler for.
   * @param response_headers enables setting of http headers (eg content-type, cache-control) in the
   * handler.
   * @param response supplies the buffer to fill in with the response body.
   * @return Http::Code the response code.
   */
  typedef std::function<Http::Code(const std::string& url, Http::HeaderMap& response_headers,
                                   Buffer::Instance& response, Server::HandlerInfo* handler_info)>
      HandlerCb;

  /**
   * Add an admin handler.
   * @param prefix supplies the URL prefix to handle.
   * @param help_text supplies the help text for the handler.
   * @param callback supplies the callback to invoke when the prefix matches.
   * @param removable if true allows the handler to be removed via removeHandler.
   * @param mutates_server_state indicates whether callback will mutate server state.
   * @return bool true if the handler was added, false if it was not added.
   */
  virtual bool addHandler(const std::string& prefix, const std::string& help_text,
                          HandlerCb callback, bool removable, bool mutates_server_state) PURE;

  /**
   * Remove an admin handler if it is removable.
   * @param prefix supplies the URL prefix of the handler to delete.
   * @return bool true if the handler was removed, false if it was not removed.
   */
  virtual bool removeHandler(const std::string& prefix) PURE;

  /**
   * Obtain socket the admin endpoint is bound to.
   * @return Network::ListenSocket& socket reference.
   */
  virtual const Network::ListenSocket& socket() PURE;
};

} // namespace Server
} // namespace Envoy
