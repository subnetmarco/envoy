#include <memory>

#include "envoy/config/bootstrap/v3/bootstrap.pb.h"
#include "envoy/config/filter/network/tcp_proxy/v2/tcp_proxy.pb.h"

#include "test/integration/http_integration.h"
#include "test/integration/http_protocol_integration.h"

#include "gtest/gtest.h"

namespace Envoy {
namespace {

// Terminating CONNECT and sending raw TCP upstream.
class ConnectTerminationIntegrationTest
    : public testing::TestWithParam<Network::Address::IpVersion>,
      public HttpIntegrationTest {
public:
  ConnectTerminationIntegrationTest()
      : HttpIntegrationTest(Http::CodecClient::Type::HTTP2, GetParam()) {
    enable_half_close_ = true;
  }

  void initialize() override {
    auto host = config_helper_.createVirtualHost("host", "/");
    //    host.mutable_proxying_config();
    config_helper_.addVirtualHost(host);
    config_helper_.addConfigModifier(
        [&](envoy::extensions::filters::network::http_connection_manager::v3::HttpConnectionManager&
                hcm) -> void {
          hcm.add_upgrade_configs()->set_upgrade_type("CONNECT");
          hcm.mutable_http2_protocol_options()->set_allow_connect(true);

          if (enable_timeout_) {
            hcm.mutable_stream_idle_timeout()->set_seconds(0);
            hcm.mutable_stream_idle_timeout()->set_nanos(200 * 1000 * 1000);
          }
        });
    HttpIntegrationTest::initialize();
  }

  void setUpConnection() {
    codec_client_ = makeHttpConnection(lookupPort("http"));
    auto encoder_decoder = codec_client_->startRequest(connect_headers_);
    request_encoder_ = &encoder_decoder.first;
    response_ = std::move(encoder_decoder.second);
    ASSERT_TRUE(fake_upstreams_[0]->waitForRawConnection(fake_raw_upstream_connection_));
  }

  void sendBidirectionalData(const char* downstream_send_data = "hello",
                             const char* upstream_received_data = "hello",
                             const char* upstream_send_data = "there!",
                             const char* downstream_received_data = "there!") {
    // Send some data upstream.
    codec_client_->sendData(*request_encoder_, downstream_send_data, false);
    ASSERT_TRUE(fake_raw_upstream_connection_->waitForData(
        FakeRawConnection::waitForInexactMatch(upstream_received_data)));

    // Send some data downstream.
    ASSERT_TRUE(fake_raw_upstream_connection_->write(upstream_send_data));
    response_->waitForBodyData(strlen(downstream_received_data));
    EXPECT_EQ(downstream_received_data, response_->body());
  }

  Http::TestRequestHeaderMapImpl connect_headers_{{":method", "CONNECT"},
                                                  {":path", "/"},
                                                  {":protocol", "bytestream"},
                                                  {":scheme", "https"},
                                                  {":authority", "host"}};
  FakeRawConnectionPtr fake_raw_upstream_connection_;
  IntegrationStreamDecoderPtr response_;
  bool enable_timeout_{};
};

// TODO(alyssawilk) make sure that if data is sent with the connect it does not go upstream
// until the 200 headers are sent before unhiding ANY config.
TEST_P(ConnectTerminationIntegrationTest, DISABLED_Basic) {
  initialize();

  setUpConnection();
  sendBidirectionalData("hello", "hello", "there!", "there!");
  // Send a second set of data to make sure for example headers are only sent once.
  sendBidirectionalData(",bye", "hello,bye", "ack", "there!ack");

  // Send an end stream. This should result in half close upstream.
  codec_client_->sendData(*request_encoder_, "", true);
  ASSERT_TRUE(fake_raw_upstream_connection_->waitForHalfClose());

  // Now send a FIN from upstream. This should result in clean shutdown downstream.
  ASSERT_TRUE(fake_raw_upstream_connection_->close());
  response_->waitForEndStream();
  ASSERT_FALSE(response_->reset());
}

TEST_P(ConnectTerminationIntegrationTest, DISABLED_DownstreamClose) {
  initialize();

  setUpConnection();
  sendBidirectionalData();

  // Tear down by closing the client connection.
  codec_client_->close();
  ASSERT_TRUE(fake_raw_upstream_connection_->waitForHalfClose());
}

TEST_P(ConnectTerminationIntegrationTest, DISABLED_DownstreamReset) {
  initialize();

  setUpConnection();
  sendBidirectionalData();

  // Tear down by resetting the client stream.
  codec_client_->sendReset(*request_encoder_);
  ASSERT_TRUE(fake_raw_upstream_connection_->waitForHalfClose());
}

TEST_P(ConnectTerminationIntegrationTest, DISABLED_UpstreamClose) {
  initialize();

  setUpConnection();
  sendBidirectionalData();

  // Tear down by closing the upstream connection.
  ASSERT_TRUE(fake_raw_upstream_connection_->close());
  response_->waitForReset();
}

TEST_P(ConnectTerminationIntegrationTest, DISABLED_TestTimeout) {
  enable_timeout_ = true;
  initialize();

  setUpConnection();

  // Wait for the timeout to close the connection.
  response_->waitForEndStream();
  ASSERT_TRUE(fake_raw_upstream_connection_->waitForHalfClose());
}

TEST_P(ConnectTerminationIntegrationTest, DISABLED_BuggyHeaders) {
  initialize();
  // It's possible that the FIN is received before we set half close on the
  // upstream connection, so allow unexpected disconnects.
  fake_upstreams_[0]->set_allow_unexpected_disconnects(true);

  // Sending a header-only request is probably buggy, but rather than having a
  // special corner case it is treated as a regular half close.
  codec_client_ = makeHttpConnection(lookupPort("http"));
  response_ = codec_client_->makeHeaderOnlyRequest(
      Http::TestRequestHeaderMapImpl{{":method", "CONNECT"},
                                     {":path", "/"},
                                     {":protocol", "bytestream"},
                                     {":scheme", "https"},
                                     {":authority", "host"}});
  // If the connection is established (created, set to half close, and then the
  // FIN arrives), make sure the FIN arrives, and send a FIN from upstream.
  if (fake_upstreams_[0]->waitForRawConnection(fake_raw_upstream_connection_) &&
      fake_raw_upstream_connection_->connected()) {
    ASSERT_TRUE(fake_raw_upstream_connection_->waitForHalfClose());
    ASSERT_TRUE(fake_raw_upstream_connection_->close());
  }

  // Either with early close, or half close, the FIN from upstream should result
  // in clean stream teardown.
  response_->waitForEndStream();
  ASSERT_FALSE(response_->reset());
}

// For this class, forward the CONNECT request upstream
class ProxyingConnectIntegrationTest : public HttpProtocolIntegrationTest {
public:
  void initialize() override {
    config_helper_.addConfigModifier(
        [&](envoy::extensions::filters::network::http_connection_manager::v3::HttpConnectionManager&
                hcm) -> void {
          hcm.add_upgrade_configs()->set_upgrade_type("CONNECT");
          hcm.mutable_http2_protocol_options()->set_allow_connect(true);
        });
    HttpProtocolIntegrationTest::initialize();
  }

