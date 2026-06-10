#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <csignal>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Config {
  std::string host = "127.0.0.1";
  int port = 8080;
  std::string path = "/ws";
  std::string session = "smoke";
  std::string password = "changeme";
};

void usage(const char *argv0) {
  std::cerr << "Usage: " << argv0
            << " [--host 127.0.0.1] [--port 8080] [--path /ws] [--session smoke] [--password changeme]\n";
}

Config parse_args(int argc, char **argv) {
  Config cfg;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto need = [&](std::string &dst) {
      if (i + 1 >= argc) throw std::runtime_error("missing value for " + arg);
      dst = argv[++i];
    };
    if (arg == "--host") {
      need(cfg.host);
    } else if (arg == "--port") {
      if (i + 1 >= argc) throw std::runtime_error("missing value for --port");
      cfg.port = std::stoi(argv[++i]);
    } else if (arg == "--path") {
      need(cfg.path);
    } else if (arg == "--session") {
      need(cfg.session);
    } else if (arg == "--password") {
      need(cfg.password);
    } else if (arg == "-h" || arg == "--help") {
      usage(argv[0]);
      std::exit(0);
    } else {
      throw std::runtime_error("unknown arg: " + arg);
    }
  }
  return cfg;
}

void write_all(int fd, const uint8_t *buf, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = ::send(fd, buf + off, len - off, MSG_NOSIGNAL);
    if (n <= 0) throw std::runtime_error("send failed");
    off += static_cast<size_t>(n);
  }
}

void write_all_str(int fd, const std::string &s) {
  write_all(fd, reinterpret_cast<const uint8_t *>(s.data()), s.size());
}

std::vector<uint8_t> read_some(int fd, size_t nmax = 4096) {
  std::vector<uint8_t> out(nmax);
  ssize_t n = ::recv(fd, out.data(), out.size(), 0);
  if (n <= 0) throw std::runtime_error("recv failed");
  out.resize(static_cast<size_t>(n));
  return out;
}

int connect_tcp(const std::string &host, int port) {
  struct addrinfo hints {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo *res = nullptr;
  const std::string p = std::to_string(port);
  if (::getaddrinfo(host.c_str(), p.c_str(), &hints, &res) != 0) {
    throw std::runtime_error("getaddrinfo failed");
  }
  int fd = -1;
  for (auto *ai = res; ai; ai = ai->ai_next) {
    fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
    ::close(fd);
    fd = -1;
  }
  ::freeaddrinfo(res);
  if (fd < 0) throw std::runtime_error("connect failed");
  timeval tv {};
  tv.tv_sec = 3;
  tv.tv_usec = 0;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  return fd;
}

std::vector<uint8_t> make_masked_frame(const std::string &payload) {
  const uint64_t len = payload.size();
  std::vector<uint8_t> frame;
  frame.reserve(static_cast<size_t>(len) + 14);
  frame.push_back(0x81);  // FIN + text

  if (len <= 125) {
    frame.push_back(static_cast<uint8_t>(0x80 | len));
  } else if (len <= 65535) {
    frame.push_back(0x80 | 126);
    frame.push_back(static_cast<uint8_t>((len >> 8) & 0xff));
    frame.push_back(static_cast<uint8_t>(len & 0xff));
  } else {
    frame.push_back(0x80 | 127);
    for (int i = 7; i >= 0; --i) frame.push_back(static_cast<uint8_t>((len >> (8 * i)) & 0xff));
  }

  uint8_t mask[4];
  static std::mt19937 rng{123456};
  for (int i = 0; i < 4; ++i) mask[i] = static_cast<uint8_t>(rng() & 0xff);
  frame.insert(frame.end(), mask, mask + 4);

  for (size_t i = 0; i < payload.size(); ++i) {
    frame.push_back(static_cast<uint8_t>(payload[i]) ^ mask[i % 4]);
  }
  return frame;
}

std::string read_text_frame(int fd) {
  uint8_t hdr[2];
  ssize_t n = ::recv(fd, hdr, 2, MSG_WAITALL);
  if (n != 2) throw std::runtime_error("failed to read frame header");
  const bool masked = (hdr[1] & 0x80) != 0;
  uint64_t len = hdr[1] & 0x7f;
  if (len == 126) {
    uint8_t ext[2];
    if (::recv(fd, ext, 2, MSG_WAITALL) != 2) throw std::runtime_error("failed len16");
    len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
  } else if (len == 127) {
    uint8_t ext[8];
    if (::recv(fd, ext, 8, MSG_WAITALL) != 8) throw std::runtime_error("failed len64");
    len = 0;
    for (int i = 0; i < 8; ++i) len = (len << 8) | ext[i];
  }
  uint8_t mask[4] = {0, 0, 0, 0};
  if (masked) {
    if (::recv(fd, mask, 4, MSG_WAITALL) != 4) throw std::runtime_error("failed mask");
  }
  std::string payload(len, '\0');
  if (len > 0) {
    if (::recv(fd, payload.data(), len, MSG_WAITALL) != static_cast<ssize_t>(len))
      throw std::runtime_error("failed payload");
  }
  if (masked) {
    for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= mask[i % 4];
  }
  return payload;
}

bool contains(const std::string &hay, const std::string &needle) {
  return hay.find(needle) != std::string::npos;
}

}  // namespace

