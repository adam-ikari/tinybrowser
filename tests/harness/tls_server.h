#ifndef TB_TEST_TLS_SERVER_H
#define TB_TEST_TLS_SERVER_H
#include <atomic>
#include <string>
#include <thread>

// 基于 mbedtls 的最小 HTTPS 服务器:单连接按需服务,回环。
// 每个请求固定返回 <title>TLS OK</title><p>secure</p>。
class TlsServer {
public:
  TlsServer(const std::string &cert_file, const std::string &key_file);
  ~TlsServer();
  TlsServer(const TlsServer &) = delete;
  TlsServer &operator=(const TlsServer &) = delete;

  int port() const { return port_; }
  std::string base() const;   // https://localhost:<port>

private:
  void run();

  int fd_ = -1;
  int port_ = 0;
  std::string cert_, key_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
};
#endif
