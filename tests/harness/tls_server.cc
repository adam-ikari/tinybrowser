#include "tls_server.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

namespace {

int bio_send(void *ctx, const unsigned char *buf, size_t len) {
  int fd = *(int *)ctx;
  ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
  if (n < 0) return -1;
  return (int)n;
}

int bio_recv(void *ctx, unsigned char *buf, size_t len) {
  int fd = *(int *)ctx;
  ssize_t n = recv(fd, buf, len, 0);
  if (n == 0) return MBEDTLS_ERR_SSL_CONN_EOF;
  if (n < 0) return -1;
  return (int)n;
}

// 握手 → 读请求头(直到 \r\n\r\n)→ 回固定响应 → close_notify。
void serve_one(int fd, mbedtls_ssl_config *conf, const std::string &resp) {
  mbedtls_ssl_context ssl;
  mbedtls_ssl_init(&ssl);
  if (mbedtls_ssl_setup(&ssl, conf) != 0) { mbedtls_ssl_free(&ssl); return; }
  mbedtls_ssl_set_bio(&ssl, &fd, bio_send, bio_recv, NULL);

  int ret, iters = 0;
  do { ret = mbedtls_ssl_handshake(&ssl); } while (
      (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) && ++iters < 200);
  if (ret != 0) { mbedtls_ssl_free(&ssl); return; }

  std::string req;
  char buf[2048];
  for (;;) {
    ret = mbedtls_ssl_read(&ssl, (unsigned char *)buf, sizeof buf);
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
    if (ret <= 0) break;
    req.append(buf, (size_t)ret);
    if (req.find("\r\n\r\n") != std::string::npos) break;
  }

  size_t off = 0;
  while (off < resp.size()) {
    ret = mbedtls_ssl_write(&ssl, (const unsigned char *)resp.data() + off, resp.size() - off);
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
    if (ret <= 0) break;
    off += (size_t)ret;
  }
  mbedtls_ssl_close_notify(&ssl);
  mbedtls_ssl_free(&ssl);
}

}  // namespace

TlsServer::TlsServer(const std::string &cert_file, const std::string &key_file)
    : cert_(cert_file), key_(key_file) {
  fd_ = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;  // ephemeral
  bind(fd_, (struct sockaddr *)&addr, sizeof addr);
  socklen_t alen = sizeof addr;
  getsockname(fd_, (struct sockaddr *)&addr, &alen);
  port_ = ntohs(addr.sin_port);
  listen(fd_, 16);
  thread_ = std::thread([this] { run(); });
}

TlsServer::~TlsServer() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
  if (fd_ >= 0) close(fd_);
}

std::string TlsServer::base() const {
  return "https://localhost:" + std::to_string(port_);
}

void TlsServer::run() {
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&drbg);
  bool ok = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                  (const unsigned char *)"tb-tls", 6) == 0;
  if (!ok) { mbedtls_ctr_drbg_free(&drbg); mbedtls_entropy_free(&entropy); return; }

  mbedtls_x509_crt srvcert;
  mbedtls_pk_context pkey;
  mbedtls_x509_crt_init(&srvcert);
  mbedtls_pk_init(&pkey);
  ok = mbedtls_x509_crt_parse_file(&srvcert, cert_.c_str()) == 0 &&
       mbedtls_pk_parse_keyfile(&pkey, key_.c_str(), NULL, NULL, NULL) == 0;
  if (!ok) {
    mbedtls_pk_free(&pkey);
    mbedtls_x509_crt_free(&srvcert);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    return;
  }

  mbedtls_ssl_config conf;
  mbedtls_ssl_config_init(&conf);
  mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER,
                              MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
  mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
  mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
  mbedtls_ssl_conf_own_cert(&conf, &srvcert, &pkey);

  std::string body = "<title>TLS OK</title><p>secure</p>";
  std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                     "Content-Length: " + std::to_string(body.size()) +
                     "\r\nConnection: close\r\n\r\n" + body;

  struct pollfd pfd;
  pfd.fd = fd_;
  pfd.events = POLLIN;
  for (;;) {
    pfd.revents = 0;
    int pr = poll(&pfd, 1, 100);
    if (stop_) break;
    if (pr <= 0) continue;
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
    int c = accept(fd_, nullptr, nullptr);
    if (c < 0) { if (stop_) break; continue; }
    serve_one(c, &conf, resp);
    close(c);
  }

  mbedtls_ssl_config_free(&conf);
  mbedtls_pk_free(&pkey);
  mbedtls_x509_crt_free(&srvcert);
  mbedtls_ctr_drbg_free(&drbg);
  mbedtls_entropy_free(&entropy);
}