  Http::TestRequestHeaderMapImpl connect_headers_{{":method", "CONNECT"},
                                                  {":path", "/"},
                                                  {":protocol", "bytestream"},
                                                  {":scheme", "https"},
                                                  {":authority", "host:80"}};
  IntegrationStreamDecoderPtr response_;
};

INSTANTIATE_TEST_SUITE_P(Protocols, ProxyingConnectIntegrationTest,
                         testing::ValuesIn(HttpProtocolIntegrationTest::getProtocolTestParams()),
                         HttpProtocolIntegrationTest::protocolTestParamsToString);

TEST_P(ProxyingConnectIntegrationTest, ProxyConnect) {
  initialize();

  // Send request headers.
  codec_client_ = makeHttpConnection(lookupPort("http"));
  auto encoder_decoder = codec_client_->startRequest(connect_headers_);
  request_encoder_ = &encoder_decoder.first;
  response_ = std::move(encoder_decoder.second);

  // Wait for them to arrive upstream.
  AssertionResult result =
      fake_upstreams_[0]->waitForHttpConnection(*dispatcher_, fake_upstream_connection_);
  RELEASE_ASSERT(result, result.message());
  result = fake_upstream_connection_->waitForNewStream(*dispatcher_, upstream_request_);
  RELEASE_ASSERT(result, result.message());
  ASSERT_TRUE(upstream_request_->waitForHeadersComplete());
  EXPECT_EQ(upstream_request_->headers().get(Http::Headers::get().Method)->value(), "CONNECT");
  if (upstreamProtocol() == FakeHttpConnection::Type::HTTP1) {
    EXPECT_TRUE(upstream_request_->headers().get(Http::Headers::get().Protocol) == nullptr);
  } else {
    EXPECT_EQ(upstream_request_->headers().get(Http::Headers::get().Protocol)->value(),
              "bytestream");
  }

  // Send response headers
  upstream_request_->encodeHeaders(default_response_headers_, false);

  // Wait for them to arrive downstream.
  response_->waitForHeaders();
  EXPECT_EQ("200", response_->headers().Status()->value().getStringView());

  // Make sure that even once the response has started, that data can continue to go upstream.
  codec_client_->sendData(*request_encoder_, "hello", false);
  ASSERT_TRUE(upstream_request_->waitForData(*dispatcher_, 5));

  // Also test upstream to downstream data.
  upstream_request_->encodeData(12, false);
  response_->waitForBodyData(12);

  cleanupUpstreamAndDownstream();
}

INSTANTIATE_TEST_SUITE_P(IpVersions, ConnectTerminationIntegrationTest,
                         testing::ValuesIn(TestEnvironment::getIpVersionsForTest()),
                         TestUtility::ipTestParamsToString);

// Tunneling downstream TCP over an upstream HTTP channel.
class TcpTunnelingIntegrationTest : public testing::TestWithParam<Network::Address::IpVersion>,
                                    public HttpIntegrationTest {
public:
  TcpTunnelingIntegrationTest() : HttpIntegrationTest(Http::CodecClient::Type::HTTP2, GetParam()) {}

  void SetUp() override {
    enable_half_close_ = true;
    setDownstreamProtocol(Http::CodecClient::Type::HTTP2);
    setUpstreamProtocol(FakeHttpConnection::Type::HTTP2);

    config_helper_.addConfigModifier(
        [&](envoy::config::bootstrap::v3::Bootstrap& bootstrap) -> void {
          envoy::config::filter::network::tcp_proxy::v2::TcpProxy proxy_config;
          proxy_config.set_stat_prefix("tcp_stats");
          proxy_config.set_cluster("cluster_0");
          proxy_config.mutable_tunneling_config()->set_hostname("host.com");

          auto* listener = bootstrap.mutable_static_resources()->add_listeners();
          listener->set_name("tcp_proxy");
          auto* socket_address = listener->mutable_address()->mutable_socket_address();
          socket_address->set_address(Network::Test::getLoopbackAddressString(GetParam()));
          socket_address->set_port_value(0);

          auto* filter_chain = listener->add_filter_chains();
          auto* filter = filter_chain->add_filters();
          filter->mutable_typed_config()->PackFrom(proxy_config);
          filter->set_name("envoy.filters.network.tcp_proxy");
        });
  }
};

TEST_P(TcpTunnelingIntegrationTest, Basic) {
  initialize();

  // Start a connection, and verify the upgrade headers are received upstream.
  IntegrationTcpClientPtr tcp_client = makeTcpConnection(lookupPort("tcp_proxy"));
  ASSERT_TRUE(fake_upstreams_[0]->waitForHttpConnection(*dispatcher_, fake_upstream_connection_));
  ASSERT_TRUE(fake_upstream_connection_->waitForNewStream(*dispatcher_, upstream_request_));
  ASSERT_TRUE(upstream_request_->waitForHeadersComplete());

  // Send upgrade headers downstream, fully establishing the connection.
  upstream_request_->encodeHeaders(default_response_headers_, false);

  // Send some data from downstream to upstream, and make sure it goes through.
  tcp_client->write("hello", false);
  ASSERT_TRUE(upstream_request_->waitForData(*dispatcher_, 5));

  // Send data from upstream to downstream.
  upstream_request_->encodeData(12, false);
  tcp_client->waitForData(12);

  // Now send more data and close the TCP client. This should be treated as half close, so the data
  // should go through.
  tcp_client->write("hello", false);
  tcp_client->close();
  ASSERT_TRUE(upstream_request_->waitForData(*dispatcher_, 5));
  ASSERT_TRUE(upstream_request_->waitForEndStream(*dispatcher_));

  // If the upstream now sends 'end stream' the connection is fully closed.
  upstream_request_->encodeData(0, true);
}

TEST_P(TcpTunnelingIntegrationTest, InvalidResponseHeaders) {
  initialize();

  // Start a connection, and verify the upgrade headers are received upstream.
  IntegrationTcpClientPtr tcp_client = makeTcpConnection(lookupPort("tcp_proxy"));
  ASSERT_TRUE(fake_upstreams_[0]->waitForHttpConnection(*dispatcher_, fake_upstream_connection_));
  ASSERT_TRUE(fake_upstream_connection_->waitForNewStream(*dispatcher_, upstream_request_));
  ASSERT_TRUE(upstream_request_->waitForHeadersComplete());

  // Send invalid response_ headers, and verify that the client disconnects and
  // upstream gets a stream reset.
  default_response_headers_.setStatus(enumToInt(Http::Code::ServiceUnavailable));
  upstream_request_->encodeHeaders(default_response_headers_, false);
  ASSERT_TRUE(upstream_request_->waitForReset());

  // The connection should be fully closed, but the client has no way of knowing
  // that. Ensure the FIN is read and clean up state.
  tcp_client->waitForHalfClose();
  tcp_client->close();
}

TEST_P(TcpTunnelingIntegrationTest, CloseUpstreamFirst) {
  initialize();

  // Establish a connection.
  IntegrationTcpClientPtr tcp_client = makeTcpConnection(lookupPort("tcp_proxy"));
  ASSERT_TRUE(fake_upstreams_[0]->waitForHttpConnection(*dispatcher_, fake_upstream_connection_));
  ASSERT_TRUE(fake_upstream_connection_->waitForNewStream(*dispatcher_, upstream_request_));
  ASSERT_TRUE(upstream_request_->waitForHeadersComplete());
  upstream_request_->encodeHeaders(default_response_headers_, false);

  // Send data in both directions.
  tcp_client->write("hello", false);
  ASSERT_TRUE(upstream_request_->waitForData(*dispatcher_, 5));

  // Send data from upstream to downstream with an end stream and make sure the data is received
  // before the connection is half-closed.
  upstream_request_->encodeData(12, true);
  tcp_client->waitForData(12);
  tcp_client->waitForHalfClose();

  // Attempt to send data upstream.
  // should go through.
  tcp_client->write("hello", false);
  ASSERT_TRUE(upstream_request_->waitForData(*dispatcher_, 5));

  tcp_client->write("hello", true);
  ASSERT_TRUE(upstream_request_->waitForData(*dispatcher_, 5));
  ASSERT_TRUE(upstream_request_->waitForEndStream(*dispatcher_));
}

TEST_P(TcpTunnelingIntegrationTest, ResetStreamTest) {
  enable_half_close_ = false;
  initialize();

  // Establish a connection.
  IntegrationTcpClientPtr tcp_client = makeTcpConnection(lookupPort("tcp_proxy"));
  ASSERT_TRUE(fake_upstreams_[0]->waitForHttpConnection(*dispatcher_, fake_upstream_connection_));
  ASSERT_TRUE(fake_upstream_connection_->waitForNewStream(*dispatcher_, upstream_request_));
  ASSERT_TRUE(upstream_request_->waitForHeadersComplete());
  upstream_request_->encodeHeaders(default_response_headers_, false);

  // Reset the stream.
  upstream_request_->encodeResetStream();
  tcp_client->waitForDisconnect(true);
}

TEST_P(TcpTunnelingIntegrationTest, TestIdletimeoutWithLargeOutstandingData) {
  enable_half_close_ = false;
  config_helper_.setBufferLimits(1024, 1024);
  config_helper_.addConfigModifier([&](envoy::config::bootstrap::v3::Bootstrap& bootstrap) -> void {
    auto* listener = bootstrap.mutable_static_resources()->mutable_listeners(1);
    auto* filter_chain = listener->mutable_filter_chains(0);
    auto* config_blob = filter_chain->mutable_filters(0)->mutable_typed_config();

    ASSERT_TRUE(
        config_blob->Is<API_NO_BOOST(envoy::config::filter::network::tcp_proxy::v2::TcpProxy)>());
    auto tcp_proxy_config = MessageUtil::anyConvert<API_NO_BOOST(
        envoy::config::filter::network::tcp_proxy::v2::TcpProxy)>(*config_blob);
    tcp_proxy_config.mutable_idle_timeout()->set_nanos(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::milliseconds(500))
            .count());
    config_blob->PackFrom(tcp_proxy_config);
  });

