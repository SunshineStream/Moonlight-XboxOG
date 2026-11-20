/**
 * @file tests/unit/network/socket_bio_test.cpp
 * @brief Verify socket BIO retry behavior, ownership, and a real TLS exchange.
 */
// test header include
#include "src/network/socket_bio.h"

// standard includes
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <string>

// lib includes
#include <lizardbyte/common/testing.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>

// local includes
#include "src/network/host_pairing.h"

namespace {

  /** @brief Deterministic socket behavior used to exercise BIO callbacks. */
  struct FakeSocket final: network::SocketBioTransport {
    int result = -1;  ///< Next read or write result.
    bool retry = false;  ///< Whether the last error can be retried.
    std::string sent;  ///< Last bytes passed to the send callback.

    /** @copydoc network::SocketBioTransport::read_bytes */
    int read_bytes(char *buffer, int size) override {
      if (result > 0) {
        std::memset(buffer, 'a', std::min(size, result));
      }
      return result;
    }

    /** @copydoc network::SocketBioTransport::write_bytes */
    int write_bytes(const char *buffer, int size) override {
      sent.assign(buffer, size);
      return result;
    }

    /** @copydoc network::SocketBioTransport::retryable */
    bool retryable() const override {
      return retry;
    }
  };

  /** @brief Use one end of an in-memory BIO pair as a socket transport. */
  class MemoryBioTransport final: public network::SocketBioTransport {
  public:
    /**
     * @brief Take ownership of the memory BIO endpoint.
     * @param endpoint One endpoint of a connected BIO pair.
     */
    explicit MemoryBioTransport(BIO *endpoint):
        endpoint_(endpoint, &BIO_free) {}

    /** @copydoc network::SocketBioTransport::read_bytes */
    int read_bytes(char *buffer, int size) override {
      return BIO_read(endpoint_.get(), buffer, size);
    }

    /** @copydoc network::SocketBioTransport::write_bytes */
    int write_bytes(const char *buffer, int size) override {
      return BIO_write(endpoint_.get(), buffer, size);
    }

    /** @copydoc network::SocketBioTransport::retryable */
    bool retryable() const override {
      return BIO_should_retry(endpoint_.get()) != 0;
    }

  private:
    std::unique_ptr<BIO, decltype(&BIO_free)> endpoint_;  ///< Owned in-memory endpoint.
  };

  /** @brief Report the active retry direction and clear it after successful I/O. */
  TEST_F(BaseTest, SocketBioReportsRetryDirectionAndClearsItAfterSuccess) {
    FakeSocket socket;
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(network::make_socket_bio(socket), &BIO_free);
    ASSERT_NE(bio, nullptr);
    std::array<char, 8> buffer {};
    socket.retry = true;
    EXPECT_EQ(BIO_read(bio.get(), buffer.data(), buffer.size()), -1);
    EXPECT_TRUE(BIO_should_read(bio.get()));
    EXPECT_EQ(BIO_write(bio.get(), "hello", 5), -1);
    EXPECT_TRUE(BIO_should_write(bio.get()));
    EXPECT_FALSE(BIO_should_read(bio.get()));
    socket.result = 3;
    EXPECT_EQ(BIO_read(bio.get(), buffer.data(), buffer.size()), 3);
    EXPECT_EQ(std::string(buffer.data(), 3), "aaa");
    EXPECT_FALSE(BIO_should_retry(bio.get()));
    EXPECT_EQ(BIO_write(bio.get(), "hello", 5), 3);
    EXPECT_EQ(socket.sent, "hello");
    EXPECT_FALSE(BIO_should_retry(bio.get()));
  }

  /** @brief Distinguish fatal I/O errors from EOF and leave ownership with the caller. */
  TEST_F(BaseTest, SocketBioDistinguishesFatalErrorsFromEofAndRetainsCallerOwnership) {
    FakeSocket socket;
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(network::make_socket_bio(socket), &BIO_free);
    ASSERT_NE(bio, nullptr);
    char byte;
    EXPECT_EQ(BIO_read(bio.get(), &byte, 1), -1);
    EXPECT_FALSE(BIO_should_retry(bio.get()));
    EXPECT_FALSE(BIO_eof(bio.get()));
    socket.result = 0;
    EXPECT_EQ(BIO_read(bio.get(), &byte, 1), 0);
    EXPECT_TRUE(BIO_eof(bio.get()));
    EXPECT_EQ(BIO_flush(bio.get()), 1);
    EXPECT_EQ(BIO_set_close(bio.get(), BIO_CLOSE), 0);
    EXPECT_EQ(BIO_get_close(bio.get()), BIO_NOCLOSE);
    bio.reset();
    socket.sent = "still owned by caller";
    EXPECT_EQ(socket.sent, "still owned by caller");
  }

