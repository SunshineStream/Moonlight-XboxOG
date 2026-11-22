/**
 * @file src/_nxdk_compat/openssl_compat.h
 * @brief Adapt OpenSSL's single-process assumptions to the Xbox runtime.
 *
 * POSIX descriptor I/O is disabled by Configure; pairing uses a custom socket BIO.
 */
#pragma once

/** @brief Request Annex K time conversion declarations from PDCLib. */
#define __STDC_WANT_LIB_EXT1__ 1
/** @brief Request the nxdk random-source declaration from PDCLib. */
#define _CRT_RAND_S
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>

/**
 * @brief Identify the sole Xbox application for OpenSSL's fork detection.
 * @return Stable process identifier; Xbox applications cannot fork.
 */
static inline int moonlight_openssl_getpid(void) {
  return 1;
}

/**
 * @brief Identify the single privileged Xbox security identity.
 * @return Zero; distinct real and effective users or groups do not exist.
 */
static inline unsigned int moonlight_openssl_identity(void) {
  return 0;
}

/**
 * @brief Adapt PDCLib's Annex K gmtime_s argument order for OpenSSL.
 * @param result Destination broken-down timestamp.
 * @param timer Source Unix timestamp.
 * @return Zero on success, or the PDCLib error code.
 */
static inline int moonlight_openssl_gmtime_s(struct tm *result, const time_t *timer) {
  return gmtime_s(timer, result);
}

/**
 * @brief Fill an OpenSSL seed buffer from nxdk's platform random source.
 * @param buffer Destination byte buffer.
 * @param size Number of bytes requested by OpenSSL.
 * @return Zero on success, or -1 with errno set if nxdk cannot supply a word.
 */
static inline int moonlight_openssl_getentropy(void *buffer, size_t size) {
  unsigned char *output = buffer;
  while (size != 0) {
    unsigned int word;
    if (rand_s(&word) != 0) {
      errno = EIO;
      return -1;
    }
    const size_t chunk = size < sizeof(word) ? size : sizeof(word);
    memcpy(output, &word, chunk);
    output += chunk;
    size -= chunk;
  }
  return 0;
}

/** @brief Describe the sole application to OpenSSL's fork detection. */
#define getpid moonlight_openssl_getpid
/** @brief Describe the real user identity to OpenSSL. */
#define getuid moonlight_openssl_identity
/** @brief Describe the effective user identity to OpenSSL. */
#define geteuid moonlight_openssl_identity
/** @brief Describe the real group identity to OpenSSL. */
#define getgid moonlight_openssl_identity
/** @brief Describe the effective group identity to OpenSSL. */
#define getegid moonlight_openssl_identity
/** @brief Use PDCLib's Annex K time conversion. */
#define gmtime_s moonlight_openssl_gmtime_s
/** @brief Use PDCLib's timespec-based sleep in OpenSSL's POSIX timing code. */
#define nanosleep thrd_sleep

/** @brief Use nxdk's narrow-character semaphore API in OpenSSL's Win32 thread backend. */
#define CreateSemaphoreA CreateSemaphore
/** @brief Publish thread initialization before other Xbox threads observe completion. */
#define MemoryBarrier() __atomic_thread_fence(__ATOMIC_SEQ_CST)
/** @brief Provide the Win32 add-and-fetch operation on the Xbox CPU. */
#define InterlockedAdd64(target, value) __atomic_add_fetch((target), (value), __ATOMIC_SEQ_CST)
/** @brief Provide the Win32 fetch-and-AND operation on the Xbox CPU. */
#define InterlockedAnd64(target, value) __atomic_fetch_and((target), (value), __ATOMIC_SEQ_CST)
/** @brief Provide the Win32 64-bit fetch-and-OR operation on the Xbox CPU. */
#define InterlockedOr64(target, value) __atomic_fetch_or((target), (value), __ATOMIC_SEQ_CST)
/** @brief Provide the Win32 32-bit fetch-and-OR operation on the Xbox CPU. */
#define InterlockedOr(target, value) __atomic_fetch_or((target), (value), __ATOMIC_SEQ_CST)
