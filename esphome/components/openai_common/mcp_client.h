#pragma once

#include <string>

namespace esphome::openai_common {

/// Configuration for one MCP server (streamable-HTTP transport).
/// The component owns a vector of these, populated at codegen time.
struct McpServerConfig {
  std::string name;
  std::string url;
  /// "Bearer <token>" or empty when no auth is needed.
  std::string auth_header;
  /// Runtime: session ID from the `initialize` handshake. Empty when not yet
  /// initialized. Set once and reused for all subsequent requests to this server.
  std::string session_id;
  /// Runtime: whether the `initialize` handshake has been completed.
  bool initialized{false};
  /// Runtime: true when `initialize` returned no Mcp-Session-Id header.
  /// Stateless servers don't require the handshake; calling `initialize`
  /// creates a server-side session that then expires, causing 400s on
  /// subsequent calls. When true, skip `initialize` entirely.
  bool stateless{false};
};

/// Builds a JSON-RPC `initialize` request body.
/// The server responds with its capabilities and a `Mcp-Session-Id` header.
inline std::string mcp_build_initialize_request(int id) {
  return std::string("{\"jsonrpc\":\"2.0\",\"method\":\"initialize\",\"params\":{\"protocolVersion\":"
                     "\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{\"name\":\"esphome-openai\","
                     "\"version\":\"1.0\"}},\"id\":") +
         std::to_string(id) + "}";
}

/// Builds a JSON-RPC `notifications/initialized` notification body.
/// This is a notification (no `id`), so the server does not send a JSON-RPC
/// response (it may respond with HTTP 202 Accepted).
inline std::string mcp_build_initialized_notification() {
  return std::string("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
}

/// Builds a JSON-RPC ``tools/list`` request body.
/// ``id`` is the JSON-RPC request id (incremented per call by the caller).
inline std::string mcp_build_tools_list_request(int id) {
  return std::string("{\"jsonrpc\":\"2.0\",\"method\":\"tools/list\",\"id\":") + std::to_string(id) + "}";
}

/// Builds a JSON-RPC ``tools/call`` request body.
/// ``arguments_json`` should be a JSON object string (e.g. ``{}`` or
/// ``{"name":"kitchen"}``). If empty or not starting with ``{``, ``{}`` is used.
inline std::string mcp_build_tools_call_request(int id, const std::string &name,
                                                const std::string &arguments_json) {
  std::string args = arguments_json;
  if (args.empty() || args[0] != '{') {
    args = "{}";
  }
  // Escape the tool name (it comes from the LLM and may contain quotes).
  std::string escaped_name;
  escaped_name.reserve(name.size() + 8);
  for (char c : name) {
    if (c == '"' || c == '\\') {
      escaped_name.push_back('\\');
    }
    escaped_name.push_back(c);
  }
  return std::string("{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"params\":{\"name\":\"") + escaped_name +
         "\",\"arguments\":" + args + "},\"id\":" + std::to_string(id) + "}";
}

}  // namespace esphome::openai_common
