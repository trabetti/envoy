#include <cstdint>
#include <memory>
#include <string>

#include "common/buffer/buffer_impl.h"
#include "common/config/filter_json.h"
#include "common/filter/tcp_proxy.h"
#include "common/network/address_impl.h"
#include "common/stats/stats_impl.h"
#include "common/upstream/upstream_impl.h"

#include "test/common/upstream/utility.h"
#include "test/mocks/buffer/mocks.h"
#include "test/mocks/network/mocks.h"
#include "test/mocks/runtime/mocks.h"
#include "test/mocks/server/mocks.h"
#include "test/mocks/upstream/host.h"
#include "test/mocks/upstream/mocks.h"
#include "test/test_common/printers.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::MatchesRegex;
using testing::NiceMock;
using testing::Return;
using testing::ReturnPointee;
using testing::ReturnRef;
using testing::SaveArg;
using testing::_;

namespace Envoy {
namespace Filter {

namespace {
TcpProxyConfig constructTcpProxyConfigFromJson(const Json::Object& json,
                                               Server::Configuration::FactoryContext& context) {
  envoy::api::v2::filter::network::TcpProxy tcp_proxy;
  Config::FilterJson::translateTcpProxy(json, tcp_proxy);
  return TcpProxyConfig(tcp_proxy, context);
}
} // namespace

TEST(TcpProxyConfigTest, NoRouteConfig) {
  std::string json = R"EOF(
    {
      "stat_prefix": "name"
    }
    )EOF";

  Json::ObjectSharedPtr config = Json::Factory::loadFromString(json);
  NiceMock<Server::Configuration::MockFactoryContext> factory_context;
  EXPECT_THROW(constructTcpProxyConfigFromJson(*config, factory_context), EnvoyException);
}

TEST(TcpProxyConfigTest, NoCluster) {
  std::string json = R"EOF(
    {
      "stat_prefix": "name",
      "route_config": {
        "routes": [
          {
            "cluster": "fake_cluster"
          }
        ]
      }
    }
    )EOF";

  Json::ObjectSharedPtr config = Json::Factory::loadFromString(json);
  NiceMock<Server::Configuration::MockFactoryContext> factory_context;
  EXPECT_CALL(factory_context.cluster_manager_, get("fake_cluster")).WillOnce(Return(nullptr));
  EXPECT_THROW(constructTcpProxyConfigFromJson(*config, factory_context), EnvoyException);
}

TEST(TcpProxyConfigTest, BadTcpProxyConfig) {
  std::string json_string = R"EOF(
  {
    "stat_prefix": 1,
    "route_config": {
      "routes": [
        {
          "cluster": "fake_cluster"
        }
      ]
    }
   }
  )EOF";

  Json::ObjectSharedPtr json_config = Json::Factory::loadFromString(json_string);
  NiceMock<Server::Configuration::MockFactoryContext> factory_context;
  EXPECT_THROW(constructTcpProxyConfigFromJson(*json_config, factory_context), Json::Exception);
}

