#pragma once

#include "esphome/core/defines.h"

#ifdef USE_OPENAI_RESPONSES

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/static_task.h"

#include "esphome/components/audio/audio.h"
#include "esphome/components/microphone/microphone_source.h"
#include "esphome/components/ring_buffer/ring_buffer.h"
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

#include "esphome/components/openai_common/openai_audio.h"

#ifdef USE_OPENAI_RESPONSES_MCP
#include "mcp_client.h"
#endif

namespace esphome::openai_responses {

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

// Message types sent from the HTTP task to the main loop via a FreeRTOS
// message buffer. Each message starts with one of these bytes, followed by
// type-specific payload:
//   DATA  — raw response bytes (the rest of the message)
//   DONE  — no payload; the HTTP request completed successfully
//   ERROR — 1-byte code length, code bytes, 1-byte message length, message bytes
enum class HttpMsgType : uint8_t {
  DATA = 0,             // followed by raw response bytes (chat/STT only)
  DONE = 1,             // no payload; the HTTP request completed successfully
  ERROR_ = 2,           // 1-byte code length, code bytes, 1-byte message length, message bytes
  TTS_STREAM_START = 3, // no payload; the TTS speaker has started (fire on_tts_stream_start)
  TTS_STREAM_DONE = 4,  // no payload; the feeder task has drained all audio
};

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

class OpenAIResponses : public Component {
 public:
  OpenAIResponses();
  ~OpenAIResponses();

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override;

