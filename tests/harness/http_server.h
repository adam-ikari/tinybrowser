#ifndef TB_TEST_HTTP_SERVER_H
#define TB_TEST_HTTP_SERVER_H
#include <atomic>
#include <map>
#include <string>
#include <thread>

// 极简单线程 accept 循环的测试 HTTP 服务器(std::thread + POSIX socket)。
class HttpServer {
public:
  // routes: path -> {status, content_type, body}
  struct Route { int status; std::string content_type; std::string body; };
  explicit HttpServer(const std::map<std::string, Route> &routes);
  ~HttpServer();
  HttpServer(const HttpServer &) = delete;
  HttpServer &operator=(const HttpServer &) = delete;

  int port() const { return port_; }
  std::string base() const;           // http://127.0.0.1:<port>
  int request_count() const { return requests_.load(); }

private:
  void run();
  static bool header_value(const std::string &req, const std::string &name,
                           std::string &out);
  static void respond(int fd, int status, const char *reason,
                      const std::string &content_type, const std::string &body,
                      const std::string &extra_headers);

  int fd_ = -1;
  int port_ = 0;
  std::map<std::string, Route> routes_;
  std::thread thread_;
  std::atomic<int> requests_{0};
  std::atomic<bool> stop_{false};
};
#endif