TEST(TcpProxyConfigTest, Routes) {
  std::string json = R"EOF(
    {
      "stat_prefix": "name",
      "route_config": {
        "routes": [
          {
            "destination_ip_list": [
              "10.10.10.10/32",
              "10.10.11.0/24",
              "10.11.0.0/16",
              "11.0.0.0/8",
              "128.0.0.0/1"
            ],
            "cluster": "with_destination_ip_list"
          },
          {
            "destination_ip_list": [
              "::1/128",
              "2001:abcd::/64"
            ],
            "cluster": "with_v6_destination"
          },
          {
            "destination_ports": "1-1024,2048-4096,12345",
            "cluster": "with_destination_ports"
          },
          {
            "source_ports": "23457,23459",
            "cluster": "with_source_ports"
          },
          {
            "destination_ip_list": [
              "2002::/32"
            ],
            "source_ip_list": [
              "2003::/64"
            ],
            "cluster": "with_v6_source_and_destination"
          },
          {
            "destination_ip_list": [
              "10.0.0.0/24"
            ],
            "source_ip_list": [
              "20.0.0.0/24"
            ],
            "destination_ports" : "10000",
            "source_ports": "20000",
            "cluster": "with_everything"
          },
          {
            "cluster": "catch_all"
          }
        ]
      }
    }
    )EOF";

  Json::ObjectSharedPtr json_config = Json::Factory::loadFromString(json);
  NiceMock<Server::Configuration::MockFactoryContext> factory_context_;

  TcpProxyConfig config_obj(constructTcpProxyConfigFromJson(*json_config, factory_context_));

  {
    // hit route with destination_ip (10.10.10.10/32)
    NiceMock<Network::MockConnection> connection;
    connection.local_address_ = std::make_shared<Network::Address::Ipv4Instance>("10.10.10.10");
    EXPECT_EQ(std::string("with_destination_ip_list"), config_obj.getRouteFromEntries(connection));
  }

  {
    // fall-through
    NiceMock<Network::MockConnection> connection;
    connection.local_address_ = std::make_shared<Network::Address::Ipv4Instance>("10.10.10.11");
    connection.remote_address_ = std::make_shared<Network::Address::Ipv4Instance>("0.0.0.0");
    EXPECT_EQ(std::string("catch_all"), config_obj.getRouteFromEntries(connection));
  }

  {
    // hit route with destination_ip (10.10.11.0/24)
    NiceMock<Network::MockConnection> connection;
    connection.local_address_ = std::make_shared<Network::Address::Ipv4Instance>("10.10.11.11");
    EXPECT_EQ(std::string("with_destination_ip_list"), config_obj.getRouteFromEntries(connection));
  }

  {
    // fall-through
    NiceMock<Network::MockConnection> connection;
    connection.local_address_ = std::make_shared<Network::Address::Ipv4Instance>("10.10.12.12");
    connection.remote_address_ = std::make_shared<Network::Address::Ipv4Instance>("0.0.0.0");
    EXPECT_EQ(std::string("catch_all"), config_obj.getRouteFromEntries(connection));
  }

  {
    // hit route with destination_ip (10.11.0.0/16)
    NiceMock<Network::MockConnection> connection;
    connection.local_address_ = std::make_shared<Network::Address::Ipv4Instance>("10.11.11.11");
    EXPECT_EQ(std::string("with_destination_ip_list"), config_obj.getRouteFromEntries(connection));
  }

  {
    // fall-through
    NiceMock<Network::MockConnection> connection;
    connection.local_address_ = std::make_shared<Network::Address::Ipv4Instance>("10.12.12.12");
    connection.remote_address_ = std::make_shared<Network::Address::Ipv4Instance>("0.0.0.0");
    EXPECT_EQ(std::string("catch_all"), config_obj.getRouteFromEntries(connection));
  }

  {
    // hit route with destination_ip (11.0.0.0/8)
    NiceMock<Network::MockConnection> connection;
    connection.local_address_ = std::make_shared<Network::Address::Ipv4Instance>("11.11.11.11");
    EXPECT_EQ(std::string("with_destination_ip_list"), config_obj.getRouteFromEntries(connection));
  }

  {
    // fall-through
    NiceMock<Network::MockConnection> connection;
    connection.local_address_ = std::make_shared<Network::Address::Ipv4Instance>("12.12.12.12");
    connection.remote_address_ = std::make_shared<Network::Address::Ipv4Instance>("0.0.0.0");
    EXPECT_EQ(std::string("catch_all"), config_obj.getRouteFromEntries(connection));
  }

  {
    // hit route with destination_ip (128.0.0.0/8)
    NiceMock<Network::MockConnection> connection;
    connection.local_address_ = std::make_shared<Network::Address::Ipv4Instance>("128.255.255.255");
    EXPECT_EQ(std::string("with_destination_ip_list"), config_obj.getRouteFromEntries(connection));
  }

  {
    // hit route with destination port range
    NiceMock<Network::MockConnection> connection;
    connection.local_address_ = std::make_shared<Network::Address::Ipv4Instance>("1.2.3.4", 12345);
    EXPECT_EQ(std::string("with_destination_ports"), config_obj.getRouteFromEntries(connection));
  }

  {
    // fall through
    NiceMock<Network::MockConnection> connection;
    connection.local_address_ = std::make_shared<Network::Address::Ipv4Instance>("1.2.3.4", 23456);
    connection.remote_address_ = std::make_shared<Network::Address::Ipv4Instance>("0.0.0.0");
    EXPECT_EQ(std::string("catch_all"), config_obj.getRouteFromEntries(connection));
  }

  {
    // hit route with source port range
    NiceMock<Network::MockConnection> connection;
    connection.local_address_ = std::make_shared<Network::Address::Ipv4Instance>("1.2.3.4", 23456);
    connection.remote_address_ = std::make_shared<Network::Address::Ipv4Instance>("0.0.0.0", 23459);
    EXPECT_EQ(std::string("with_source_ports"), config_obj.getRouteFromEntries(connection));
  }

  {
    // fall through
    NiceMock<Network::MockConnection> connection;
    connection.local_address_ = std::make_shared<Network::Address::Ipv4Instance>("1.2.3.4", 23456);
    connection.remote_address_ = std::make_shared<Network::Address::Ipv4Instance>("0.0.0.0", 23458);
    EXPECT_EQ(std::string("catch_all"), config_obj.getRouteFromEntries(connection));
  }

  {
    // hit the route with all criterias present
    NiceMock<Network::MockConnection> connection;
    connection.local_address_ = std::make_shared<Network::Address::Ipv4Instance>("10.0.0.0", 10000);
    connection.remote_address_ =
        std::make_shared<Network::Address::Ipv4Instance>("20.0.0.0", 20000);
    EXPECT_EQ(std::string("with_everything"), config_obj.getRouteFromEntries(connection));
  }

  {
    // fall through
    NiceMock<Network::MockConnection> connection;
    connection.local_address_ = std::make_shared<Network::Address::Ipv4Instance>("10.0.0.0", 10000);
    connection.remote_address_ =
        std::make_shared<Network::Address::Ipv4Instance>("30.0.0.0", 20000);
    EXPECT_EQ(std::string("catch_all"), config_obj.getRouteFromEntries(connection));
  }

  {
    // hit route with destination_ip (::1/128)
    NiceMock<Network::MockConnection> connection;
    connection.local_address_ = std::make_shared<Network::Address::Ipv6Instance>("::1");
    EXPECT_EQ(std::string("with_v6_destination"), config_obj.getRouteFromEntries(connection));
  }

  {
    // hit route with destination_ip (2001:abcd/64")
    NiceMock<Network::MockConnection> connection;
    connection.local_address_ =
        std::make_shared<Network::Address::Ipv6Instance>("2001:abcd:0:0:1::");
    EXPECT_EQ(std::string("with_v6_destination"), config_obj.getRouteFromEntries(connection));
  }

  {
    // hit route with destination_ip ("2002::/32") and source_ip ("2003::/64")
    NiceMock<Network::MockConnection> connection;
    connection.local_address_ =
        std::make_shared<Network::Address::Ipv6Instance>("2002:0:0:0:0:0::1");
    connection.remote_address_ =
        std::make_shared<Network::Address::Ipv6Instance>("2003:0:0:0:0::5");
    EXPECT_EQ(std::string("with_v6_source_and_destination"),
              config_obj.getRouteFromEntries(connection));
  }

  {
    // fall through
    NiceMock<Network::MockConnection> connection;
    connection.local_address_ = std::make_shared<Network::Address::Ipv6Instance>("2004::");
    connection.remote_address_ = std::make_shared<Network::Address::Ipv6Instance>("::");
    EXPECT_EQ(std::string("catch_all"), config_obj.getRouteFromEntries(connection));
  }
}

