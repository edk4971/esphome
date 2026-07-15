#pragma once

#include "esphome/core/defines.h"

#ifdef USE_OPENAI_CONVERSATIONS

#include "esphome/core/automation.h"
#include "esphome/core/helpers.h"

#include "esphome/components/audio/audio.h"
#include "esphome/components/microphone/microphone_source.h"
#include "esphome/components/speaker/speaker.h"
#ifdef USE_MICRO_WAKE_WORD
#include "esphome/components/micro_wake_word/micro_wake_word.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif

#include <esp_http_client.h>
#include <freertos/message_buffer.h>

#include <memory>
#include <string>
#include <vector>

#include "esphome/components/openai_common/openai_http.h"

#ifdef USE_OPENAI_CONVERSATIONS_MCP
#include "esphome/components/openai_common/mcp_client.h"
#endif

namespace esphome::openai_conversations {

// Bring the shared HttpMsgType enum into this namespace so existing
// unqualified references continue to resolve.
using openai_common::HttpMsgType;

#ifdef USE_OPENAI_CONVERSATIONS_MCP
#ifndef MAX_PARALLEL_TOOLS
#define MAX_PARALLEL_TOOLS 4
#endif
#ifndef MAX_TOOL_ROUNDS
#define MAX_TOOL_ROUNDS 5
#endif
#ifndef MAX_TOOL_RESULT_BYTES
#define MAX_TOOL_RESULT_BYTES 8192
#endif

/// One accumulated tool call from a streaming chat response. Tool calls arrive
/// fragmented across SSE chunks: the first chunk for an index carries id +
/// function.name, and subsequent chunks append to function.arguments (partial
/// JSON string). We accumulate per-index and only parse the arguments at the end.
struct AccumulatedToolCall {
  int index{-1};
  std::string id;
  std::string name;
  std::string arguments;  // accumulated partial JSON string from delta.function.arguments
};

/// Tool-name to server-index routing entry. Built lazily by calling tools/list
/// on each configured MCP server, then cached for the component lifetime.
struct ToolRoute {
  std::string tool_name;
  uint8_t server_index;
};
#endif

// Finite state machine driving one conversation turn. The component is single
// threaded: every state transition happens in loop() and processes a bounded
// amount of work per pass (one HTTP chunk, one VAD check, one ring-buffer
// drain) to avoid tripping the main-loop watchdog.
enum class State : uint8_t {
  IDLE,
  // Deferred start: continue_request_start_() runs here (in our loop, not
  // inside MWW's wake-word trigger) so MWW's loop() isn't blocked by the
  // on_start callback + buffer reset.
  STARTING_TURN,
  // Microphone acquisition / release
  STARTING_MICROPHONE,
  STOPPING_MICROPHONE,
  // Listening + local VAD
  LISTENING,   // mic on, draining ring buffer into the recording buffer, VAD running
  RECORDING,   // speech detected above threshold; keep recording until silence
  // Request building (in PSRAM)
  BUILDING_REQUEST,  // multimodal: chat JSON+base64 ; STT mode: multipart form
  // HTTP request phase: which target endpoint we are talking to
  SENDING_REQUEST,   // chunked POST body write (chat | stt | tts | mcp)
  // Response reading phases
  READING_STT,   // Mode 2 only: read transcription JSON, then build+send chat
  READING_CHAT,  // SSE: parse delta.content, accumulate response_text_, then build+send TTS
  READING_TTS,    // binary WAV stream -> speaker buffer
#ifdef USE_OPENAI_CONVERSATIONS_MCP
  READING_MCP,         // MCP JSON-RPC response (tools/list or tools/call)
  FETCHING_TOOLS,      // proactive tools/list refresh before a turn starts
  EXECUTING_TOOLS,     // orchestrating tool calls: execute tools, re-query LLM
#endif
  DRAINING_AUDIO,  // TTS stream ended; wait for speaker to finish
  // Teardown
  ERROR_TEARDOWN,  // cleanup after an error or explicit stop
};

// Which HTTP endpoint the SENDING_REQUEST/reading state is currently talking
// to. Tracked separately from State so the body-build and read states can be
// shared across endpoints.
enum class RequestTarget : uint8_t {
  NONE,
  CHAT,   // POST {endpoint_base}/v1/chat/completions (multimodal or text)
  STT,    // POST {endpoint_base}/v1/audio/transcriptions (Mode 2)
  TTS,    // POST {endpoint_base}/v1/audio/speech
#ifdef USE_OPENAI_CONVERSATIONS_MCP
  MCP_CALL,  // POST to a configured MCP server (tools/list or tools/call)
#endif
};

class OpenAIConversations : public esphome::openai_common::OpenAIHTTPBase {
 public:
  OpenAIConversations();
  ~OpenAIConversations() override;