int main(int argc, char **argv) {
  try {
    std::signal(SIGPIPE, SIG_IGN);
    const Config cfg = parse_args(argc, argv);
    auto connect_and_join = [&]() -> int {
      const int fd = connect_tcp(cfg.host, cfg.port);
      const std::string req =
          "GET " + cfg.path + " HTTP/1.1\r\n"
          "Host: " + cfg.host + ":" + std::to_string(cfg.port) + "\r\n"
          "Upgrade: websocket\r\n"
          "Connection: Upgrade\r\n"
          "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
          "Sec-WebSocket-Version: 13\r\n\r\n";
      write_all_str(fd, req);

      std::string response;
      while (response.find("\r\n\r\n") == std::string::npos) {
        const auto chunk = read_some(fd);
        response.append(reinterpret_cast<const char *>(chunk.data()), chunk.size());
      }
      if (!contains(response, "101")) throw std::runtime_error("websocket handshake failed");

      auto send_json = [&](const std::string &json) {
        auto frame = make_masked_frame(json);
        write_all(fd, frame.data(), frame.size());
      };
      send_json("{\"type\":\"join\",\"session\":\"" + cfg.session + "\",\"password\":\"" + cfg.password + "\"}");
      std::string join_resp;
      for (int i = 0; i < 3; ++i) {
        join_resp = read_text_frame(fd);
        if (contains(join_resp, "\"type\":\"joined\"")) break;
      }
      if (!contains(join_resp, "\"type\":\"joined\"")) throw std::runtime_error("join failed: " + join_resp);
      return fd;
    };

    int fd = connect_and_join();

    auto send_json = [&](const std::string &json) {
      auto frame = make_masked_frame(json);
      write_all(fd, frame.data(), frame.size());
    };

    std::cout << "join_ok\n";

    const std::string huge_payload = "{\"type\":\"offer\",\"payload\":{\"sdp\":\"" + std::string(70 * 1024, 'x') + "\"}}";
    send_json(huge_payload);
    std::string large_resp;
    bool size_ok = false;
    try {
      large_resp = read_text_frame(fd);
      size_ok = contains(large_resp, "message too large");
    } catch (...) {
      size_ok = true;
    }
    if (!size_ok) throw std::runtime_error("expected size-limit handling");
    std::cout << "size_limit_ok\n";
    ::close(fd);
    fd = connect_and_join();
    auto send_json2 = [&](const std::string &json) {
      auto frame = make_masked_frame(json);
      write_all(fd, frame.data(), frame.size());
    };

    bool rate_hit = false;
    for (int i = 0; i < 100; ++i) {
      send_json2("{\"type\":\"offer\",\"payload\":{\"sdp\":\"x\"}}");
      const std::string r = read_text_frame(fd);
      if (contains(r, "rate limit")) {
        rate_hit = true;
        break;
      }
    }
    if (!rate_hit) throw std::runtime_error("expected rate limit was not triggered");
    std::cout << "rate_limit_ok\n";

    send_json("{\"type\":\"leave\"}");
    (void)read_text_frame(fd);
    ::close(fd);
    std::cout << "ws_smoke_pass\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "ws_smoke_fail: " << e.what() << '\n';
    return 1;
  }
}