TEST(TcpProxyConfigTest, EmptyRouteConfig) {
  std::string json = R"EOF(
    {
      "stat_prefix": "name",
      "route_config": {
        "routes": [
        ]
      }
    }
    )EOF";

  Json::ObjectSharedPtr json_config = Json::Factory::loadFromString(json);
  NiceMock<Server::Configuration::MockFactoryContext> factory_context_;

  TcpProxyConfig config_obj(constructTcpProxyConfigFromJson(*json_config, factory_context_));

  NiceMock<Network::MockConnection> connection;
  EXPECT_EQ(std::string(""), config_obj.getRouteFromEntries(connection));
}

TEST(TcpProxyConfigTest, AccessLogConfig) {
  envoy::api::v2::filter::network::TcpProxy config;
  envoy::api::v2::filter::accesslog::AccessLog* log = config.mutable_access_log()->Add();
  log->set_name(Config::AccessLogNames::get().FILE);
  {
    envoy::api::v2::filter::accesslog::FileAccessLog file_access_log;
    file_access_log.set_path("some_path");
    file_access_log.set_format("the format specifier");
    ProtobufWkt::Struct* custom_config = log->mutable_config();
    MessageUtil::jsonConvert(file_access_log, *custom_config);
  }

  log = config.mutable_access_log()->Add();
  log->set_name(Config::AccessLogNames::get().FILE);
  {
    envoy::api::v2::filter::accesslog::FileAccessLog file_access_log;
    file_access_log.set_path("another path");
    ProtobufWkt::Struct* custom_config = log->mutable_config();
    MessageUtil::jsonConvert(file_access_log, *custom_config);
  }

  NiceMock<Server::Configuration::MockFactoryContext> factory_context_;
  TcpProxyConfig config_obj(config, factory_context_);

  EXPECT_EQ(2, config_obj.accessLogs().size());
}

class TcpProxyNoConfigTest : public testing::Test {
public:
  TcpProxyNoConfigTest() {}

  NiceMock<Network::MockReadFilterCallbacks> filter_callbacks_;
  NiceMock<Server::Configuration::MockFactoryContext> factory_context_;
  std::unique_ptr<TcpProxy> filter_;
};

TEST_F(TcpProxyNoConfigTest, Initialization) {
  filter_.reset(new TcpProxy(nullptr, factory_context_.cluster_manager_));
  filter_->initializeReadFilterCallbacks(filter_callbacks_);
}

TEST_F(TcpProxyNoConfigTest, ReadDisableDownstream) {
  filter_.reset(new TcpProxy(nullptr, factory_context_.cluster_manager_));
  filter_->initializeReadFilterCallbacks(filter_callbacks_);

  filter_->readDisableDownstream(true);
}

class TcpProxyTest : public testing::Test {
public:
  TcpProxyTest() {
    ON_CALL(*factory_context_.access_log_manager_.file_, write(_))
        .WillByDefault(SaveArg<0>(&access_log_data_));
  }

  void configure(const envoy::api::v2::filter::network::TcpProxy& config) {
    config_.reset(new TcpProxyConfig(config, factory_context_));
  }

  envoy::api::v2::filter::network::TcpProxy defaultConfig() {
    envoy::api::v2::filter::network::TcpProxy config;
    config.set_stat_prefix("name");
    auto* route = config.mutable_deprecated_v1()->mutable_routes()->Add();
    route->set_cluster("fake_cluster");

    return config;
  }

  // Return the default config, plus one file access log with the specified format
  envoy::api::v2::filter::network::TcpProxy accessLogConfig(const std::string access_log_format) {
    envoy::api::v2::filter::network::TcpProxy config = defaultConfig();
    envoy::api::v2::filter::accesslog::AccessLog* access_log = config.mutable_access_log()->Add();
    access_log->set_name(Config::AccessLogNames::get().FILE);
    envoy::api::v2::filter::accesslog::FileAccessLog file_access_log;
    file_access_log.set_path("unused");
    file_access_log.set_format(access_log_format);
    MessageUtil::jsonConvert(file_access_log, *access_log->mutable_config());

    return config;
  }

