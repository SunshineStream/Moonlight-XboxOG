/**
 * @file tests/unit/network/host_quit_test.cpp
 * @brief Verifies authenticated cancellation, stale-target protection and host quit verification.
 */
// test header include
#include "src/network/host_pairing.h"

// standard includes
#include <atomic>
#include <string>

// lib includes
#include <lizardbyte/common/testing.h>

namespace {
  /**
   * @brief Own paired credentials and a scripted host without real network traffic.
   */
  class HostQuitTest: public BaseTest {  // NOSONAR(cpp:S3656) gtest fixture lifetime
  protected:
    network::PairingIdentity identity;  ///< Valid client certificate and unique ID.
    int running = 7;  ///< App currently reported by the host.
    bool paired = true;  ///< Pairing status reported in serverinfo.
    bool keepRunning = false;  ///< Simulate a host that accepts cancel but refuses to terminate.
    bool cancelTransportFailure = false;  ///< Fail the authenticated cancel request.
    bool serverTransportFailure = false;  ///< Fail serverinfo requests.
    bool failVerification = false;  ///< Fail only the post-cancel serverinfo query.
    int cancelStatus = 200;  ///< HTTP status for cancel responses.
    std::string cancelBody = "<root status_code=\"200\"><cancel>1</cancel></root>";  ///< Scripted protocol result.
    unsigned cancelRequests = 0;  ///< Number of actual cancel requests.

    /**
     * @brief Return sufficient valid host metadata for both plain and authenticated serverinfo queries.
     * @return Serverinfo XML with the current app and pairing state.
     */
    std::string server_info() const {
      return "<root status_code=\"200\"><hostname>Test PC</hostname><appversion>7.1.0.0</appversion><uniqueid>server-one</uniqueid><CurrentGame>" + std::to_string(running) + "</CurrentGame><ExternalPort>47989</ExternalPort><HttpsPort>47990</HttpsPort><PairStatus>" + (paired ? "1" : "0") + "</PairStatus></root>";
    }

    /**
     * @brief Install deterministic serverinfo and cancel responses.
     */
    void SetUp() override {
      BaseTest::SetUp();
      identity = network::create_pairing_identity();
      ASSERT_TRUE(network::is_valid_pairing_identity(identity));
      network::testing::set_host_pairing_http_test_handler([this](const auto &request, auto *response, std::string *error, const std::atomic<bool> *) {
        if (request.pathAndQuery.rfind("/cancel?", 0) == 0) {
          ++cancelRequests;
          EXPECT_TRUE(request.useTls);
          EXPECT_EQ(request.port, 47990U);
          EXPECT_EQ(request.tlsClientIdentity, &identity);
          EXPECT_NE(request.pathAndQuery.find("uniqueid=" + identity.uniqueId), std::string::npos);
          if (cancelTransportFailure) {
            if (error != nullptr) {
              *error = "Cancel request timed out";
            }
            return false;
          }
          response->statusCode = cancelStatus;
          response->body = cancelBody;
          if (!keepRunning && cancelStatus == 200 && cancelBody.find("status_code=\"200\"") != std::string::npos) {
            running = 0;
          }
          return true;
        }
        EXPECT_EQ(request.pathAndQuery.rfind("/serverinfo?", 0), 0U);
        if (serverTransportFailure || (failVerification && cancelRequests != 0)) {
          if (error != nullptr) {
            *error = "Serverinfo unavailable";
          }
          return false;
        }
        response->statusCode = 200;
        response->body = server_info();
        return true;
      });
    }

    /**
     * @brief Remove the scripted transport after every test.
     */
    void TearDown() override {
      network::testing::clear_host_pairing_http_test_handler();
      BaseTest::TearDown();
    }
  };

  /**
   * @brief Quit uses the paired HTTPS channel and verifies that the server reports an idle app slot.
   */
  TEST_F(HostQuitTest, CancelsAndVerifiesTheConfirmedApp) {
    std::string error;
    EXPECT_TRUE(network::quit_host_app("192.0.2.1", 47989, identity, 7, &error)) << error;
    EXPECT_EQ(cancelRequests, 1U);
    EXPECT_EQ(running, 0);
    EXPECT_TRUE(error.empty());
  }

  /**
   * @brief Already stopped and replaced sessions never receive a cancel for an unconfirmed target.
   */
  TEST_F(HostQuitTest, ProtectsAlreadyStoppedAndDifferentRunningApps) {
    std::string error;
    running = 0;
    EXPECT_TRUE(network::quit_host_app("192.0.2.1", 47989, identity, 7, &error));
    EXPECT_EQ(cancelRequests, 0U);
    running = 8;
    EXPECT_FALSE(network::quit_host_app("192.0.2.1", 47989, identity, 7, &error));
    EXPECT_NE(error.find("changed"), std::string::npos);
    EXPECT_EQ(cancelRequests, 0U);
  }

  /**
   * @brief Invalid targets, missing credentials and unpaired hosts are rejected before cancel.
   */
  TEST_F(HostQuitTest, RequiresAValidPairedTarget) {
    EXPECT_FALSE(network::quit_host_app({}, 47989, identity, 7));
    EXPECT_FALSE(network::quit_host_app("192.0.2.1", 47989, identity, 0));
    EXPECT_FALSE(network::quit_host_app("192.0.2.1", 47989, {}, 7));
    paired = false;
    EXPECT_FALSE(network::quit_host_app("192.0.2.1", 47989, identity, 7));
    EXPECT_EQ(cancelRequests, 0U);
  }

  /**
   * @brief Transport, HTTP and protocol rejections retain an actionable failure instead of reporting success.
   */
  TEST_F(HostQuitTest, ReportsTransportAndHostFailures) {
    std::string error;
    serverTransportFailure = true;
    EXPECT_FALSE(network::quit_host_app("192.0.2.1", 47989, identity, 7, &error));
    EXPECT_NE(error.find("Serverinfo"), std::string::npos);
    serverTransportFailure = false;
    cancelTransportFailure = true;
    EXPECT_FALSE(network::quit_host_app("192.0.2.1", 47989, identity, 7, &error));
    EXPECT_NE(error.find("timed out"), std::string::npos);
    cancelTransportFailure = false;
    cancelStatus = 403;
    EXPECT_FALSE(network::quit_host_app("192.0.2.1", 47989, identity, 7, &error));
    cancelStatus = 200;
    cancelBody = "<root status_code=\"503\" status_message=\"Another client owns the app\"/>";
    EXPECT_FALSE(network::quit_host_app("192.0.2.1", 47989, identity, 7, &error));
    EXPECT_NE(error.find("Another client"), std::string::npos);
    cancelBody = "malformed response";
    EXPECT_FALSE(network::quit_host_app("192.0.2.1", 47989, identity, 7, &error));
  }

  /**
   * @brief A successful cancel response alone cannot clear the local in-progress state.
   */
  TEST_F(HostQuitTest, VerifiesTheHostActuallyClosedTheApp) {
    std::string error;
    keepRunning = true;
    EXPECT_FALSE(network::quit_host_app("192.0.2.1", 47989, identity, 7, &error));
    EXPECT_NE(error.find("still running"), std::string::npos);
    failVerification = true;
    EXPECT_FALSE(network::quit_host_app("192.0.2.1", 47989, identity, 7, &error));
    EXPECT_NE(error.find("Serverinfo"), std::string::npos);
  }
}  // namespace
