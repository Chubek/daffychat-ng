#include <csignal>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "App.h"
#include "confuse.h"
#include "nlohmann/json.hpp"

namespace {

using json = nlohmann::json;

struct Config {
  std::string host = "0.0.0.0";
  int port = 8080;
  std::string static_dir = ".";
  std::string password = "changeme";
  std::string stun_url = "";
  std::string turn_url = "";
  std::string turn_username = "";
  std::string turn_password = "";
  size_t max_ws_message_bytes = 64 * 1024;
  unsigned int max_messages_per_second = 60;
  int peer_session_max_age_minutes = 30;
};

struct CliOverrides {
  bool help = false;
  std::optional<std::string> config_file;
  std::optional<std::string> host;
  std::optional<int> port;
  std::optional<std::string> static_dir;
  std::optional<std::string> password;
  std::optional<std::string> stun_url;
  std::optional<std::string> turn_url;
  std::optional<std::string> turn_username;
  std::optional<std::string> turn_password;
  std::optional<size_t> max_ws_message_bytes;
  std::optional<unsigned int> max_messages_per_second;
  std::optional<int> peer_session_max_age_minutes;
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

void signal_handler(int) {
  if (g_listen_socket) {
    us_listen_socket_close(0, g_listen_socket);
    g_listen_socket = nullptr;
  }
}

void log_line(const std::string &msg) { std::cerr << "[daffychat] " << msg << '\n'; }

void print_usage(const char *argv0) {
  std::cerr
      << "Usage: " << argv0
      << " [--config <path>] [--host <address>] [--port <port>] [--static-dir <path>] [--password <shared-secret>]\n"
      << "       [--stun-url <url>] [--turn-url <url>] [--turn-username <name>] [--turn-password <secret>]\n"
      << "       [--max-ws-message-bytes <n>] [--max-messages-per-second <n>] [--peer-session-max-age-minutes <n>]\n";
}

bool parse_int_arg(const std::string &raw, int min_value, int max_value, int &value) {
  try {
    const int parsed = std::stoi(raw);
    if (parsed < min_value || parsed > max_value) return false;
    value = parsed;
    return true;
  } catch (...) {
    return false;
  }
}

bool parse_uint_arg(const std::string &raw, unsigned int min_value, unsigned int max_value, unsigned int &value) {
  try {
    const unsigned long parsed = std::stoul(raw);
    if (parsed < min_value || parsed > max_value) return false;
    value = static_cast<unsigned int>(parsed);
    return true;
  } catch (...) {
    return false;
  }
}

bool parse_size_arg(const std::string &raw, size_t min_value, size_t max_value, size_t &value) {
  try {
    const unsigned long long parsed = std::stoull(raw);
    if (parsed < min_value || parsed > max_value) return false;
    value = static_cast<size_t>(parsed);
    return true;
  } catch (...) {
    return false;
  }
}

std::optional<CliOverrides> parse_cli_overrides(int argc, char **argv) {
  CliOverrides out;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto need = [&](std::string &dst) -> bool {
      if (i + 1 >= argc) return false;
      dst = argv[++i];
      return true;
    };

    if (arg == "-h" || arg == "--help") {
      out.help = true;
      continue;
    }
    if (arg == "--config") {
      std::string v;
      if (!need(v)) return std::nullopt;
      out.config_file = v;
      continue;
    }
    if (arg == "--host") {
      std::string v;
      if (!need(v)) return std::nullopt;
      out.host = v;
      continue;
    }
    if (arg == "--port") {
      std::string v;
      if (!need(v)) return std::nullopt;
      int parsed = 0;
      if (!parse_int_arg(v, 1, 65535, parsed)) return std::nullopt;
      out.port = parsed;
      continue;
    }
    if (arg == "--static-dir") {
      std::string v;
      if (!need(v)) return std::nullopt;
      out.static_dir = v;
      continue;
    }
    if (arg == "--password") {
      std::string v;
      if (!need(v) || v.empty()) return std::nullopt;
      out.password = v;
      continue;
    }
    if (arg == "--stun-url") {
      std::string v;
      if (!need(v)) return std::nullopt;
      out.stun_url = v;
      continue;
    }
    if (arg == "--turn-url") {
      std::string v;
      if (!need(v)) return std::nullopt;
      out.turn_url = v;
      continue;
    }
    if (arg == "--turn-username") {
      std::string v;
      if (!need(v)) return std::nullopt;
      out.turn_username = v;
      continue;
    }
    if (arg == "--turn-password") {
      std::string v;
      if (!need(v)) return std::nullopt;
      out.turn_password = v;
      continue;
    }
    if (arg == "--max-ws-message-bytes") {
      std::string v;
      if (!need(v)) return std::nullopt;
      size_t parsed = 0;
      if (!parse_size_arg(v, 1024, 4 * 1024 * 1024, parsed)) return std::nullopt;
      out.max_ws_message_bytes = parsed;
      continue;
    }
    if (arg == "--max-messages-per-second") {
      std::string v;
      if (!need(v)) return std::nullopt;
      unsigned int parsed = 0;
      if (!parse_uint_arg(v, 1, 5000, parsed)) return std::nullopt;
      out.max_messages_per_second = parsed;
      continue;
    }
    if (arg == "--peer-session-max-age-minutes") {
      std::string v;
      if (!need(v)) return std::nullopt;
      int parsed = 0;
      if (!parse_int_arg(v, 1, 24 * 60, parsed)) return std::nullopt;
      out.peer_session_max_age_minutes = parsed;
      continue;
    }
    return std::nullopt;
  }
  return out;
}

int cfg_func_set_stun(cfg_t *cfg, cfg_opt_t *, int argc, const char **argv) {
  if (argc != 1) {
    cfg_error(cfg, "set_stun(url) requires exactly one argument");
    return -1;
  }
  return cfg_setstr(cfg, "stun_url", argv[0]);
}

int cfg_func_set_turn(cfg_t *cfg, cfg_opt_t *, int argc, const char **argv) {
  if (argc != 3) {
    cfg_error(cfg, "set_turn(url, username, password) requires exactly three arguments");
    return -1;
  }
  if (cfg_setstr(cfg, "turn_url", argv[0]) != CFG_SUCCESS) return -1;
  if (cfg_setstr(cfg, "turn_username", argv[1]) != CFG_SUCCESS) return -1;
  if (cfg_setstr(cfg, "turn_password", argv[2]) != CFG_SUCCESS) return -1;
  return 0;
}

int cfg_func_limit(cfg_t *cfg, cfg_opt_t *, int argc, const char **argv) {
  if (argc != 2) {
    cfg_error(cfg, "limit(name, value) requires exactly two arguments");
    return -1;
  }

  const std::string key = argv[0];
  const std::string value = argv[1];
  if (key == "max_ws_message_bytes") {
    size_t parsed = 0;
    if (!parse_size_arg(value, 1024, 4 * 1024 * 1024, parsed)) {
      cfg_error(cfg, "limit(max_ws_message_bytes, value): invalid value");
      return -1;
    }
    return cfg_setint(cfg, "max_ws_message_bytes", static_cast<long>(parsed));
  }
  if (key == "max_messages_per_second") {
    unsigned int parsed = 0;
    if (!parse_uint_arg(value, 1, 5000, parsed)) {
      cfg_error(cfg, "limit(max_messages_per_second, value): invalid value");
      return -1;
    }
    return cfg_setint(cfg, "max_messages_per_second", static_cast<long>(parsed));
  }
  if (key == "peer_session_max_age_minutes") {
    int parsed = 0;
    if (!parse_int_arg(value, 1, 24 * 60, parsed)) {
      cfg_error(cfg, "limit(peer_session_max_age_minutes, value): invalid value");
      return -1;
    }
    return cfg_setint(cfg, "peer_session_max_age_minutes", static_cast<long>(parsed));
  }

  cfg_error(cfg, "limit(): unknown key '%s'", argv[0]);
  return -1;
}

bool validate_config(const Config &cfg, std::string &error) {
  if (cfg.host.empty()) {
    error = "host must not be empty";
    return false;
  }
  if (cfg.port < 1 || cfg.port > 65535) {
    error = "port must be in range 1..65535";
    return false;
  }
  if (cfg.password.empty()) {
    error = "password must not be empty";
    return false;
  }
  if (cfg.max_ws_message_bytes < 1024 || cfg.max_ws_message_bytes > 4 * 1024 * 1024) {
    error = "max_ws_message_bytes out of supported range";
    return false;
  }
  if (cfg.max_messages_per_second < 1 || cfg.max_messages_per_second > 5000) {
    error = "max_messages_per_second out of supported range";
    return false;
  }
  if (cfg.peer_session_max_age_minutes < 1 || cfg.peer_session_max_age_minutes > 24 * 60) {
    error = "peer_session_max_age_minutes out of supported range";
    return false;
  }
  return true;
}

bool load_config_file(const std::string &path, Config &cfg, std::string &error) {
  if (path.empty()) return true;
  if (!std::filesystem::exists(path)) {
    error = "config file not found: " + path;
    return false;
  }

  cfg_opt_t opts[] = {
      CFG_STR("host", cfg.host.c_str(), CFGF_NONE),
      CFG_INT("port", cfg.port, CFGF_NONE),
      CFG_STR("static_dir", cfg.static_dir.c_str(), CFGF_NONE),
      CFG_STR("password", cfg.password.c_str(), CFGF_NONE),
      CFG_STR("stun_url", cfg.stun_url.c_str(), CFGF_NONE),
      CFG_STR("turn_url", cfg.turn_url.c_str(), CFGF_NONE),
      CFG_STR("turn_username", cfg.turn_username.c_str(), CFGF_NONE),
      CFG_STR("turn_password", cfg.turn_password.c_str(), CFGF_NONE),
      CFG_INT("max_ws_message_bytes", static_cast<long int>(cfg.max_ws_message_bytes), CFGF_NONE),
      CFG_INT("max_messages_per_second", static_cast<long int>(cfg.max_messages_per_second), CFGF_NONE),
      CFG_INT("peer_session_max_age_minutes", cfg.peer_session_max_age_minutes, CFGF_NONE),
      CFG_FUNC("set_stun", &cfg_func_set_stun),
      CFG_FUNC("set_turn", &cfg_func_set_turn),
      CFG_FUNC("limit", &cfg_func_limit),
      CFG_FUNC("include", &cfg_include),
      CFG_END(),
  };

  cfg_t *parser = cfg_init(opts, CFGF_NOCASE);
  if (!parser) {
    error = "cfg_init() failed";
    return false;
  }

  const int ret = cfg_parse(parser, path.c_str());

  if (ret == CFG_FILE_ERROR) {
    error = "failed to read config file: " + path;
    cfg_free(parser);
    return false;
  }
  if (ret == CFG_PARSE_ERROR) {
    error = "parse error in config file: " + path;
    cfg_free(parser);
    return false;
  }

  cfg.host = cfg_getstr(parser, "host");
  cfg.port = static_cast<int>(cfg_getint(parser, "port"));
  cfg.static_dir = cfg_getstr(parser, "static_dir");
  cfg.password = cfg_getstr(parser, "password");
  cfg.stun_url = cfg_getstr(parser, "stun_url");
  cfg.turn_url = cfg_getstr(parser, "turn_url");
  cfg.turn_username = cfg_getstr(parser, "turn_username");
  cfg.turn_password = cfg_getstr(parser, "turn_password");
  cfg.max_ws_message_bytes = static_cast<size_t>(cfg_getint(parser, "max_ws_message_bytes"));
  cfg.max_messages_per_second = static_cast<unsigned int>(cfg_getint(parser, "max_messages_per_second"));
  cfg.peer_session_max_age_minutes = static_cast<int>(cfg_getint(parser, "peer_session_max_age_minutes"));

  cfg_free(parser);
  return true;
}

void apply_cli_overrides(Config &cfg, const CliOverrides &ov) {
  if (ov.host) cfg.host = *ov.host;
  if (ov.port) cfg.port = *ov.port;
  if (ov.static_dir) cfg.static_dir = *ov.static_dir;
  if (ov.password) cfg.password = *ov.password;
  if (ov.stun_url) cfg.stun_url = *ov.stun_url;
  if (ov.turn_url) cfg.turn_url = *ov.turn_url;
  if (ov.turn_username) cfg.turn_username = *ov.turn_username;
  if (ov.turn_password) cfg.turn_password = *ov.turn_password;
  if (ov.max_ws_message_bytes) cfg.max_ws_message_bytes = *ov.max_ws_message_bytes;
  if (ov.max_messages_per_second) cfg.max_messages_per_second = *ov.max_messages_per_second;
  if (ov.peer_session_max_age_minutes) cfg.peer_session_max_age_minutes = *ov.peer_session_max_age_minutes;
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
  const auto max_age = std::chrono::minutes(g_cfg.peer_session_max_age_minutes);
  std::vector<std::string> to_erase;
  to_erase.reserve(g_sessions.size());
  for (auto &kv : g_sessions) {
    auto &s = kv.second;
    if (s.peer_a && (now - s.peer_a->connected_at) > max_age) s.peer_a.reset();
    if (s.peer_b && (now - s.peer_b->connected_at) > max_age) s.peer_b.reset();
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
  return data->messages_in_window <= g_cfg.max_messages_per_second;
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
    if (type == "offer" || type == "answer" || type == "ice-candidate" || type == "bye") {
      return;
    }
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
  const auto ov = parse_cli_overrides(argc, argv);
  if (!ov.has_value()) {
    print_usage(argv[0]);
    return 2;
  }
  if (ov->help) {
    print_usage(argv[0]);
    return 0;
  }

  Config cfg;
  std::string error;
  if (ov->config_file) {
    if (!load_config_file(*ov->config_file, cfg, error)) {
      log_line("fatal: " + error);
      return 1;
    }
  }
  apply_cli_overrides(cfg, *ov);
  if (!validate_config(cfg, error)) {
    log_line("fatal: invalid configuration: " + error);
    return 1;
  }
  g_cfg = cfg;

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
                                  if (message.size() > g_cfg.max_ws_message_bytes) {
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
  log_line("limits: max_ws_message_bytes=" + std::to_string(g_cfg.max_ws_message_bytes) +
           ", max_messages_per_second=" + std::to_string(g_cfg.max_messages_per_second) +
           ", peer_session_max_age_minutes=" + std::to_string(g_cfg.peer_session_max_age_minutes));

  app.listen(g_cfg.host.c_str(), g_cfg.port, [](us_listen_socket_t *token) {
    g_listen_socket = token;
    if (!token) log_line("fatal: failed to bind/listen");
  });

  if (!g_listen_socket) return 1;
  app.run();

  log_line("shutdown complete");
  return 0;
}