  void setup(uint32_t connections, const envoy::api::v2::filter::network::TcpProxy& config) {
    configure(config);
    upstream_local_address_ = Network::Utility::resolveUrl("tcp://2.2.2.2:50000");
    upstream_remote_address_ = Network::Utility::resolveUrl("tcp://127.0.0.1:80");
    if (connections >= 1) {
      {
        testing::InSequence sequence;
        for (uint32_t i = 0; i < connections; i++) {
          connect_timers_.push_back(
              new NiceMock<Event::MockTimer>(&filter_callbacks_.connection_.dispatcher_));
          EXPECT_CALL(*connect_timers_.at(i), enableTimer(_));
        }
      }

      for (uint32_t i = 0; i < connections; i++) {
        upstream_connections_.push_back(new NiceMock<Network::MockClientConnection>());
        upstream_hosts_.push_back(std::make_shared<NiceMock<Upstream::MockHost>>());
        conn_infos_.push_back(Upstream::MockHost::MockCreateConnectionData());
        conn_infos_.at(i).connection_ = upstream_connections_.back();
        conn_infos_.at(i).host_description_ = upstream_hosts_.back();

        ON_CALL(*upstream_hosts_.at(i), cluster())
            .WillByDefault(ReturnPointee(
                factory_context_.cluster_manager_.thread_local_cluster_.cluster_.info_));
        ON_CALL(*upstream_hosts_.at(i), address()).WillByDefault(Return(upstream_remote_address_));
        upstream_connections_.at(i)->local_address_ = upstream_local_address_;
        EXPECT_CALL(*upstream_connections_.at(i), addReadFilter(_))
            .WillOnce(SaveArg<0>(&upstream_read_filter_));
        EXPECT_CALL(*upstream_connections_.at(i), dispatcher())
            .WillRepeatedly(ReturnRef(filter_callbacks_.connection_.dispatcher_));
      }
    }

    {
      testing::InSequence sequence;
      for (uint32_t i = 0; i < connections; i++) {
        EXPECT_CALL(factory_context_.cluster_manager_, tcpConnForCluster_("fake_cluster", _))
            .WillOnce(Return(conn_infos_.at(i)))
            .RetiresOnSaturation();
      }
      EXPECT_CALL(factory_context_.cluster_manager_, tcpConnForCluster_("fake_cluster", _))
          .WillRepeatedly(Return(Upstream::MockHost::MockCreateConnectionData()));
    }

    filter_.reset(new TcpProxy(config_, factory_context_.cluster_manager_));
    EXPECT_CALL(filter_callbacks_.connection_, readDisable(true));
    filter_->initializeReadFilterCallbacks(filter_callbacks_);
    EXPECT_EQ(connections >= 1 ? Network::FilterStatus::Continue
                               : Network::FilterStatus::StopIteration,
              filter_->onNewConnection());

    EXPECT_EQ(Optional<uint64_t>(), filter_->computeHashKey());
    EXPECT_EQ(&filter_callbacks_.connection_, filter_->downstreamConnection());
  }

  void setup(uint32_t connections) { setup(connections, defaultConfig()); }

  void raiseEventUpstreamConnected(uint32_t conn_index) {
    EXPECT_CALL(*connect_timers_.at(conn_index), disableTimer());
    EXPECT_CALL(filter_callbacks_.connection_, readDisable(false));
    upstream_connections_.at(conn_index)->raiseEvent(Network::ConnectionEvent::Connected);
  }

  TcpProxyConfigSharedPtr config_;
  NiceMock<Network::MockReadFilterCallbacks> filter_callbacks_;
  NiceMock<Server::Configuration::MockFactoryContext> factory_context_;
  std::vector<std::shared_ptr<NiceMock<Upstream::MockHost>>> upstream_hosts_{};
  std::vector<NiceMock<Network::MockClientConnection>*> upstream_connections_{};
  std::vector<Upstream::MockHost::MockCreateConnectionData> conn_infos_;
  Network::ReadFilterSharedPtr upstream_read_filter_;
  std::vector<NiceMock<Event::MockTimer>*> connect_timers_;
  std::unique_ptr<TcpProxy> filter_;
  std::string access_log_data_;
  Network::Address::InstanceConstSharedPtr upstream_local_address_;
  Network::Address::InstanceConstSharedPtr upstream_remote_address_;
};

TEST_F(TcpProxyTest, UpstreamDisconnect) {
  setup(1);

  Buffer::OwnedImpl buffer("hello");
  EXPECT_CALL(*upstream_connections_.at(0), write(BufferEqual(&buffer)));
  filter_->onData(buffer);

  raiseEventUpstreamConnected(0);

  Buffer::OwnedImpl response("world");
  EXPECT_CALL(filter_callbacks_.connection_, write(BufferEqual(&response)));
  upstream_read_filter_->onData(response);

  EXPECT_CALL(filter_callbacks_.connection_, close(Network::ConnectionCloseType::FlushWrite));
  upstream_connections_.at(0)->raiseEvent(Network::ConnectionEvent::RemoteClose);
}

// Test that reconnect is attempted after a connect failure
TEST_F(TcpProxyTest, ConnectAttemptsUpstreamFail) {
  envoy::api::v2::filter::network::TcpProxy config = defaultConfig();
  config.mutable_max_connect_attempts()->set_value(2);
  setup(2, config);

  EXPECT_CALL(*upstream_connections_.at(0), close(Network::ConnectionCloseType::NoFlush));
  upstream_connections_.at(0)->raiseEvent(Network::ConnectionEvent::RemoteClose);
  raiseEventUpstreamConnected(1);

  EXPECT_EQ(0U, factory_context_.cluster_manager_.thread_local_cluster_.cluster_.info_->stats_store_
                    .counter("upstream_cx_connect_attempts_exceeded")
                    .value());
}

// Test that reconnect is attempted after a connect timeout
TEST_F(TcpProxyTest, ConnectAttemptsUpstreamTimeout) {
  envoy::api::v2::filter::network::TcpProxy config = defaultConfig();
  config.mutable_max_connect_attempts()->set_value(2);
  setup(2, config);

  EXPECT_CALL(*upstream_connections_.at(0), close(Network::ConnectionCloseType::NoFlush));
  connect_timers_.at(0)->callback_();
  raiseEventUpstreamConnected(1);

  EXPECT_EQ(0U, factory_context_.cluster_manager_.thread_local_cluster_.cluster_.info_->stats_store_
                    .counter("upstream_cx_connect_attempts_exceeded")
                    .value());
}

