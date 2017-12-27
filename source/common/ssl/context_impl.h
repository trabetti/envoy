#pragma once

#include <string>
#include <vector>

#include "envoy/runtime/runtime.h"
#include "envoy/ssl/context.h"
#include "envoy/ssl/context_config.h"
#include "envoy/stats/stats.h"
#include "envoy/stats/stats_macros.h"

#include "common/ssl/context_impl.h"
#include "common/ssl/context_manager_impl.h"

#include "openssl/ssl.h"

namespace Envoy {
#ifndef OPENSSL_IS_BORINGSSL
#error Envoy requires BoringSSL
#endif

namespace Ssl {

// clang-format off
#define ALL_SSL_STATS(COUNTER, GAUGE, HISTOGRAM)                                                   \
  COUNTER(connection_error)                                                                        \
  COUNTER(handshake)                                                                               \
  COUNTER(session_reused)                                                                          \
  COUNTER(no_certificate)                                                                          \
  COUNTER(fail_no_sni_match)                                                                       \
  COUNTER(fail_verify_no_cert)                                                                     \
  COUNTER(fail_verify_error)                                                                       \
  COUNTER(fail_verify_san)                                                                         \
  COUNTER(fail_verify_cert_hash)
// clang-format on

/**
 * Wrapper struct for SSL stats. @see stats_macros.h
 */
struct SslStats {
  ALL_SSL_STATS(GENERATE_COUNTER_STRUCT, GENERATE_GAUGE_STRUCT, GENERATE_HISTOGRAM_STRUCT)
};

class ContextImpl : public virtual Context {
public:
  virtual bssl::UniquePtr<SSL> newSsl() const;

  /**
   * Logs successful TLS handshake and updates stats.
   * @param ssl the connection to log
   */
  void logHandshake(SSL* ssl) const;

  /**
   * Performs subjectAltName verification
   * @param ssl the certificate to verify
   * @param subject_alt_names the configured subject_alt_names to match
   * @return true if the verification succeeds
   */
  static bool verifySubjectAltName(X509* cert, const std::vector<std::string>& subject_alt_names);

  /**
   * Determines whether the given name matches 'pattern' which may optionally begin with a wildcard.
   * NOTE:  public for testing
   * @param san the subjectAltName to match
   * @param pattern the pattern to match against (*.example.com)
   * @return true if the san matches pattern
   */
  static bool dNSNameMatch(const std::string& dnsName, const char* pattern);

  SslStats& stats() { return stats_; }

  // Ssl::Context
  size_t daysUntilFirstCertExpires() const override;
  std::string getCaCertInformation() const override;
  std::string getCertChainInformation() const override;

protected:
  ContextImpl(ContextManagerImpl& parent, Stats::Scope& scope, const ContextConfig& config);

  /**
   * The global SSL-library index used for storing a pointer to the context
   * in the SSL instance, for retrieval in callbacks.
   */
  static int sslContextIndex();

  static int verifyCallback(X509_STORE_CTX* store_ctx, void* arg);
  int verifyCertificate(X509* cert);

  /**
   * Verifies certificate hash for pinning. The hash is the SHA-256 has of the DER encoding of the
   * certificate.
   *
   * The hash can be computed using 'openssl x509 -noout -fingerprint -sha256 -in cert.pem'
   *
   * @param ssl the certificate to verify
   * @param certificate_hash the configured certificate hash to match
   * @return true if the verification succeeds
   */
  static bool verifyCertificateHash(X509* cert, const std::vector<uint8_t>& certificate_hash);

  std::vector<uint8_t> parseAlpnProtocols(const std::string& alpn_protocols);
  static SslStats generateStats(Stats::Scope& scope);
  int32_t getDaysUntilExpiration(const X509* cert) const;
  bssl::UniquePtr<X509> loadCert(const std::string& cert_file);
  static std::string getSerialNumber(const X509* cert);
  std::string getCaFileName() const { return ca_file_path_; };
  std::string getCertChainFileName() const { return cert_chain_file_path_; };

  ContextManagerImpl& parent_;
  bssl::UniquePtr<SSL_CTX> ctx_;
  std::vector<std::string> verify_subject_alt_name_list_;
  std::vector<uint8_t> verify_certificate_hash_;
  Stats::Scope& scope_;
  SslStats stats_;
  std::vector<uint8_t> parsed_alpn_protocols_;
  bssl::UniquePtr<X509> ca_cert_;
  bssl::UniquePtr<X509> cert_chain_;
  std::string ca_file_path_;
  std::string cert_chain_file_path_;
  const uint16_t min_protocol_version_;
  const uint16_t max_protocol_version_;
  const std::string ecdh_curves_;
};

class ClientContextImpl : public ContextImpl, public ClientContext {
public:
  ClientContextImpl(ContextManagerImpl& parent, Stats::Scope& scope,
                    const ClientContextConfig& config);
  ~ClientContextImpl() { parent_.releaseClientContext(this); }

  bssl::UniquePtr<SSL> newSsl() const override;

private:
  std::string server_name_indication_;
};

class ServerContextImpl : public ContextImpl, public ServerContext {
public:
  ServerContextImpl(ContextManagerImpl& parent, const std::string& listener_name,
                    const std::vector<std::string>& server_names, Stats::Scope& scope,
                    const ServerContextConfig& config, bool skip_context_update,
                    Runtime::Loader& runtime);
  ~ServerContextImpl() { parent_.releaseServerContext(this, listener_name_, server_names_); }

private:
  ssl_select_cert_result_t processClientHello(const SSL_CLIENT_HELLO* client_hello);
  void updateConnectionContext(SSL* ssl);

  int alpnSelectCallback(const unsigned char** out, unsigned char* outlen, const unsigned char* in,
                         unsigned int inlen);
  int sessionTicketProcess(SSL* ssl, uint8_t* key_name, uint8_t* iv, EVP_CIPHER_CTX* ctx,
                           HMAC_CTX* hmac_ctx, int encrypt);

  const std::string listener_name_;
  const std::vector<std::string> server_names_;
  const bool skip_context_update_;
  Runtime::Loader& runtime_;
  std::vector<uint8_t> parsed_alt_alpn_protocols_;
  const std::vector<ServerContextConfig::SessionTicketKey> session_ticket_keys_;
};

} // namespace Ssl
} // namespace Envoy
