#include "http_server.h"
#include <arpa/inet.h>
#include <cstring>
#include <poll.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

// 小工具:把内存安全地把字符串塞进动态缓冲。
static bool read_until_headers(int fd, std::string &buf) {
  char tmp[2048];
  for (;;) {
    ssize_t n = recv(fd, tmp, sizeof tmp, 0);
    if (n <= 0) return false;
    buf.append(tmp, (size_t)n);
    if (buf.find("\r\n\r\n") != std::string::npos) return true;
  }
}

HttpServer::HttpServer(const std::map<std::string, Route> &routes)
    : routes_(routes) {
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

HttpServer::~HttpServer() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
  if (fd_ >= 0) close(fd_);
}

std::string HttpServer::base() const {
  return "http://127.0.0.1:" + std::to_string(port_);
}

bool HttpServer::header_value(const std::string &req, const std::string &name,
                              std::string &out) {
  std::string needle = name + ":";
  size_t pos = 0;
  while ((pos = req.find(needle, pos)) != std::string::npos) {
    // 只认行首(前一行以 \r\n 结尾)
    if (pos == 0 || req.compare(pos - 2, 2, "\r\n") == 0) {
      pos += needle.size();
      while (pos < req.size() && (req[pos] == ' ' || req[pos] == '\t')) pos++;
      size_t end = req.find("\r\n", pos);
      if (end == std::string::npos) end = req.size();
      out = req.substr(pos, end - pos);
      return true;
    }
    pos += needle.size();
  }
  return false;
}

void HttpServer::respond(int fd, int status, const char *reason,
                         const std::string &content_type,
                         const std::string &body,
                         const std::string &extra_headers) {
  std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason +
                     "\r\nContent-Type: " + content_type +
                     "\r\nContent-Length: " + std::to_string(body.size()) +
                     "\r\nConnection: close\r\n" + extra_headers + "\r\n";
  std::string out = head + body;
  size_t off = 0;
  while (off < out.size()) {
    ssize_t n = send(fd, out.data() + off, out.size() - off, 0);
    if (n <= 0) break;
    off += (size_t)n;
  }
}

void HttpServer::run() {
  std::string req;
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
    if (c < 0) continue;
    requests_.fetch_add(1);
    req.clear();
    bool ok = read_until_headers(c, req);
    if (!ok) {
      close(c);
      continue;
    }
    // 解析 "GET <path> HTTP/1.1"
    std::string path;
    size_t sp1 = req.find(' ');
    size_t sp2 = sp1 == std::string::npos ? std::string::npos : req.find(' ', sp1 + 1);
    if (sp1 != std::string::npos && sp2 != std::string::npos)
      path = req.substr(sp1 + 1, sp2 - sp1 - 1);
    // 吞掉请求体(如有),避免干扰后续请求(其实 Connection: close 不必要,稳妥起见)
    std::string cl;
    if (header_value(req, "Content-Length", cl)) {
      size_t need = (size_t)strtoul(cl.c_str(), nullptr, 10);
      while (req.size() < need && read_until_headers(c, req)) {}
    }

    if (path == "/redirect") {
      respond(c, 302, "Found", "text/html", "", "Location: /target\r\n");
      close(c);
      continue;
    }
    // 回显请求的 Cookie 头当作正文。cookie 测试需要断言「浏览器实际发了什么」,
    // 而不只是「jar 里有什么」—— 这两者之间的连线只有真发一次请求才验证得到。
    if (path == "/echo-cookie") {
      std::string ck;
      header_value(req, "Cookie", ck);
      respond(c, 200, "OK", "text/plain", ck, "");
      close(c);
      continue;
    }
    auto it = routes_.find(path);
    if (it == routes_.end()) {
      respond(c, 404, "Not Found", "text/plain", "not found", "");
    } else {
      const Route &r = it->second;
      respond(c, r.status, r.status == 200 ? "OK" : "Status", r.content_type,
              r.body, r.extra_headers);
    }
    close(c);
  }
}