// Test that only the configured number of connect attempts occur
TEST_F(TcpProxyTest, ConnectAttemptsLimit) {
  envoy::api::v2::filter::network::TcpProxy config = defaultConfig();
  config.mutable_max_connect_attempts()->set_value(3);
  setup(3, config);

  {
    testing::InSequence sequence;
    EXPECT_CALL(*upstream_connections_.at(0), close(Network::ConnectionCloseType::NoFlush));
    EXPECT_CALL(*upstream_connections_.at(1), close(Network::ConnectionCloseType::NoFlush));
    EXPECT_CALL(*upstream_connections_.at(2), close(Network::ConnectionCloseType::NoFlush));
    EXPECT_CALL(filter_callbacks_.connection_, close(Network::ConnectionCloseType::NoFlush));
  }

  // Try both failure modes
  connect_timers_.at(0)->callback_();
  upstream_connections_.at(1)->raiseEvent(Network::ConnectionEvent::RemoteClose);
  upstream_connections_.at(2)->raiseEvent(Network::ConnectionEvent::RemoteClose);

  EXPECT_EQ(1U, factory_context_.cluster_manager_.thread_local_cluster_.cluster_.info_->stats_store_
                    .counter("upstream_cx_connect_timeout")
                    .value());
  EXPECT_EQ(2U, factory_context_.cluster_manager_.thread_local_cluster_.cluster_.info_->stats_store_
                    .counter("upstream_cx_connect_fail")
                    .value());
  EXPECT_EQ(1U, factory_context_.cluster_manager_.thread_local_cluster_.cluster_.info_->stats_store_
                    .counter("upstream_cx_connect_attempts_exceeded")
                    .value());
  EXPECT_EQ(0U, factory_context_.cluster_manager_.thread_local_cluster_.cluster_.info_->stats_store_
                    .counter("upstream_cx_overflow")
                    .value());
  EXPECT_EQ(0U, factory_context_.cluster_manager_.thread_local_cluster_.cluster_.info_->stats_store_
                    .counter("upstream_cx_no_successful_host")
                    .value());
}

// Test that the tcp proxy sends the correct notifications to the outlier detector
TEST_F(TcpProxyTest, OutlierDetection) {
  envoy::api::v2::filter::network::TcpProxy config = defaultConfig();
  config.mutable_max_connect_attempts()->set_value(3);
  setup(3, config);

  EXPECT_CALL(upstream_hosts_.at(0)->outlier_detector_,
              putResult(Upstream::Outlier::Result::TIMEOUT));
  connect_timers_.at(0)->callback_();

  EXPECT_CALL(upstream_hosts_.at(1)->outlier_detector_,
              putResult(Upstream::Outlier::Result::CONNECT_FAILED));
  upstream_connections_.at(1)->raiseEvent(Network::ConnectionEvent::RemoteClose);

  EXPECT_CALL(upstream_hosts_.at(2)->outlier_detector_,
              putResult(Upstream::Outlier::Result::SUCCESS));
  raiseEventUpstreamConnected(2);
}

TEST_F(TcpProxyTest, UpstreamDisconnectDownstreamFlowControl) {
  setup(1);

  Buffer::OwnedImpl buffer("hello");
  EXPECT_CALL(*upstream_connections_.at(0), write(BufferEqual(&buffer)));
  filter_->onData(buffer);

  raiseEventUpstreamConnected(0);

  Buffer::OwnedImpl response("world");
  EXPECT_CALL(filter_callbacks_.connection_, write(BufferEqual(&response)));
  upstream_read_filter_->onData(response);

  EXPECT_CALL(*upstream_connections_.at(0), readDisable(true));
  filter_callbacks_.connection_.runHighWatermarkCallbacks();

  EXPECT_CALL(filter_callbacks_.connection_, close(Network::ConnectionCloseType::FlushWrite));
  upstream_connections_.at(0)->raiseEvent(Network::ConnectionEvent::RemoteClose);

  filter_callbacks_.connection_.runLowWatermarkCallbacks();
}

TEST_F(TcpProxyTest, DownstreamDisconnectRemote) {
  setup(1);

  Buffer::OwnedImpl buffer("hello");
  EXPECT_CALL(*upstream_connections_.at(0), write(BufferEqual(&buffer)));
  filter_->onData(buffer);

  raiseEventUpstreamConnected(0);

  Buffer::OwnedImpl response("world");
  EXPECT_CALL(filter_callbacks_.connection_, write(BufferEqual(&response)));
  upstream_read_filter_->onData(response);

  EXPECT_CALL(*upstream_connections_.at(0), close(Network::ConnectionCloseType::FlushWrite));
  filter_callbacks_.connection_.raiseEvent(Network::ConnectionEvent::RemoteClose);
}

TEST_F(TcpProxyTest, DownstreamDisconnectLocal) {
  setup(1);

  Buffer::OwnedImpl buffer("hello");
  EXPECT_CALL(*upstream_connections_.at(0), write(BufferEqual(&buffer)));
  filter_->onData(buffer);

  raiseEventUpstreamConnected(0);

  Buffer::OwnedImpl response("world");
  EXPECT_CALL(filter_callbacks_.connection_, write(BufferEqual(&response)));
  upstream_read_filter_->onData(response);

  EXPECT_CALL(*upstream_connections_.at(0), close(Network::ConnectionCloseType::NoFlush));
  filter_callbacks_.connection_.raiseEvent(Network::ConnectionEvent::LocalClose);
}