  void setup() override;
  void loop() override;
  void dump_config() override;

  // --- Conversations-specific wiring setters (called from codegen) ---
  void set_stt_model(const std::string &v) {
    this->stt_model_ = v;
    // Setting an STT model selects Mode 2 (transcribe first). An empty
    // stt_model_ means Mode 1 (multimodal audio sent straight to chat).
    this->multimodal_ = v.empty();
  }
  void set_stt_language(const std::string &v) { this->stt_language_ = v; }
  void set_tts_model(const std::string &v) { this->tts_model_ = v; }
  void set_tts_voice(const std::string &v) { this->tts_voice_ = v; }
  void set_tts_sample_rate(uint32_t v) { this->tts_sample_rate_ = v; }
  void set_has_tools(bool v) { this->has_tools_ = v; }
#ifdef USE_OPENAI_CONVERSATIONS_MCP
  void set_tools_cache_ttl_ms(uint32_t v) { this->tools_cache_ttl_ms_ = v; }
  void add_mcp_server(const std::string &name, const std::string &url, const std::string &api_key) {
    openai_common::McpServerConfig cfg;
    cfg.name = name;
    cfg.url = url;
    if (!api_key.empty()) {
      cfg.auth_header = "Bearer " + api_key;
    }
    this->mcp_servers_.push_back(std::move(cfg));
  }
#endif

  // --- Public API (used by actions/conditions) ---
  void request_start(bool silence_detection);
  void request_stop();

  /// Pre-warm models by POSTing to /backend/load for each configured model.
  /// Called on boot after WiFi connects. Fire-and-forget.
  void prewarm_models();
  static void prewarm_models_task_(void *arg);

#ifdef USE_OPENAI_CONVERSATIONS_MCP
  /// Pre-fetch tools/list from all MCP servers. Called on boot after WiFi
  /// connects so the first wake-word turn doesn't pay the fetch latency.
  /// No-op if already cached or not idle.
  void prefetch_tools();
#endif

  // --- Conversations-specific callback registration ---
  template<typename F> void add_on_tool_start_callback(F &&cb) { this->on_tool_start_cb_.add(std::forward<F>(cb)); }

 protected:
  // --- OpenAIBase overrides ---
  bool is_active_() const override;
  void teardown_to_idle_() override;
  void fail_(const std::string &code, const std::string &message) override;

  // --- OpenAIHTTPBase virtual hooks ---
  bool build_http_url_and_content_type_(char *url, size_t buf_size,
                                        const char *&content_type) const override;
  void set_http_extra_headers_(esp_http_client_handle_t client) override;
  bool is_http_status_acceptable_(int status) const override;
  bool http_feeds_speaker_() const override;
  void http_feed_speaker_(esp_http_client_handle_t client) override;
  void process_sse_line_(const char *line, size_t len) override;
  void on_http_header_(const char *key, const char *value) override;

  // --- Buffer lifecycle overrides (extend the shared base implementations) ---
  bool allocate_buffers_() override;
  void deallocate_buffers_() override;
  void reset_turn_state_() override;

  // --- State machine helpers ---
  void set_state_(State s);
  void start_microphone_();
  void stop_microphone_();

  // --- Request body builders (write into request_body_, set request_body_size_) ---
  // Multimodal (Mode 1): builds the chat-completions JSON with an audio_url
  // data URL containing the base64-encoded WAV (44-byte header + PCM).
  void build_chat_request_body_multimodal_();
  // Mode 2: builds a multipart/form-data body with a 'file' field (WAV) and a
  // 'model' field, for /v1/audio/transcriptions.
  void build_stt_multipart_body_();
  // Mode 2 (round 1): builds the chat-completions JSON with the transcribed user
  // text as a message.
  void build_chat_request_body_text_(const std::string &user_text);
  // Builds the small /v1/audio/speech JSON body.
  void build_tts_request_body_();

