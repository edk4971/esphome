#pragma once

#include "esphome/core/defines.h"

#ifdef USE_OPENAI_RESPONSES

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

#include <atomic>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "esphome/components/openai_common/openai_http.h"
#include "esphome/components/openai_common/openai_audio.h"

#ifdef USE_OPENAI_RESPONSES_MCP
#include "esphome/components/openai_common/mcp_client.h"
#endif

namespace esphome::openai_responses {

// Bring the shared HttpMsgType enum into this namespace so existing
// unqualified references continue to resolve.
using openai_common::HttpMsgType;

#ifdef USE_OPENAI_RESPONSES_MCP
#ifndef MAX_PARALLEL_TOOLS
#define MAX_PARALLEL_TOOLS 4
#endif
#ifndef MAX_TOOL_ROUNDS
#define MAX_TOOL_ROUNDS 5
#endif
#ifndef MAX_TOOL_RESULT_BYTES
#define MAX_TOOL_RESULT_BYTES 8192
#endif

/// One accumulated tool call from a streaming Responses API response. Tool
/// calls arrive fragmented across SSE chunks: response.output_item.added
/// carries name + call_id + id, and response.function_call_arguments.delta
/// chunks append to `arguments` (partial JSON string). We accumulate per
/// output_index and only parse the arguments at stream end.
struct AccumulatedToolCall {
  int index{-1};          // output_index from the SSE stream
  std::string id;         // Responses API call_id (used to post function_call_output)
  std::string item_id;    // Responses API item id (fc_...) — for matching arg deltas
  std::string name;
  std::string arguments;  // accumulated partial JSON string from function_call_arguments.delta
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
  READING_CHAT,  // SSE: parse response.output_text.delta, accumulate response_text_, then build+send TTS
  READING_TTS,    // binary WAV stream -> speaker buffer
#ifdef USE_OPENAI_RESPONSES_MCP
  READING_MCP,         // MCP JSON-RPC response (tools/list or tools/call)
  FETCHING_TOOLS,      // proactive tools/list refresh before a turn starts
  EXECUTING_TOOLS,     // orchestrating tool calls: execute tools, re-query LLM
#endif
  DRAINING_AUDIO,  // TTS stream ended; wait for speaker to finish
  STREAMING_TTS_DRAIN,  // SSE done; TTS producer + feeder still draining
  // Teardown
  ERROR_TEARDOWN,  // cleanup after an error or explicit stop
};

// Which HTTP endpoint the SENDING_REQUEST/reading state is currently talking
// to. Tracked separately from State so the body-build and read states can be
// shared across endpoints.
enum class RequestTarget : uint8_t {
  NONE,
  CHAT,   // POST {endpoint_base}/v1/responses (multimodal or text)
  STT,    // POST {endpoint_base}/v1/audio/transcriptions (Mode 2)
  TTS,    // POST {endpoint_base}/v1/audio/speech
#ifdef USE_OPENAI_RESPONSES_MCP
  MCP_CALL,  // POST to a configured MCP server (tools/list or tools/call)
#endif
};

class OpenAIResponses : public esphome::openai_common::OpenAIHTTPBase {
 public:
  OpenAIResponses();
  ~OpenAIResponses();

  void setup() override;
  void loop() override;
  void dump_config() override;

  // --- Responses-specific wiring setters (called from codegen) ---
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
  void set_streaming_tts(bool v) { this->streaming_tts_ = v; }
  void set_has_tools(bool v) { this->has_tools_ = v; }
#ifdef USE_OPENAI_RESPONSES_MCP
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

#ifdef USE_OPENAI_RESPONSES_MCP
  /// Pre-fetch tools/list from all MCP servers. Called on boot after WiFi
  /// connects so the first wake-word turn doesn't pay the fetch latency.
  /// No-op if already cached or not idle.
  void prefetch_tools();
#endif

  // --- Responses-specific callback registration ---
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
  // Multimodal (Mode 1, round 1): builds the Responses API JSON with an
  // input_file part containing the base64-encoded WAV (44-byte header + PCM).
  void build_chat_request_body_multimodal_();
  // Mode 2: builds a multipart/form-data body with a 'file' field (WAV) and a
  // 'model' field, for /v1/audio/transcriptions.
  void build_stt_multipart_body_();
  // Mode 2 (round 1): builds the Responses API JSON with the transcribed user
  // text as an input message.
  void build_chat_request_body_text_(const std::string &user_text);
  // Builds the small /v1/audio/speech JSON body.
  void build_tts_request_body_();

  // --- Streaming TTS (concurrent with SSE reading) ---
  void start_tts_producer_task_();
  void stop_tts_producer_task_();
  static void tts_producer_task_fn_(void *arg);
  // Pushes a sentence to the thread-safe tts_queue_ and wakes the producer.
  void push_tts_sentence_(const std::string &sentence);
  // Pushes any remaining tts_pending_text_ and sets tts_queue_done_.
  void flush_tts_pending_();
  // Builds a TTS request body for the given text.
  void build_tts_request_body_for_(const std::string &text);
  // Scans for the last sentence boundary in text.
  size_t find_sentence_boundary_(const std::string &text);