TEST_F(TcpProxyTest, UpstreamConnectTimeout) {
  setup(1, accessLogConfig("%RESPONSE_FLAGS%"));

  Buffer::OwnedImpl buffer("hello");
  EXPECT_CALL(*upstream_connections_.at(0), write(BufferEqual(&buffer)));
  filter_->onData(buffer);

  EXPECT_CALL(filter_callbacks_.connection_, close(Network::ConnectionCloseType::NoFlush));
  EXPECT_CALL(*upstream_connections_.at(0), close(Network::ConnectionCloseType::NoFlush));
  connect_timers_.at(0)->callback_();
  EXPECT_EQ(1U, factory_context_.cluster_manager_.thread_local_cluster_.cluster_.info_->stats_store_
                    .counter("upstream_cx_connect_timeout")
                    .value());

  filter_.reset();
  EXPECT_EQ(access_log_data_, "UF");
}

TEST_F(TcpProxyTest, NoHost) {
  EXPECT_CALL(filter_callbacks_.connection_, close(Network::ConnectionCloseType::NoFlush));
  setup(0, accessLogConfig("%RESPONSE_FLAGS%"));
  filter_.reset();
  EXPECT_EQ(access_log_data_, "UH");
}

TEST_F(TcpProxyTest, DisconnectBeforeData) {
  configure(defaultConfig());
  filter_.reset(new TcpProxy(config_, factory_context_.cluster_manager_));
  filter_->initializeReadFilterCallbacks(filter_callbacks_);

  filter_callbacks_.connection_.raiseEvent(Network::ConnectionEvent::RemoteClose);
}

TEST_F(TcpProxyTest, UpstreamConnectFailure) {
  setup(1, accessLogConfig("%RESPONSE_FLAGS%"));

  Buffer::OwnedImpl buffer("hello");
  filter_->onData(buffer);

  EXPECT_CALL(filter_callbacks_.connection_, close(Network::ConnectionCloseType::NoFlush));
  EXPECT_CALL(*connect_timers_.at(0), disableTimer());
  upstream_connections_.at(0)->raiseEvent(Network::ConnectionEvent::RemoteClose);
  EXPECT_EQ(1U, factory_context_.cluster_manager_.thread_local_cluster_.cluster_.info_->stats_store_
                    .counter("upstream_cx_connect_fail")
                    .value());

  filter_.reset();
  EXPECT_EQ(access_log_data_, "UF");
}

TEST_F(TcpProxyTest, UpstreamConnectionLimit) {
  configure(accessLogConfig("%RESPONSE_FLAGS%"));
  factory_context_.cluster_manager_.thread_local_cluster_.cluster_.info_->resource_manager_.reset(
      new Upstream::ResourceManagerImpl(factory_context_.runtime_loader_, "fake_key", 0, 0, 0, 0));

  // setup sets up expectation for tcpConnForCluster but this test is expected to NOT call that
  filter_.reset(new TcpProxy(config_, factory_context_.cluster_manager_));
  // The downstream connection closes if the proxy can't make an upstream connection.
  EXPECT_CALL(filter_callbacks_.connection_, close(Network::ConnectionCloseType::NoFlush));
  filter_->initializeReadFilterCallbacks(filter_callbacks_);
  filter_->onNewConnection();

  EXPECT_EQ(1U, factory_context_.cluster_manager_.thread_local_cluster_.cluster_.info_->stats_store_
                    .counter("upstream_cx_overflow")
                    .value());

  filter_.reset();
  EXPECT_EQ(access_log_data_, "UO");
}

// Tests that the idle timer closes both connections, and gets updated when either
// connection has activity.
TEST_F(TcpProxyTest, IdleTimeout) {
  envoy::api::v2::filter::network::TcpProxy config = defaultConfig();
  config.mutable_idle_timeout()->set_seconds(1);
  setup(1, config);

  Event::MockTimer* idle_timer = new Event::MockTimer(&filter_callbacks_.connection_.dispatcher_);
  EXPECT_CALL(*idle_timer, enableTimer(std::chrono::milliseconds(1000)));
  raiseEventUpstreamConnected(0);

  Buffer::OwnedImpl buffer("hello");
  EXPECT_CALL(*idle_timer, enableTimer(std::chrono::milliseconds(1000)));
  filter_->onData(buffer);

  buffer.add("hello2");
  EXPECT_CALL(*idle_timer, enableTimer(std::chrono::milliseconds(1000)));
  upstream_read_filter_->onData(buffer);

  EXPECT_CALL(*idle_timer, enableTimer(std::chrono::milliseconds(1000)));
  filter_callbacks_.connection_.raiseBytesSentCallbacks(1);

  EXPECT_CALL(*idle_timer, enableTimer(std::chrono::milliseconds(1000)));
  upstream_connections_.at(0)->raiseBytesSentCallbacks(2);

  EXPECT_CALL(*idle_timer, disableTimer());
  EXPECT_CALL(*upstream_connections_.at(0), close(Network::ConnectionCloseType::NoFlush));
  EXPECT_CALL(filter_callbacks_.connection_, close(Network::ConnectionCloseType::NoFlush));
  idle_timer->callback_();
}