  // --- Response processing ---
  // process_sse_line_() is implemented as an OpenAIHTTPBase virtual hook
  // override (declared above). Reads the STT JSON response fully and extracts
  // the "text" field.
  void process_stt_response_(const uint8_t *data, size_t len);

#ifdef USE_OPENAI_CONVERSATIONS_MCP
  // --- Tool call handling ---
  // Appends the assistant message (with its tool_calls array) to turn_messages_.
  void append_assistant_tool_calls_message_();
  // Appends a role:"tool" message with the given tool_call_id and result text.
  void append_tool_result_message_(const std::string &tool_call_id, const std::string &result);
  // Resets all accumulated tool call entries (count + per-entry fields).
  // Must be called before accumulating a new SSE stream's tool calls,
  // otherwise stale arguments from the previous turn get appended to.
  void reset_accumulated_tool_calls_();
  // Builds a round-2+ chat request body from the accumulated turn history.
  // For multimodal, re-uses the retained base64 audio; for text, re-uses the
  // stored user_text_.
  void build_chat_request_body_from_history_();
  // Saves the base64 audio from the round-1 request body into retained_b64_audio_
  // so round 2+ can re-include it without re-encoding.
  void retain_b64_audio_(size_t offset, size_t len);
  // Extracts the JSON-RPC response from mcp_response_text_, handling both
  // single-JSON and SSE (data: lines) framing.
  std::string extract_mcp_json_();
  // Processes the parsed MCP response. Called from READING_MCP on DONE.
  void process_mcp_response_();
  // Continuation of request_start after the tools cache is refreshed (or when
  // the cache is valid). Fires on_start, stops MWW, allocates buffers, starts mic.
  void continue_request_start_();
  // Builds the cached OpenAI-format tools JSON array from raw MCP tools/list
  // responses stored in raw_tools_per_server_. Called after all servers queried.
  void build_cached_tools_json_();
#endif

  // --- Config (conversations-specific; set once at codegen) ---
  std::string stt_model_;
  std::string stt_language_{"en"};
  std::string tts_model_;
  std::string tts_voice_;
  uint32_t tts_sample_rate_{24000};
  bool has_tools_{false};
  bool multimodal_{true};
#ifdef USE_OPENAI_CONVERSATIONS_MCP
  std::vector<openai_common::McpServerConfig> mcp_servers_;
#endif

  // --- Runtime state ---
  State state_{State::IDLE};
  RequestTarget request_target_{RequestTarget::NONE};
  bool silence_detection_{true};

  // PSRAM speaker bookkeeping for streaming TTS playback (the 16 KiB
  // speaker_buffer_ itself lives in OpenAIBase).
  bool tts_stream_started_{false};