  /** @brief Transfer application data through TLS and release the wrapper exactly once. */
  TEST_F(BaseTest, SocketBioCompletesTlsHandshakeAndTransfersEncryptedData) {
    const auto identity = network::create_pairing_identity();
    ASSERT_FALSE(identity.certificatePem.empty());
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> serverContext(SSL_CTX_new(TLS_server_method()), &SSL_CTX_free);
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> clientContext(SSL_CTX_new(TLS_client_method()), &SSL_CTX_free);
    ASSERT_NE(serverContext, nullptr);
    ASSERT_NE(clientContext, nullptr);
    std::unique_ptr<BIO, decltype(&BIO_free)> certInput(BIO_new_mem_buf(identity.certificatePem.data(), static_cast<int>(identity.certificatePem.size())), &BIO_free);
    std::unique_ptr<BIO, decltype(&BIO_free)> keyInput(BIO_new_mem_buf(identity.privateKeyPem.data(), static_cast<int>(identity.privateKeyPem.size())), &BIO_free);
    std::unique_ptr<X509, decltype(&X509_free)> certificate(PEM_read_bio_X509(certInput.get(), nullptr, nullptr, nullptr), &X509_free);
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(PEM_read_bio_PrivateKey(keyInput.get(), nullptr, nullptr, nullptr), &EVP_PKEY_free);
    ASSERT_NE(certificate, nullptr);
    ASSERT_NE(key, nullptr);
    ASSERT_EQ(SSL_CTX_use_certificate(serverContext.get(), certificate.get()), 1);
    ASSERT_EQ(SSL_CTX_use_PrivateKey(serverContext.get(), key.get()), 1);
    BIO *clientEnd = nullptr;
    BIO *serverEnd = nullptr;
    ASSERT_EQ(BIO_new_bio_pair(&clientEnd, 0, &serverEnd, 0), 1);
    MemoryBioTransport transport(clientEnd);
    std::unique_ptr<BIO, decltype(&BIO_free)> serverBio(serverEnd, &BIO_free);
    int freed = 0;
    std::unique_ptr<SSL, decltype(&SSL_free)> server(SSL_new(serverContext.get()), &SSL_free);
    std::unique_ptr<SSL, decltype(&SSL_free)> client(SSL_new(clientContext.get()), &SSL_free);
    ASSERT_NE(server, nullptr);
    ASSERT_NE(client, nullptr);
    std::unique_ptr<BIO, decltype(&BIO_free)> clientBio(network::make_socket_bio(transport), &BIO_free);
    ASSERT_NE(clientBio, nullptr);
    const int freeCounterIndex = BIO_get_ex_new_index(0, nullptr, nullptr, nullptr, [](auto *, auto *data, auto *, int, long, auto *) {
      if (auto *counter = static_cast<int *>(data); counter != nullptr) {
        ++*counter;
      }
    });
    ASSERT_GE(freeCounterIndex, 0);
    ASSERT_EQ(BIO_set_ex_data(clientBio.get(), freeCounterIndex, &freed), 1);
    SSL_set_bio(client.get(), clientBio.get(), clientBio.get());
    clientBio.release();
    SSL_set_bio(server.get(), serverEnd, serverEnd);
    serverBio.release();
    SSL_set_connect_state(client.get());
    SSL_set_accept_state(server.get());
    for (int step = 0; step < 32 && (!SSL_is_init_finished(client.get()) || !SSL_is_init_finished(server.get())); ++step) {
      for (SSL *peer : {client.get(), server.get()}) {
        const int result = SSL_do_handshake(peer);
        if (result != 1) {
          const int error = SSL_get_error(peer, result);
          ASSERT_TRUE(error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) << error;
        }
      }
    }
    ASSERT_TRUE(SSL_is_init_finished(client.get()));
    ASSERT_TRUE(SSL_is_init_finished(server.get()));
    ASSERT_EQ(SSL_write(client.get(), "hello", 5), 5);
    std::array<char, 8> received {};
    ASSERT_EQ(SSL_read(server.get(), received.data(), received.size()), 5);
    EXPECT_EQ(std::string(received.data(), 5), "hello");
    client.reset();
    EXPECT_EQ(freed, 1);
    ASSERT_EQ(SSL_write(server.get(), "ok", 2), 2);
    EXPECT_GT(transport.read_bytes(received.data(), received.size()), 0);
  }
}  // namespace