// Tests that the idle timer is disabled when the downstream connection is closed.
TEST_F(TcpProxyTest, IdleTimerDisabledDownstreamClose) {
  envoy::api::v2::filter::network::TcpProxy config = defaultConfig();
  config.mutable_idle_timeout()->set_seconds(1);
  setup(1, config);

  Event::MockTimer* idle_timer = new Event::MockTimer(&filter_callbacks_.connection_.dispatcher_);
  EXPECT_CALL(*idle_timer, enableTimer(std::chrono::milliseconds(1000)));
  raiseEventUpstreamConnected(0);

  EXPECT_CALL(*idle_timer, disableTimer());
  filter_callbacks_.connection_.raiseEvent(Network::ConnectionEvent::RemoteClose);
}

// Tests that the idle timer is disabled when the upstream connection is closed.
TEST_F(TcpProxyTest, IdleTimerDisabledUpstreamClose) {
  envoy::api::v2::filter::network::TcpProxy config = defaultConfig();
  config.mutable_idle_timeout()->set_seconds(1);
  setup(1, config);

  Event::MockTimer* idle_timer = new Event::MockTimer(&filter_callbacks_.connection_.dispatcher_);
  EXPECT_CALL(*idle_timer, enableTimer(std::chrono::milliseconds(1000)));
  raiseEventUpstreamConnected(0);

  EXPECT_CALL(*idle_timer, disableTimer());
  upstream_connections_.at(0)->raiseEvent(Network::ConnectionEvent::RemoteClose);
}

// Test that access log fields %UPSTREAM_HOST% and %UPSTREAM_CLUSTER% are correctly logged.
TEST_F(TcpProxyTest, AccessLogUpstreamHost) {
  setup(1, accessLogConfig("%UPSTREAM_HOST% %UPSTREAM_CLUSTER%"));
  filter_.reset();
  EXPECT_EQ(access_log_data_, "127.0.0.1:80 fake_cluster");
}

// Test that access log field %UPSTREAM_LOCAL_ADDRESS% is correctly logged.
TEST_F(TcpProxyTest, AccessLogUpstreamLocalAddress) {
  setup(1, accessLogConfig("%UPSTREAM_LOCAL_ADDRESS%"));
  filter_.reset();
  EXPECT_EQ(access_log_data_, "2.2.2.2:50000");
}

// Test that access log fields %DOWNSTREAM_ADDRESS% and %DOWNSTREAM_LOCAL_ADDRESS% are correctly
// logged.
TEST_F(TcpProxyTest, AccessLogDownstreamAddress) {
  filter_callbacks_.connection_.local_address_ =
      Network::Utility::resolveUrl("tcp://1.1.1.2:20000");
  filter_callbacks_.connection_.remote_address_ =
      Network::Utility::resolveUrl("tcp://1.1.1.1:40000");
  setup(1, accessLogConfig("%DOWNSTREAM_ADDRESS% %DOWNSTREAM_LOCAL_ADDRESS%"));
  filter_.reset();
  EXPECT_EQ(access_log_data_, "1.1.1.1 1.1.1.2:20000");
}

// Test that access log fields %BYTES_RECEIVED%, %BYTES_SENT%, %START_TIME%, %DURATION% are
// all correctly logged.
TEST_F(TcpProxyTest, AccessLogBytesRxTxDuration) {
  setup(1, accessLogConfig("bytesreceived=%BYTES_RECEIVED% bytessent=%BYTES_SENT% "
                           "datetime=%START_TIME% nonzeronum=%DURATION%"));

  raiseEventUpstreamConnected(0);
  Buffer::OwnedImpl buffer("a");
  filter_->onData(buffer);
  Buffer::OwnedImpl response("bb");
  upstream_read_filter_->onData(response);

  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  upstream_connections_.at(0)->raiseEvent(Network::ConnectionEvent::RemoteClose);
  filter_.reset();

  EXPECT_THAT(access_log_data_,
              MatchesRegex(
                  "bytesreceived=1 bytessent=2 datetime=[0-9-]+T[0-9:.]+Z nonzeronum=[1-9][0-9]*"));
}

// Tests that upstream flush works properly with no idle timeout configured.
TEST_F(TcpProxyTest, UpstreamFlushNoTimeout) {
  setup(1);
  raiseEventUpstreamConnected(0);

  EXPECT_CALL(*upstream_connections_.at(0), close(Network::ConnectionCloseType::FlushWrite))
      .WillOnce(Return()); // Cancel default action of raising LocalClose
  EXPECT_CALL(*upstream_connections_.at(0), state())
      .WillOnce(Return(Network::Connection::State::Closing));
  filter_callbacks_.connection_.raiseEvent(Network::ConnectionEvent::RemoteClose);
  filter_.reset();

  EXPECT_EQ(1U, config_->stats().upstream_flush_active_.value());

  // Send some bytes; no timeout configured so this should be a no-op (not a crash).
  upstream_connections_.at(0)->raiseBytesSentCallbacks(1);

  // Simulate flush complete.
  upstream_connections_.at(0)->raiseEvent(Network::ConnectionEvent::LocalClose);
  EXPECT_EQ(1U, config_->stats().upstream_flush_total_.value());
  EXPECT_EQ(0U, config_->stats().upstream_flush_active_.value());
}

