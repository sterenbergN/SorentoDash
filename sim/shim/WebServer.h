// ESP32 WebServer stand-in: a small blocking-free HTTP/1.0 server on
// 127.0.0.1:8080, served from handleClient() on the sketch's own loop (same
// threading as the real board). Firmware uploads are not supported here.
#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "Arduino.h"

enum HTTPMethod { HTTP_ANY, HTTP_GET, HTTP_POST };
enum HTTPUploadStatus { UPLOAD_FILE_START, UPLOAD_FILE_WRITE, UPLOAD_FILE_END, UPLOAD_FILE_ABORTED };
struct HTTPUpload {
  HTTPUploadStatus status = UPLOAD_FILE_ABORTED;
  size_t totalSize = 0, currentSize = 0;
  uint8_t buf[1];
};

class WebServer {
public:
  static constexpr int PORT = 8080;
  explicit WebServer(int = 80) {}

  void on(const char *path, HTTPMethod m, std::function<void()> fn) { routes_.push_back({path, m, fn, nullptr}); }
  void on(const char *path, HTTPMethod m, std::function<void()> fn, std::function<void()> upload) {
    routes_.push_back({path, m, fn, upload});
  }
  void onNotFound(std::function<void()> fn) { notFound_ = fn; }

  void begin() {
    static bool wsa = false;
    if (!wsa) {
      WSADATA d;
      WSAStartup(MAKEWORD(2, 2), &d);
      wsa = true;
    }
    if (listen_ != INVALID_SOCKET) return;
    listen_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL yes = TRUE;
    setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listen_, (sockaddr *)&a, sizeof(a)) != 0 || ::listen(listen_, 8) != 0) {
      printf("[sim] web server: port %d busy\n", PORT);
      closesocket(listen_);
      listen_ = INVALID_SOCKET;
      return;
    }
    u_long nb = 1;
    ioctlsocket(listen_, FIONBIO, &nb);
    printf("[sim] phone page: http://localhost:%d\n", PORT);
  }

  void stop() {
    if (listen_ != INVALID_SOCKET) closesocket(listen_);
    listen_ = INVALID_SOCKET;
  }

  void handleClient() {
    if (listen_ == INVALID_SOCKET) return;
    SOCKET c = accept(listen_, nullptr, nullptr);
    if (c == INVALID_SOCKET) return;
    u_long nb = 0;
    ioctlsocket(c, FIONBIO, &nb);
    DWORD tmo = 2000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));
    client_ = c;
    serve();
    closesocket(c);
    client_ = INVALID_SOCKET;
  }

  String arg(const char *name) { return std::string(name) == "plain" ? String(body_) : String(); }
  size_t clientContentLength() { return body_.size(); }
  HTTPUpload &upload() { return upload_; }

  void sendHeader(const char *k, const char *v, bool = false) { extra_ += std::string(k) + ": " + v + "\r\n"; }
  void send(int code, const char *type, const String &body) { reply(code, type, body.c_str(), body.length()); }
  void send(int code, const char *type, const char *body) { reply(code, type, body, strlen(body)); }
  void send_P(int code, const char *type, const char *body) { reply(code, type, body, strlen(body)); }

private:
  struct Route {
    std::string path;
    HTTPMethod method;
    std::function<void()> fn, upload;
  };
  std::vector<Route> routes_;
  std::function<void()> notFound_;
  SOCKET listen_ = INVALID_SOCKET, client_ = INVALID_SOCKET;
  std::string body_, extra_;
  HTTPUpload upload_;
  bool replied_ = false;

  void serve() {
    std::string req;
    char buf[4096];
    size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
      int n = recv(client_, buf, sizeof(buf), 0);
      if (n <= 0) return;
      req.append(buf, n);
      headerEnd = req.find("\r\n\r\n");
    }
    size_t len = 0;
    std::string lower = req.substr(0, headerEnd);
    for (auto &ch : lower) ch = (char)tolower((unsigned char)ch);
    size_t cl = lower.find("content-length:");
    if (cl != std::string::npos) len = strtoul(lower.c_str() + cl + 15, nullptr, 10);
    while (req.size() < headerEnd + 4 + len) {
      int n = recv(client_, buf, sizeof(buf), 0);
      if (n <= 0) break;
      req.append(buf, n);
    }
    body_ = req.substr(headerEnd + 4, len);
    size_t sp1 = req.find(' '), sp2 = req.find(' ', sp1 + 1);
    std::string method = req.substr(0, sp1), path = req.substr(sp1 + 1, sp2 - sp1 - 1);
    path = path.substr(0, path.find('?'));
    HTTPMethod m = method == "POST" ? HTTP_POST : HTTP_GET;
    extra_.clear();
    replied_ = false;
    sim::noteWebRequest();
    for (auto &r : routes_) {
      if (r.path == path && (r.method == HTTP_ANY || r.method == m)) {
        if (r.upload) {  // firmware upload: not possible in the simulator
          reply(400, "text/plain", "Firmware updates only work on the real display.", 47);
          return;
        }
        r.fn();
        if (!replied_) reply(200, "text/plain", "", 0);
        return;
      }
    }
    if (notFound_) notFound_();
    if (!replied_) reply(404, "text/plain", "not found", 9);
  }

  void reply(int code, const char *type, const char *body, size_t n) {
    if (replied_ || client_ == INVALID_SOCKET) return;
    replied_ = true;
    std::string h = "HTTP/1.0 " + std::to_string(code) + (code < 300 ? " OK" : code < 400 ? " Found" : " Error") +
                    "\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(n) +
                    "\r\nCache-Control: no-store\r\nConnection: close\r\n" + extra_ + "\r\n";
    ::send(client_, h.data(), (int)h.size(), 0);
    size_t sent = 0;
    while (sent < n) {
      int k = ::send(client_, body + sent, (int)std::min<size_t>(n - sent, 65536), 0);
      if (k <= 0) break;
      sent += k;
    }
  }
};
