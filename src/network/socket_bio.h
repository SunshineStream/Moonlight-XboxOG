/**
 * @file src/network/socket_bio.h
 * @brief Declare an OpenSSL BIO backed by caller-owned socket operations.
 */
#pragma once
#include <openssl/bio.h>

namespace network {

  /**
   * @brief Caller-owned socket operations that outlive the returned BIO.
   *
   * Method names avoid lwIP's POSIX compatibility macros such as `send`.
   */
  class SocketBioTransport {
  public:
    /** @brief Release the transport implementation. */
    virtual ~SocketBioTransport() = default;

    /**
     * @brief Read from the transport.
     * @param buffer Destination bytes.
     * @param size Available destination bytes.
     * @return Number of bytes read, zero at EOF, or a negative error.
     */
    virtual int read_bytes(char *buffer, int size) = 0;

    /**
     * @brief Write to the transport.
     * @param buffer Source bytes.
     * @param size Available source bytes.
     * @return Number of bytes written or a negative error.
     */
    virtual int write_bytes(const char *buffer, int size) = 0;

    /**
     * @brief Check whether the last transport error permits a retry.
     * @return True when the operation can be retried.
     */
    virtual bool retryable() const = 0;
  };

  /**
   * @brief Create a socket BIO without requiring OpenSSL's POSIX descriptor support.
   *
   * The caller retains ownership of the socket. BIO_free never closes it.
   * @param transport Caller-owned socket operations, valid until BIO_free.
   * @return Owned BIO, or nullptr when setup or allocation fails.
   */
  BIO *make_socket_bio(SocketBioTransport &transport);

}  // namespace network
