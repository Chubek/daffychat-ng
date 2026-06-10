#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <string>
#include <chrono>
#include <unordered_map>
#include <vector>

#include "nlohmann/json.hpp"
#include "App.h"

namespace {

using json = nlohmann::json;

struct Config {
  std::string host = "0.0.0.0";
  int port = 8080;
  std::string static_dir = ".";
  std::string password = "changeme";
  std::string stun_url = "stun:stun.l.google.com:19302";
  std::string turn_url = "";
  std::string turn_username = "";
  std::string turn_password = "";
};

struct PeerState {
  std::string peer_id;
  uWS::WebSocket<false, true, struct WsUserData> *ws = nullptr;
  std::chrono::steady_clock::time_point connected_at = std::chrono::steady_clock::now();
};

struct SessionState {
  std::optional<PeerState> peer_a;
  std::optional<PeerState> peer_b;
};

struct WsUserData {
  std::string session_id;
  std::string peer_id;
  bool joined = false;
  std::chrono::steady_clock::time_point rate_window_start = std::chrono::steady_clock::now();
  unsigned int messages_in_window = 0;
};

Config g_cfg;
std::unordered_map<std::string, SessionState> g_sessions;
us_listen_socket_t *g_listen_socket = nullptr;

constexpr size_t kMaxWsMessageBytes = 64 * 1024;
constexpr unsigned int kMaxMessagesPerSecond = 60;
constexpr auto kPeerSessionMaxAge = std::chrono::minutes(30);

void signal_handler(int) {
  if (g_listen_socket) {
    us_listen_socket_close(0, g_listen_socket);
    g_listen_socket = nullptr;
  }
}

void log_line(const std::string &msg) { std::cerr << "[daffychat] " << msg << '\n'; }

void print_usage(const char *argv0) {
  std::cerr << "Usage: " << argv0
            << " [--host <address>] [--port <port>] [--static-dir <path>] [--password <shared-secret>]\n"
            << "       [--stun-url <url>] [--turn-url <url>] [--turn-username <name>] [--turn-password <secret>]\n";
}

std::optional<Config> parse_args(int argc, char **argv) {
  Config cfg;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--host") {
      if (i + 1 >= argc) return std::nullopt;
      cfg.host = argv[++i];
      continue;
    }
    if (arg == "--port") {
      if (i + 1 >= argc) return std::nullopt;
      try {
        cfg.port = std::stoi(argv[++i]);
      } catch (...) {
        return std::nullopt;
      }
      if (cfg.port < 1 || cfg.port > 65535) return std::nullopt;
      continue;
    }
    if (arg == "--static-dir") {
      if (i + 1 >= argc) return std::nullopt;
      cfg.static_dir = argv[++i];
      continue;
    }
    if (arg == "--password") {
      if (i + 1 >= argc) return std::nullopt;
      cfg.password = argv[++i];
      if (cfg.password.empty()) return std::nullopt;
      continue;
    }
    if (arg == "--stun-url") {
      if (i + 1 >= argc) return std::nullopt;
      cfg.stun_url = argv[++i];
      continue;
    }
    if (arg == "--turn-url") {
      if (i + 1 >= argc) return std::nullopt;
      cfg.turn_url = argv[++i];
      continue;
    }
    if (arg == "--turn-username") {
      if (i + 1 >= argc) return std::nullopt;
      cfg.turn_username = argv[++i];
      continue;
    }
    if (arg == "--turn-password") {
      if (i + 1 >= argc) return std::nullopt;
      cfg.turn_password = argv[++i];
      continue;
    }
    if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      return std::nullopt;
    }
    return std::nullopt;
  }
  return cfg;
}