  // --- Wiring setters (called from codegen) ---
  void set_microphone_source(microphone::MicrophoneSource *mic_source) { this->mic_source_ = mic_source; }
  void set_speaker(speaker::Speaker *speaker) { this->speaker_ = speaker; }
#ifdef USE_MICRO_WAKE_WORD
  void set_micro_wake_word(micro_wake_word::MicroWakeWord *mww) { this->micro_wake_word_ = mww; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_text_request_sensor(text_sensor::TextSensor *sensor) { this->text_request_sensor_ = sensor; }
  void set_text_response_sensor(text_sensor::TextSensor *sensor) { this->text_response_sensor_ = sensor; }
#endif

  void set_endpoint_base(const std::string &v) { this->endpoint_base_ = v; }
  void set_api_key(const std::string &v) { this->api_key_ = v; }
  void set_chat_model(const std::string &v) { this->chat_model_ = v; }
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
  void set_system_prompt(const std::string &v) { this->system_prompt_ = v; }
  void set_silence_threshold(float v) { this->silence_threshold_ = v; }
  void set_silence_duration_ms(uint32_t v) { this->silence_duration_ms_ = v; }
  void set_max_recording_ms(uint32_t v) { this->max_recording_ms_ = v; }
  void set_volume_multiplier(float v) { this->volume_multiplier_ = v; }
  void set_streaming_tts(bool v) { this->streaming_tts_ = v; }
  void set_has_tools(bool v) { this->has_tools_ = v; }
  void set_wake_word(const std::string &v) { this->wake_word_ = v; }
#ifdef USE_OPENAI_RESPONSES_MCP
  void set_tools_cache_ttl_ms(uint32_t v) { this->tools_cache_ttl_ms_ = v; }
  void add_mcp_server(const std::string &name, const std::string &url, const std::string &api_key) {
    McpServerConfig cfg;
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
  bool is_running() const { return this->state_ != State::IDLE; }

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

  // --- Callback registration (templatized to accept forwarder structs) ---
  // Each registers into a LazyCallbackManager so idle instances cost only a
  // nullptr (4 bytes) instead of an empty vector.
  template<typename F> void add_on_start_callback(F &&cb) { this->on_start_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_listening_callback(F &&cb) { this->on_listening_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_wake_word_detected_callback(F &&cb) {
    this->on_wake_word_detected_cb_.add(std::forward<F>(cb));
  }
  template<typename F> void add_on_stt_end_callback(F &&cb) { this->on_stt_end_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_tool_start_callback(F &&cb) { this->on_tool_start_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_tts_start_callback(F &&cb) { this->on_tts_start_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_tts_end_callback(F &&cb) { this->on_tts_end_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_tts_stream_start_callback(F &&cb) {
    this->on_tts_stream_start_cb_.add(std::forward<F>(cb));
  }
  template<typename F> void add_on_tts_stream_end_callback(F &&cb) {
    this->on_tts_stream_end_cb_.add(std::forward<F>(cb));
  }
  template<typename F> void add_on_end_callback(F &&cb) { this->on_end_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_idle_callback(F &&cb) { this->on_idle_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_error_callback(F &&cb) { this->on_error_cb_.add(std::forward<F>(cb)); }

  protected:
  // --- Buffer lifecycle ---
  bool allocate_buffers_();
  void deallocate_buffers_();
  // Resets turn-scoped state (indices + variable-size turn buffers) without
  // freeing the long-lived fixed buffers (ring/recording/speaker/sse). Called
  // on every IDLE transition so the next turn reuses the pre-allocated PSRAM
  // instead of freeing ~1MB and reallocating it (which trips the main-loop
  // watchdog).
  void reset_turn_state_();

  // --- State machine helpers ---
  void set_state_(State s);
  void start_microphone_();
  void stop_microphone_();
  // Pulls audio from the ring buffer (written by the mic callback on another
  // task) into the contiguous recording buffer and updates the RMS-based VAD.
  void drain_ring_buffer_to_recording_();
  // Computes the RMS amplitude of a block of int16 samples, returned relative
  // to full-scale (0.0 .. 1.0). Used for the energy-based silence detector.
  static float compute_rms_(const int16_t *samples, size_t count);

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

  // --- HTTP plumbing (runs in a dedicated FreeRTOS task) ---
  // All blocking esp_http_client calls (DNS, connect, TLS, write, fetch
  // headers, read) run in http_task_fn_ on a separate task so the main loop
  // is never blocked. Response data flows back to the loop via a FreeRTOS
  // message buffer (http_msg_buffer_).
  //
  // Starts the HTTP task for the current request_target_ + request_body_.
  // The task handles open → write → fetch_headers → read loop, sending
  // DATA/DONE/ERROR messages via http_msg_buffer_.
  void start_http_task_();
  // Kills the HTTP task (if running). Non-blocking: returns true if the task
  // is suspended (or not created), false if still running (retry on next loop).
  // Avoids force-deleting a task blocked in lwIP recv_tcp() (crashes tcpip).
  bool stop_http_task_();
  // The task entry point. arg is the OpenAIResponses* this pointer.
  static void http_task_fn_(void *arg);
  void close_http_();
  // Sends an ERROR message (code + message) via the message buffer. Used by
  // the HTTP task to report failures back to the main loop. code and message
  // must be C string literals (or otherwise outlive the call).
  void send_http_error_(MessageBufferHandle_t buf, const char *code, const char *message);

  // --- Streaming TTS (concurrent with SSE reading) ---
  // Starts the TTS producer task (pops sentences, does TTS HTTP, writes PCM
  // to the PSRAM ring buffer). Also starts the speaker feeder task (reads PCM
  // from the buffer, feeds the i2s speaker continuously).
  void start_tts_producer_task_();
  void stop_tts_producer_task_();
  static void tts_producer_task_fn_(void *arg);
  // Pushes a sentence to the thread-safe tts_queue_ and wakes the producer.
  // On the first push of a turn, starts the producer + feeder tasks and fires
  // on_tts_start.
  void push_tts_sentence_(const std::string &sentence);
  // Pushes any remaining tts_pending_text_ and sets tts_queue_done_.
  void flush_tts_pending_();
  // Builds a TTS request body for the given text (refactored to accept a
  // parameter for per-sentence streaming TTS).
  void build_tts_request_body_for_(const std::string &text);
  // Scans for the last sentence boundary in text. Returns the position after
  // the boundary, or 0 if none found. Requires 40+ chars before splitting.
  size_t find_sentence_boundary_(const std::string &text);

  // --- Response processing ---
  // Feeds bytes from the HTTP message buffer into sse_line_buffer_ and, on
  // each complete '\n'-terminated line, calls process_sse_line_(). Tracks the
  // current `event:` type in sse_event_type_ between data lines.
  void process_sse_bytes_(const uint8_t *data, size_t len);
  // Handles one SSE line: tracks `event:` type lines in sse_event_type_, and
  // for `data:` lines parses the JSON payload and dispatches based on the
  // current event type (response.output_text.delta, response.function_call_*,
  // response.completed, etc.).
  void process_sse_line_(const char *line, size_t len);
  // Reads the STT JSON response fully and extracts the "text" field.
  void process_stt_response_(const uint8_t *data, size_t len);

#ifdef USE_OPENAI_RESPONSES_MCP
  // --- Tool call handling ---
  // Appends a function_call_output input item with the given call_id and
  // result text to turn_messages_.
  void append_tool_result_message_(const std::string &tool_call_id, const std::string &result);
  // Resets all accumulated tool call entries (count + per-entry fields).
  // Must be called before accumulating a new SSE stream's tool calls,
  // otherwise stale arguments from the previous turn get appended to.
  void reset_accumulated_tool_calls_();
  // Builds a round-2+ Responses API request body. When previous_response_id_
  // is set, only the function_call_output items are sent (the server already
  // has the context). Otherwise (fallback) the full round-1 context (tools,
  // instructions, user input) is rebuilt.
  void build_chat_request_body_from_history_();
  // Saves the base64 audio from the round-1 request body into retained_b64_audio_
  // so round 2+ can re-include it without re-encoding (fallback path only).
  void retain_b64_audio_(size_t offset, size_t len);
  // Extracts the JSON-RPC response from mcp_response_text_, handling both
  // single-JSON and SSE (data: lines) framing.
  std::string extract_mcp_json_();
  // Processes the parsed MCP response. Called from READING_MCP on DONE.
  void process_mcp_response_();
  // Continuation of request_start after the tools cache is refreshed (or when
  // the cache is valid). Fires on_start, stops MWW, allocates buffers, starts mic.
  void continue_request_start_();
  // Builds the cached Responses-API-format (flattened) tools JSON array from
  // raw MCP tools/list responses stored in raw_tools_per_server_. Called after
  // all servers queried.
  void build_cached_tools_json_();
#endif

  // --- Speaker output ---
  // Appends decoded PCM bytes to speaker_buffer_, applying volume_multiplier_,
  // and flushes to the speaker when the buffer fills.
  void feed_speaker_(const uint8_t *data, size_t len);
  void flush_speaker_buffer_();

  // --- Teardown ---
  // Frees all PSRAM buffers, closes HTTP, restarts micro_wake_word, fires
  // on_end/on_idle. Idempotent. Caller must ensure the speaker has stopped
  // before calling (i2s bus contention with the mic).
  void teardown_to_idle_();
  // Fires on_error with code/message and routes to ERROR_TEARDOWN.
  void fail_(const std::string &code, const std::string &message);

  // Static trampoline for esp_http_client event callback. We don't actually
  // need event data (status code, content-type are inferred from the request
  // target), so this is a no-op kept to satisfy the API. Signature matches
  // http_event_handle_cb: takes only the event pointer.
  static esp_err_t http_event_handler_(esp_http_client_event_t *event);

  // --- Config (set once at codegen) ---
  microphone::MicrophoneSource *mic_source_{nullptr};
  speaker::Speaker *speaker_{nullptr};
#ifdef USE_MICRO_WAKE_WORD
  micro_wake_word::MicroWakeWord *micro_wake_word_{nullptr};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *text_request_sensor_{nullptr};
  text_sensor::TextSensor *text_response_sensor_{nullptr};
#endif

  std::string endpoint_base_;
  std::string api_key_;
  std::string chat_model_;
  std::string stt_model_;
  std::string stt_language_{"en"};
  std::string tts_model_;
  std::string tts_voice_;
  std::string system_prompt_;
  std::string wake_word_;
  uint32_t tts_sample_rate_{24000};
  float silence_threshold_{0.002f};
  uint32_t silence_duration_ms_{700};
  uint32_t max_recording_ms_{30000};
  float volume_multiplier_{1.0f};
  bool has_tools_{false};
  bool multimodal_{true};
#ifdef USE_OPENAI_RESPONSES_MCP
  std::vector<McpServerConfig> mcp_servers_;
#endif

  // --- Runtime state ---
  State state_{State::IDLE};
  RequestTarget request_target_{RequestTarget::NONE};
  bool silence_detection_{true};
  esp_http_client_handle_t http_client_{nullptr};
  // The http client may retain a pointer to the URL string passed to init, so
  // the URL is kept alive here for the lifetime of the client.
  std::string current_url_;
  // "Bearer <api_key>" cached so its pointer stays valid for set_header.
  std::string auth_header_value_;

  // --- HTTP task (off-loads all blocking esp_http_client calls) ---
  // The task runs on its own FreeRTOS task with a PSRAM-backed stack. It
  // performs the full request lifecycle (open, write, fetch headers, read)
  // and sends response data back to the main loop via a message buffer.
  StaticTask http_task_;
  static constexpr uint32_t HTTP_TASK_STACK_SIZE = 8192;
  static constexpr UBaseType_t HTTP_TASK_PRIORITY = 3;
  // Message buffer for task → loop communication. Each message is prefixed
  // with a HttpMsgType byte. Sized to hold several read chunks so the task
  // doesn't block on sends while the loop is busy processing.
  static constexpr size_t HTTP_MSG_BUFFER_SIZE = 8192;
  MessageBufferHandle_t http_msg_buffer_{nullptr};
  // Set by the loop to request the task exit early (e.g. on request_stop).
  volatile bool http_task_should_exit_{false};
  // Read chunk size used by the task when calling esp_http_client_read.
  static constexpr size_t HTTP_TASK_READ_CHUNK = 2048;

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
  // Written by the TTS producer task, read by the speaker feeder task.
  openai_common::PsramAudioBuffer audio_buffer_;

  // Sentence accumulator for streaming TTS.
  std::string tts_pending_text_;

  // Ring buffer bridging the mic callback (producer, another task) and the
  // main loop (consumer). 16 KiB is enough to absorb mic bursts while the
  // loop drains it in chunks.
  std::unique_ptr<ring_buffer::RingBuffer> ring_buffer_;
  static constexpr size_t RING_BUFFER_SIZE = 16384;

  // Contiguous PSRAM buffer that accumulates the recorded WAV PCM. Sized to
  // max_recording_ms of 16 kHz/16-bit/mono audio (32 bytes/ms).
  uint8_t *recording_buffer_{nullptr};
  size_t recording_capacity_{0};
  size_t recording_size_{0};

  // PSRAM buffer holding a fully-built request body (chat JSON+base64, STT
  // multipart, or TTS JSON). Written by the build_* methods, drained by the
  // HTTP task's esp_http_client_write loop.
  uint8_t *request_body_{nullptr};
  size_t request_body_capacity_{0};
  size_t request_body_size_{0};
  size_t request_body_sent_{0};

  // PSRAM line buffer for reassembling SSE chunks that split across reads.
  char *sse_line_buffer_{nullptr};
  size_t sse_line_capacity_{0};
  size_t sse_line_len_{0};
  // Current SSE `event:` type for the Responses API stream. Set when an
  // "event:" line is seen, consumed (and cleared) when the matching "data:"
  // line arrives. The Responses API uses semantic event types
  // (response.output_text.delta, response.function_call_arguments.delta,
  // response.completed, ...) instead of a single choices[] payload.
  std::string sse_event_type_;

  // Accumulated chat response text. response.output_text.delta is accumulated
  // here (the actual response the user should hear). Some non-standard
  // servers (reasoning models via llama.cpp/GGUF) emit text via other event
  // types — that is accumulated separately in reasoning_text_ and only used
  // as a fallback if no output_text was ever received.
  std::string response_text_;
  std::string reasoning_text_;
  // STT transcript accumulated while reading /v1/audio/transcriptions.
  std::string stt_response_text_;

#ifdef USE_OPENAI_RESPONSES_MCP
  // --- Tool execution state ---
  // Accumulated tool calls from the current SSE stream (per output_index).
  AccumulatedToolCall accumulated_tool_calls_[MAX_PARALLEL_TOOLS];
  size_t accumulated_tool_call_count_{0};
  // True when the completed response contained at least one function_call
  // output item (set from the response.completed event). Replaces the
  // Chat Completions finish_reason == "tool_calls" check.
  bool had_tool_calls_{false};
  // The Responses API response id (resp_...) from the current turn. Set from
  // the response.created / response.completed event. Used as
  // previous_response_id on round 2+ so the server reuses its stored context
  // (tools, instructions, history) instead of us resending it.
  std::string previous_response_id_;
  // Turn history: function_call_output input items accumulated across tool
  // rounds. Each entry is a complete serialized JSON input item object. On
  // the previous_response_id path, only items added since the last round
  // (index >= turn_messages_sent_count_) are sent — the server already has
  // the earlier ones from the stored response. On the fallback path, all
  // items are sent.
  std::vector<std::string> turn_messages_;
  // Number of turn_messages_ already sent in previous rounds (used to send
  // only new tool outputs on the previous_response_id path). Reset each turn.
  size_t turn_messages_sent_count_{0};
  // For multimodal round 2+ fallback: retained base64 audio so we can
  // re-include the user's original audio without re-encoding. Allocated in
  // PSRAM. Unused on the previous_response_id path.
  char *retained_b64_audio_{nullptr};
  size_t retained_b64_len_{0};
  size_t retained_b64_capacity_{0};
  // For text mode round 2+ fallback: the transcribed user text.
  std::string user_text_;
  // MCP response accumulator (tools/list or tools/call).
  std::string mcp_response_text_;
  // Raw tools/list JSON per server, accumulated during FETCHING_TOOLS for
  // schema conversion. Cleared after build_cached_tools_json_().
  std::vector<std::string> raw_tools_per_server_;
  // Cached Responses-API-format (flattened) tools JSON array (built from
  // tools/list responses). Injected into round-1 requests. Empty when no
  // cache exists.
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
  // http_task after fetch_headers for MCP_CALL.
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

  // PSRAM speaker buffer + bookkeeping for streaming TTS playback.
  uint8_t *speaker_buffer_{nullptr};
  static constexpr size_t SPEAKER_BUFFER_SIZE = 16384;
  size_t speaker_buffer_index_{0};
  bool tts_stream_started_{false};
  // The first 44 bytes of the TTS WAV response are the RIFF/fmt header; we
  // skip them once so only raw PCM reaches the speaker.
  bool tts_header_skipped_{false};

  // --- VAD state (energy-based) ---
  uint32_t vad_last_check_ms_{0};
  uint32_t silence_since_ms_{0};      // timestamp marking the start of the current silent run
  uint32_t speech_onset_ms_{0};      // timestamp marking the start of the current above-threshold run
  bool speech_active_{false};         // true once speech onset has been confirmed
  bool speech_ended_{false};          // set when end-of-speech (silence after speech) is detected
  uint32_t listening_start_ms_{0};

  // --- Callback managers (lazy: 4 bytes each when empty) ---
  LazyCallbackManager<void()> on_start_cb_;
  LazyCallbackManager<void()> on_listening_cb_;
  LazyCallbackManager<void()> on_wake_word_detected_cb_;
  LazyCallbackManager<void(std::string)> on_stt_end_cb_;
  LazyCallbackManager<void()> on_tool_start_cb_;
  LazyCallbackManager<void(std::string)> on_tts_start_cb_;
  LazyCallbackManager<void(std::string)> on_tts_end_cb_;
  LazyCallbackManager<void()> on_tts_stream_start_cb_;
  LazyCallbackManager<void()> on_tts_stream_end_cb_;
  LazyCallbackManager<void()> on_end_cb_;
  LazyCallbackManager<void()> on_idle_cb_;
  LazyCallbackManager<void(std::string, std::string)> on_error_cb_;
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