// Tests that upstream flush works with an idle timeout configured, but the connection
// finishes draining before the timer expires.
TEST_F(TcpProxyTest, UpstreamFlushTimeoutConfigured) {
  envoy::api::v2::filter::network::TcpProxy config = defaultConfig();
  config.mutable_idle_timeout()->set_seconds(1);
  setup(1, config);

  NiceMock<Event::MockTimer>* idle_timer =
      new NiceMock<Event::MockTimer>(&filter_callbacks_.connection_.dispatcher_);
  EXPECT_CALL(*idle_timer, enableTimer(_));
  raiseEventUpstreamConnected(0);

  EXPECT_CALL(*upstream_connections_.at(0), close(Network::ConnectionCloseType::FlushWrite))
      .WillOnce(Return()); // Cancel default action of raising LocalClose
  EXPECT_CALL(*upstream_connections_.at(0), state())
      .WillOnce(Return(Network::Connection::State::Closing));
  filter_callbacks_.connection_.raiseEvent(Network::ConnectionEvent::RemoteClose);

  filter_.reset();
  EXPECT_EQ(1U, config_->stats().upstream_flush_active_.value());

  EXPECT_CALL(*idle_timer, enableTimer(std::chrono::milliseconds(1000)));
  upstream_connections_.at(0)->raiseBytesSentCallbacks(1);

  // Simulate flush complete.
  EXPECT_CALL(*idle_timer, disableTimer());
  upstream_connections_.at(0)->raiseEvent(Network::ConnectionEvent::LocalClose);
  EXPECT_EQ(1U, config_->stats().upstream_flush_total_.value());
  EXPECT_EQ(0U, config_->stats().upstream_flush_active_.value());
  EXPECT_EQ(0U, config_->stats().idle_timeout_.value());
}

// Tests that upstream flush closes the connection when the idle timeout fires.
TEST_F(TcpProxyTest, UpstreamFlushTimeoutExpired) {
  envoy::api::v2::filter::network::TcpProxy config = defaultConfig();
  config.mutable_idle_timeout()->set_seconds(1);
  setup(1, config);

  NiceMock<Event::MockTimer>* idle_timer =
      new NiceMock<Event::MockTimer>(&filter_callbacks_.connection_.dispatcher_);
  EXPECT_CALL(*idle_timer, enableTimer(_));
  raiseEventUpstreamConnected(0);

  EXPECT_CALL(*upstream_connections_.at(0), close(Network::ConnectionCloseType::FlushWrite))
      .WillOnce(Return()); // Cancel default action of raising LocalClose
  EXPECT_CALL(*upstream_connections_.at(0), state())
      .WillOnce(Return(Network::Connection::State::Closing));
  filter_callbacks_.connection_.raiseEvent(Network::ConnectionEvent::RemoteClose);

  filter_.reset();
  EXPECT_EQ(1U, config_->stats().upstream_flush_active_.value());

  EXPECT_CALL(*upstream_connections_.at(0), close(Network::ConnectionCloseType::NoFlush));
  idle_timer->callback_();
  EXPECT_EQ(1U, config_->stats().upstream_flush_total_.value());
  EXPECT_EQ(0U, config_->stats().upstream_flush_active_.value());
  EXPECT_EQ(1U, config_->stats().idle_timeout_.value());
}

class TcpProxyRoutingTest : public testing::Test {
public:
  TcpProxyRoutingTest() {
    std::string json = R"EOF(
    {
      "stat_prefix": "name",
      "route_config": {
        "routes": [
          {
            "destination_ports": "1-9999",
            "cluster": "fake_cluster"
          }
        ]
      }
    }
    )EOF";

    Json::ObjectSharedPtr config = Json::Factory::loadFromString(json);
    config_.reset(new TcpProxyConfig(constructTcpProxyConfigFromJson(*config, factory_context_)));
  }

  void setup() {
    EXPECT_CALL(filter_callbacks_, connection()).WillRepeatedly(ReturnRef(connection_));

    filter_.reset(new TcpProxy(config_, factory_context_.cluster_manager_));
    filter_->initializeReadFilterCallbacks(filter_callbacks_);
  }

  TcpProxyConfigSharedPtr config_;
  NiceMock<Network::MockConnection> connection_;
  NiceMock<Network::MockReadFilterCallbacks> filter_callbacks_;
  NiceMock<Server::Configuration::MockFactoryContext> factory_context_;
  std::unique_ptr<TcpProxy> filter_;
};

TEST_F(TcpProxyRoutingTest, NonRoutableConnection) {
  uint32_t total_cx = config_->stats().downstream_cx_total_.value();
  uint32_t non_routable_cx = config_->stats().downstream_cx_no_route_.value();

  setup();

  // Port 10000 is outside the specified destination port range.
  connection_.local_address_ = std::make_shared<Network::Address::Ipv4Instance>("1.2.3.4", 10000);

  // Expect filter to stop iteration and close connection.
  EXPECT_CALL(connection_, close(Network::ConnectionCloseType::NoFlush));
  EXPECT_EQ(Network::FilterStatus::StopIteration, filter_->onNewConnection());

  EXPECT_EQ(total_cx + 1, config_->stats().downstream_cx_total_.value());
  EXPECT_EQ(non_routable_cx + 1, config_->stats().downstream_cx_no_route_.value());
}

TEST_F(TcpProxyRoutingTest, RoutableConnection) {
  uint32_t total_cx = config_->stats().downstream_cx_total_.value();
  uint32_t non_routable_cx = config_->stats().downstream_cx_no_route_.value();

  setup();

  // Port 9999 is within the specified destination port range.
  connection_.local_address_ = std::make_shared<Network::Address::Ipv4Instance>("1.2.3.4", 9999);

  // Expect filter to try to open a connection to specified cluster.
  EXPECT_CALL(factory_context_.cluster_manager_, tcpConnForCluster_("fake_cluster", _));

  filter_->onNewConnection();

  EXPECT_EQ(total_cx + 1, config_->stats().downstream_cx_total_.value());
  EXPECT_EQ(non_routable_cx, config_->stats().downstream_cx_no_route_.value());
}

} // namespace Filter
} // namespace Envoy