bool read_file_to_string(const std::filesystem::path &path, std::string &out) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return false;
  out.assign((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  return true;
}

bool valid_token(const std::string &value) {
  if (value.empty() || value.size() > 64) return false;
  for (char c : value) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

std::string random_id() {
  static std::mt19937_64 rng{std::random_device{}()};
  static constexpr char kHex[] = "0123456789abcdef";
  std::string id(16, '0');
  for (char &c : id) c = kHex[rng() % 16];
  return id;
}

void ws_send_json(uWS::WebSocket<false, true, WsUserData> *ws, const json &obj) {
  ws->send(obj.dump(), uWS::OpCode::TEXT);
}

PeerState *find_peer(SessionState &session, const std::string &peer_id) {
  if (session.peer_a.has_value() && session.peer_a->peer_id == peer_id) return &(*session.peer_a);
  if (session.peer_b.has_value() && session.peer_b->peer_id == peer_id) return &(*session.peer_b);
  return nullptr;
}

PeerState *find_other_peer(SessionState &session, const std::string &peer_id) {
  if (session.peer_a.has_value() && session.peer_a->peer_id != peer_id) return &(*session.peer_a);
  if (session.peer_b.has_value() && session.peer_b->peer_id != peer_id) return &(*session.peer_b);
  return nullptr;
}

void remove_peer(SessionState &session, const std::string &peer_id) {
  if (session.peer_a.has_value() && session.peer_a->peer_id == peer_id) session.peer_a.reset();
  if (session.peer_b.has_value() && session.peer_b->peer_id == peer_id) session.peer_b.reset();
}

void cleanup_if_empty(const std::string &session_id) {
  auto it = g_sessions.find(session_id);
  if (it == g_sessions.end()) return;
  const bool empty = !it->second.peer_a.has_value() && !it->second.peer_b.has_value();
  if (empty) g_sessions.erase(it);
}

void prune_expired_sessions() {
  const auto now = std::chrono::steady_clock::now();
  std::vector<std::string> to_erase;
  to_erase.reserve(g_sessions.size());
  for (auto &kv : g_sessions) {
    auto &s = kv.second;
    if (s.peer_a && (now - s.peer_a->connected_at) > kPeerSessionMaxAge) s.peer_a.reset();
    if (s.peer_b && (now - s.peer_b->connected_at) > kPeerSessionMaxAge) s.peer_b.reset();
    if (!s.peer_a && !s.peer_b) to_erase.push_back(kv.first);
  }
  for (const auto &id : to_erase) g_sessions.erase(id);
}

bool rate_limit_ok(WsUserData *data) {
  const auto now = std::chrono::steady_clock::now();
  if ((now - data->rate_window_start) > std::chrono::seconds(1)) {
    data->rate_window_start = now;
    data->messages_in_window = 0;
  }
  data->messages_in_window++;
  return data->messages_in_window <= kMaxMessagesPerSecond;
}

void leave_session(WsUserData *data) {
  if (!data->joined) return;

  auto it = g_sessions.find(data->session_id);
  if (it != g_sessions.end()) {
    auto *other = find_other_peer(it->second, data->peer_id);
    if (other && other->ws) {
      ws_send_json(other->ws, json{{"type", "bye"}, {"from", data->peer_id}});
    }
    remove_peer(it->second, data->peer_id);
    cleanup_if_empty(data->session_id);
  }

  data->joined = false;
  data->session_id.clear();
  data->peer_id.clear();
}

void handle_join(uWS::WebSocket<false, true, WsUserData> *ws, WsUserData *data, const json &msg) {
  prune_expired_sessions();
  if (data->joined) {
    ws_send_json(ws, json{{"ok", false}, {"type", "error"}, {"error", "already joined"}});
    return;
  }

  const std::string session_id = msg.value("session", "");
  const std::string password = msg.value("password", "");
  if (!valid_token(session_id)) {
    ws_send_json(ws, json{{"ok", false}, {"type", "error"}, {"error", "invalid session"}});
    return;
  }
  if (password != g_cfg.password) {
    ws_send_json(ws, json{{"ok", false}, {"type", "error"}, {"error", "invalid password"}});
    return;
  }

  auto &session = g_sessions[session_id];
  size_t peers = 0;
  if (session.peer_a.has_value()) peers++;
  if (session.peer_b.has_value()) peers++;
  if (peers >= 2) {
    ws_send_json(ws, json{{"ok", false}, {"type", "error"}, {"error", "session full"}});
    return;
  }

  PeerState peer{random_id(), ws, std::chrono::steady_clock::now()};
  if (!session.peer_a.has_value()) session.peer_a = peer;
  else session.peer_b = peer;

  data->joined = true;
  data->session_id = session_id;
  data->peer_id = peer.peer_id;

  const bool paired = session.peer_a.has_value() && session.peer_b.has_value();
  ws_send_json(ws, json{
                      {"ok", true},
                      {"type", "joined"},
                      {"peer_id", peer.peer_id},
                      {"state", paired ? "paired" : "waiting"},
                  });

  if (paired) {
    if (session.peer_a->ws) ws_send_json(session.peer_a->ws, json{{"type", "peer-ready"}});
    if (session.peer_b->ws) ws_send_json(session.peer_b->ws, json{{"type", "peer-ready"}});
  }
}

void handle_signal(uWS::WebSocket<false, true, WsUserData> *ws, WsUserData *data, const json &msg) {
  if (!data->joined) {
    ws_send_json(ws, json{{"ok", false}, {"type", "error"}, {"error", "not joined"}});
    return;
  }

  const std::string type = msg.value("type", "");
  if (type != "offer" && type != "answer" && type != "ice-candidate" && type != "bye") {
    ws_send_json(ws, json{{"ok", false}, {"type", "error"}, {"error", "invalid signal type"}});
    return;
  }

  auto it = g_sessions.find(data->session_id);
  if (it == g_sessions.end()) {
    ws_send_json(ws, json{{"ok", false}, {"type", "error"}, {"error", "session not found"}});
    return;
  }
  auto *other = find_other_peer(it->second, data->peer_id);
  if (!other || !other->ws) {
    ws_send_json(ws, json{{"ok", false}, {"type", "error"}, {"error", "peer not connected"}});
    return;
  }

  ws_send_json(other->ws, json{
                             {"type", type},
                             {"from", data->peer_id},
                             {"payload", msg.value("payload", json::object())},
                         });
}

}  // namespace

int main(int argc, char **argv) {
  const auto parsed = parse_args(argc, argv);
  if (!parsed.has_value()) {
    if (argc > 1) print_usage(argv[0]);
    return argc > 1 ? 2 : 0;
  }
  g_cfg = *parsed;

  const std::filesystem::path index_path = std::filesystem::path(g_cfg.static_dir) / "client.html";
  if (!std::filesystem::exists(index_path)) {
    log_line("fatal: client.html not found at " + index_path.string());
    return 1;
  }

  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  std::string client_html;
  if (!read_file_to_string(index_path, client_html)) {
    log_line("fatal: failed to read client.html");
    return 1;
  }
  std::string manifest_webmanifest;
  std::string sw_js;
  std::string icon_svg;
  std::string guide_html;
  read_file_to_string(std::filesystem::path(g_cfg.static_dir) / "manifest.webmanifest", manifest_webmanifest);
  read_file_to_string(std::filesystem::path(g_cfg.static_dir) / "sw.js", sw_js);
  read_file_to_string(std::filesystem::path(g_cfg.static_dir) / "icon.svg", icon_svg);
  read_file_to_string(std::filesystem::path(g_cfg.static_dir) / "guide.html", guide_html);

  auto app = uWS::App();
  app.get("/health", [](auto *res, auto *) {
    res->writeStatus("200 OK")->writeHeader("Content-Type", "text/plain; charset=utf-8")->end("ok\n");
  });

  app.get("/", [client_html](auto *res, auto *) {
    res->writeStatus("200 OK")
        ->writeHeader("Content-Type", "text/html; charset=utf-8")
        ->end(client_html);
  });
  app.get("/guide", [guide_html](auto *res, auto *) {
    if (guide_html.empty()) {
      res->writeStatus("404 Not Found")->end("guide missing");
      return;
    }
    res->writeStatus("200 OK")
        ->writeHeader("Content-Type", "text/html; charset=utf-8")
        ->end(guide_html);
  });
  app.get("/manifest.webmanifest", [manifest_webmanifest](auto *res, auto *) {
    if (manifest_webmanifest.empty()) {
      res->writeStatus("404 Not Found")->end("manifest missing");
      return;
    }
    res->writeStatus("200 OK")
        ->writeHeader("Content-Type", "application/manifest+json")
        ->end(manifest_webmanifest);
  });
  app.get("/sw.js", [sw_js](auto *res, auto *) {
    if (sw_js.empty()) {
      res->writeStatus("404 Not Found")->end("sw missing");
      return;
    }
    res->writeStatus("200 OK")
        ->writeHeader("Content-Type", "application/javascript; charset=utf-8")
        ->end(sw_js);
  });
  app.get("/icon.svg", [icon_svg](auto *res, auto *) {
    if (icon_svg.empty()) {
      res->writeStatus("404 Not Found")->end("icon missing");
      return;
    }
    res->writeStatus("200 OK")
        ->writeHeader("Content-Type", "image/svg+xml")
        ->end(icon_svg);
  });
  app.get("/api/config", [](auto *res, auto *) {
    json rtc = json::array();
    if (!g_cfg.stun_url.empty()) rtc.push_back(json{{"urls", g_cfg.stun_url}});
    if (!g_cfg.turn_url.empty()) {
      rtc.push_back(json{
          {"urls", g_cfg.turn_url},
          {"username", g_cfg.turn_username},
          {"credential", g_cfg.turn_password},
      });
    }
    res->writeStatus("200 OK")
        ->writeHeader("Content-Type", "application/json")
        ->end(json{{"ok", true}, {"ice_servers", rtc}}.dump());
  });

  app.ws<WsUserData>("/ws", {
                                .open = [](auto *ws) {
                                  ws_send_json(ws, json{{"type", "hello"}, {"ok", true}});
                                },
                                .message = [](auto *ws, std::string_view message, uWS::OpCode op) {
                                  if (op != uWS::OpCode::TEXT) return;
                                  if (message.size() > kMaxWsMessageBytes) {
                                    ws_send_json(ws, json{{"ok", false}, {"type", "error"}, {"error", "message too large"}});
                                    return;
                                  }
                                  WsUserData *data = (WsUserData *) ws->getUserData();
                                  if (!rate_limit_ok(data)) {
                                    ws_send_json(ws, json{{"ok", false}, {"type", "error"}, {"error", "rate limit"}});
                                    return;
                                  }
                                  json msg;
                                  try {
                                    msg = json::parse(message);
                                  } catch (...) {
                                    ws_send_json(ws, json{{"ok", false}, {"type", "error"}, {"error", "invalid json"}});
                                    return;
                                  }

                                  const std::string type = msg.value("type", "");
                                  if (type == "join") {
                                    handle_join(ws, data, msg);
                                  } else if (type == "offer" || type == "answer" || type == "ice-candidate" || type == "bye") {
                                    handle_signal(ws, data, msg);
                                  } else if (type == "leave") {
                                    leave_session(data);
                                    ws_send_json(ws, json{{"ok", true}, {"type", "left"}});
                                  } else {
                                    ws_send_json(ws, json{{"ok", false}, {"type", "error"}, {"error", "unsupported type"}});
                                  }
                                },
                                .close = [](auto *ws, int, std::string_view) {
                                  WsUserData *data = (WsUserData *) ws->getUserData();
                                  leave_session(data);
                                },
                            });

  log_line("starting on " + g_cfg.host + ":" + std::to_string(g_cfg.port));
  log_line("serving static files from " + g_cfg.static_dir);
  log_line("websocket signaling enabled on /ws");
  log_line("config endpoint enabled on /api/config");

  app.listen(g_cfg.host.c_str(), g_cfg.port, [](us_listen_socket_t *token) {
    g_listen_socket = token;
    if (!token) log_line("fatal: failed to bind/listen");
  });

  if (!g_listen_socket) return 1;
  app.run();

  log_line("shutdown complete");
  return 0;
}