  // Accumulated chat response text + fallback reasoning text + STT transcript.
  std::string response_text_;
  std::string reasoning_text_;
  std::string stt_response_text_;

#ifdef USE_OPENAI_CONVERSATIONS_MCP
  // --- Tool execution state ---
  AccumulatedToolCall accumulated_tool_calls_[MAX_PARALLEL_TOOLS];
  size_t accumulated_tool_call_count_{0};
  // Captured finish_reason from the terminal SSE chunk ("tool_calls" or "stop").
  std::string finish_reason_;
  // Turn history: assistant + tool messages accumulated across rounds. Each
  // entry is a complete serialized JSON message object. System and user
  // messages are rebuilt at request time (they don't change between rounds).
  std::vector<std::string> turn_messages_;
  // For multimodal round 2+: retained base64 audio so we can re-include the
  // user's original audio without re-encoding. Allocated in PSRAM.
  char *retained_b64_audio_{nullptr};
  size_t retained_b64_len_{0};
  size_t retained_b64_capacity_{0};
  // For text mode round 2+: the transcribed user text.
  std::string user_text_;
  // MCP response accumulator (tools/list or tools/call).
  std::string mcp_response_text_;
  // Raw tools/list JSON per server, accumulated during FETCHING_TOOLS for
  // schema conversion. Cleared after build_cached_tools_json_().
  std::vector<std::string> raw_tools_per_server_;
  // Cached OpenAI-format tools JSON array (built from tools/list responses).
  // Injected into chat requests. Empty when no cache exists.
  std::string cached_tools_json_;
  // Millis() timestamp of the last successful tools cache refresh. 0 = never.
  uint32_t tools_cache_timestamp_ms_{0};
  // Config: how long (ms) the cache is valid before a refresh is needed.
  uint32_t tools_cache_ttl_ms_{86400000};  // 24h default
  // Tool routing: tool_name -> server_index, built during FETCHING_TOOLS.
  std::vector<ToolRoute> tool_routes_;
  // Which MCP server we are calling tools/list on (during route building).
  uint8_t mcp_route_server_index_{0};
  // Which tool call we are executing (index into accumulated_tool_calls_).
  size_t current_tool_index_{0};
  // Round counter (incremented per tool round; capped at MAX_TOOL_ROUNDS).
  uint8_t tool_round_{0};
  // JSON-RPC request id counter.
  int mcp_request_id_{1};
  // Current MCP server URL + auth (set before starting http task for MCP_CALL).
  std::string current_mcp_url_;
  std::string current_mcp_auth_;
  // Offset and length of base64 in the round-1 request body (for retention).
  size_t round1_b64_offset_{0};
  size_t round1_b64_len_{0};
  // Sub-state for MCP fetch: are we proactively refreshing tools (before a
  // turn starts) or executing a tool call mid-turn?
  enum class McpPhase : uint8_t { IDLE, FETCHING_TOOLS, EXECUTING };
  McpPhase mcp_phase_{McpPhase::IDLE};
  // Which JSON-RPC call we're making on the current MCP server. Drives the
  // FETCHING_TOOLS state machine: INITIALIZE -> INITIALIZED_NOTIF -> TOOLS_LIST.
  enum class McpCallType : uint8_t { INITIALIZE, INITIALIZED_NOTIF, TOOLS_LIST, TOOLS_CALL };
  McpCallType current_mcp_call_type_{McpCallType::INITIALIZE};
  // Session ID captured from the `initialize` response header. Set by the
  // on_http_header_ hook after fetch_headers for MCP_CALL.
  std::string current_mcp_session_id_;
  // When true, FETCHING_TOOLS was triggered by prefetch_tools() on boot —
  // return to IDLE when done instead of continuing a conversation turn.
  bool tools_prefetch_{false};
  // When true, FETCHING_TOOLS is re-initializing a stateful server whose
  // session expired mid-turn. On completion, return to EXECUTING_TOOLS and
  // retry the tool call (don't call continue_request_start_ or return to IDLE).
  bool tools_reinit_{false};
  // When tools_reinit_ is true, this is the server index to re-initialize.
  // Only this one server is queried; others are skipped.
  uint8_t reinit_server_index_{0};
#endif

  // --- Conversations-specific callback (the shared ones live in OpenAIBase) ---
  LazyCallbackManager<void()> on_tool_start_cb_;
};

// --- Automation actions / conditions (Parented to the component) ---

template<typename... Ts> class StartAction : public Action<Ts...>, public Parented<OpenAIConversations> {
  TEMPLATABLE_VALUE(std::string, wake_word)

 public:
  void play(const Ts &...x) override {
    this->parent_->set_wake_word(this->wake_word_.value(x...));
    this->parent_->request_start(this->silence_detection_);
  }
  void set_silence_detection(bool silence_detection) { this->silence_detection_ = silence_detection; }

 protected:
  bool silence_detection_{true};
};

template<typename... Ts> class StopAction : public Action<Ts...>, public Parented<OpenAIConversations> {
 public:
  void play(const Ts &...x) override { this->parent_->request_stop(); }
};

template<typename... Ts> class IsRunningCondition : public Condition<Ts...>, public Parented<OpenAIConversations> {
 public:
  bool check(const Ts &...x) override { return this->parent_->is_running(); }
};

}  // namespace esphome::openai_conversations

#endif  // USE_OPENAI_CONVERSATIONS