  // --- Response processing ---
  // process_sse_line_() is implemented as an OpenAIHTTPBase virtual hook
  // override (declared above). Reads the STT JSON response fully and extracts
  // the "text" field.
  void process_stt_response_(const uint8_t *data, size_t len);

#ifdef USE_OPENAI_RESPONSES_MCP
  // --- Tool call handling ---
  void append_tool_result_message_(const std::string &tool_call_id, const std::string &result);
  void reset_accumulated_tool_calls_();
  void build_chat_request_body_from_history_();
  void retain_b64_audio_(size_t offset, size_t len);
  std::string extract_mcp_json_();
  void process_mcp_response_();
  void continue_request_start_();
  void build_cached_tools_json_();
#endif

  // --- Config (responses-specific; set once at codegen) ---
  std::string stt_model_;
  std::string stt_language_{"en"};
  std::string tts_model_;
  std::string tts_voice_;
  uint32_t tts_sample_rate_{24000};
  bool has_tools_{false};
  bool multimodal_{true};
#ifdef USE_OPENAI_RESPONSES_MCP
  std::vector<openai_common::McpServerConfig> mcp_servers_;
#endif

  // --- Runtime state ---
  State state_{State::IDLE};
  RequestTarget request_target_{RequestTarget::NONE};
  bool silence_detection_{true};

  // --- Streaming TTS (concurrent with SSE reading) ---
  bool streaming_tts_{true};

  // TTS producer task (pops sentences, does TTS HTTP, writes PCM to buffer).
  StaticTask tts_producer_task_;
  static constexpr uint32_t TTS_PRODUCER_STACK_SIZE = 8192;
  static constexpr UBaseType_t TTS_TASK_PRIORITY = 3;
  volatile bool tts_task_should_exit_{false};

  // Thread-safe queue of sentences to TTS.
  SemaphoreHandle_t tts_queue_mutex_{nullptr};
  std::deque<std::string> tts_queue_;
  std::atomic<bool> tts_queue_done_{false};
  std::atomic<bool> tts_streaming_active_{false};

  // Separate HTTP client for TTS (owned by the TTS producer task).
  esp_http_client_handle_t tts_http_client_{nullptr};

  // PSRAM ring buffer + feeder task (2 MB, SPSC, lock-free).
  openai_common::PsramAudioBuffer audio_buffer_;

  // Sentence accumulator for streaming TTS.
  std::string tts_pending_text_;

  // PSRAM speaker bookkeeping for streaming TTS playback (the 16 KiB
  // speaker_buffer_ itself lives in OpenAIBase).
  bool tts_stream_started_{false};

  // Accumulated chat response text + fallback reasoning text + STT transcript.
  std::string response_text_;
  std::string reasoning_text_;
  std::string stt_response_text_;

#ifdef USE_OPENAI_RESPONSES_MCP
  // --- Tool execution state ---
  AccumulatedToolCall accumulated_tool_calls_[MAX_PARALLEL_TOOLS];
  size_t accumulated_tool_call_count_{0};
  bool had_tool_calls_{false};
  std::string previous_response_id_;
  std::vector<std::string> turn_messages_;
  size_t turn_messages_sent_count_{0};
  char *retained_b64_audio_{nullptr};
  size_t retained_b64_len_{0};
  size_t retained_b64_capacity_{0};
  std::string user_text_;
  std::string mcp_response_text_;
  std::vector<std::string> raw_tools_per_server_;
  std::string cached_tools_json_;
  uint32_t tools_cache_timestamp_ms_{0};
  uint32_t tools_cache_ttl_ms_{86400000};  // 24h default
  std::vector<ToolRoute> tool_routes_;
  uint8_t mcp_route_server_index_{0};
  size_t current_tool_index_{0};
  uint8_t tool_round_{0};
  int mcp_request_id_{1};
  std::string current_mcp_url_;
  std::string current_mcp_auth_;
  size_t round1_b64_offset_{0};
  size_t round1_b64_len_{0};
  enum class McpPhase : uint8_t { IDLE, FETCHING_TOOLS, EXECUTING };
  McpPhase mcp_phase_{McpPhase::IDLE};
  enum class McpCallType : uint8_t { INITIALIZE, INITIALIZED_NOTIF, TOOLS_LIST, TOOLS_CALL };
  McpCallType current_mcp_call_type_{McpCallType::INITIALIZE};
  std::string current_mcp_session_id_;
  bool tools_prefetch_{false};
  bool tools_reinit_{false};
  uint8_t reinit_server_index_{0};
#endif

  // --- Responses-specific callback (the shared ones live in OpenAIBase) ---
  LazyCallbackManager<void()> on_tool_start_cb_;
};

// --- Automation actions / conditions (Parented to the component) ---

template<typename... Ts> class StartAction : public Action<Ts...>, public Parented<OpenAIResponses> {
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

template<typename... Ts> class StopAction : public Action<Ts...>, public Parented<OpenAIResponses> {
 public:
  void play(const Ts &...x) override { this->parent_->request_stop(); }
};

template<typename... Ts> class IsRunningCondition : public Condition<Ts...>, public Parented<OpenAIResponses> {
 public:
  bool check(const Ts &...x) override { return this->parent_->is_running(); }
};

}  // namespace esphome::openai_responses

#endif  // USE_OPENAI_RESPONSES