  initialize();

  IntegrationTcpClientPtr tcp_client = makeTcpConnection(lookupPort("tcp_proxy"));
  ASSERT_TRUE(fake_upstreams_[0]->waitForHttpConnection(*dispatcher_, fake_upstream_connection_));
  ASSERT_TRUE(fake_upstream_connection_->waitForNewStream(*dispatcher_, upstream_request_));
  ASSERT_TRUE(upstream_request_->waitForHeadersComplete());
  upstream_request_->encodeHeaders(default_response_headers_, false);

  std::string data(1024 * 16, 'a');
  tcp_client->write(data);
  upstream_request_->encodeData(data, false);

  tcp_client->waitForDisconnect(true);
  ASSERT_TRUE(upstream_request_->waitForReset());
}

// Test that a downstream flush works correctly (all data is flushed)
TEST_P(TcpTunnelingIntegrationTest, TcpProxyDownstreamFlush) {
  // Use a very large size to make sure it is larger than the kernel socket read buffer.
  const uint32_t size = 50 * 1024 * 1024;
  config_helper_.setBufferLimits(size / 4, size / 4);
  initialize();

  std::string data(size, 'a');
  IntegrationTcpClientPtr tcp_client = makeTcpConnection(lookupPort("tcp_proxy"));
  ASSERT_TRUE(fake_upstreams_[0]->waitForHttpConnection(*dispatcher_, fake_upstream_connection_));
  ASSERT_TRUE(fake_upstream_connection_->waitForNewStream(*dispatcher_, upstream_request_));
  ASSERT_TRUE(upstream_request_->waitForHeadersComplete());
  upstream_request_->encodeHeaders(default_response_headers_, false);

  tcp_client->readDisable(true);
  tcp_client->write("", true);

  // This ensures that readDisable(true) has been run on its thread
  // before tcp_client starts writing.
  ASSERT_TRUE(upstream_request_->waitForEndStream(*dispatcher_));

  upstream_request_->encodeData(data, true);

  test_server_->waitForCounterGe("cluster.cluster_0.upstream_flow_control_paused_reading_total", 1);
  tcp_client->readDisable(false);
  tcp_client->waitForData(data);
  tcp_client->waitForHalfClose();
}

// Test that an upstream flush works correctly (all data is flushed)
TEST_P(TcpTunnelingIntegrationTest, TcpProxyUpstreamFlush) {
  // Use a very large size to make sure it is larger than the kernel socket read buffer.
  const uint32_t size = 50 * 1024 * 1024;
  config_helper_.setBufferLimits(size, size);
  initialize();

  std::string data(size, 'a');
  IntegrationTcpClientPtr tcp_client = makeTcpConnection(lookupPort("tcp_proxy"));
  ASSERT_TRUE(fake_upstreams_[0]->waitForHttpConnection(*dispatcher_, fake_upstream_connection_));
  ASSERT_TRUE(fake_upstream_connection_->waitForNewStream(*dispatcher_, upstream_request_));
  ASSERT_TRUE(upstream_request_->waitForHeadersComplete());
  upstream_request_->encodeHeaders(default_response_headers_, false);
  upstream_request_->readDisable(true);
  upstream_request_->encodeData("", true);

  // This ensures that fake_upstream_connection->readDisable has been run on its thread
  // before tcp_client starts writing.
  tcp_client->waitForHalfClose();

  tcp_client->write(data, true);

  // Note that upstream_flush_active will *not* be incremented for the HTTP
  // tunneling case. The data is already written to the stream, so no drainer
  // is necessary.
  upstream_request_->readDisable(false);
  ASSERT_TRUE(upstream_request_->waitForData(*dispatcher_, size));
  ASSERT_TRUE(upstream_request_->waitForEndStream(*dispatcher_));
  tcp_client->waitForHalfClose();
}

INSTANTIATE_TEST_SUITE_P(IpVersions, TcpTunnelingIntegrationTest,
                         testing::ValuesIn(TestEnvironment::getIpVersionsForTest()),
                         TestUtility::ipTestParamsToString);

} // namespace
} // namespace Envoy
