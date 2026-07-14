#pragma once

#include "esphome/core/defines.h"

#ifdef USE_OPENAI_COMMON

#include "openai_common.h"

#include "esphome/components/ring_buffer/ring_buffer.h"

#include <esp_http_client.h>
#include <freertos/message_buffer.h>

#include <memory>
#include <string>
#include <vector>

namespace esphome::openai_common {

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

/// HTTP + VAD + MCP infrastructure shared by the responses and conversations
/// components. Inherits from OpenAIBase and adds: the HTTP task plumbing
/// (esp_http_client off-loaded to a FreeRTOS task), local energy-based VAD,
/// the contiguous PSRAM recording buffer, SSE line reassembly, the PSRAM
/// request-body buffer, and the fixed-buffer lifecycle (allocate / deallocate /
/// reset-turn). Protocol-specific behaviour is provided through virtual hooks.
class OpenAIHTTPBase : public OpenAIBase {
 public:
  OpenAIHTTPBase() = default;
  ~OpenAIHTTPBase() override = default;

  void dump_config() override;

  // --- Buffer lifecycle (shared fixed PSRAM buffers) ---
  /// Allocates the long-lived fixed buffers (ring, recording, speaker, SSE)
  /// once at setup. Idempotent (guards each alloc with if (nullptr)). Returns
  /// false on allocation failure. Virtual so subclasses can extend with their
  /// own buffers (call the base implementation first).
  virtual bool allocate_buffers_();
  /// Frees all PSRAM buffers (fixed + variable-size turn buffers). Called on
  /// destruction / full teardown. Virtual so subclasses can extend.
  virtual void deallocate_buffers_();
  /// Resets turn-scoped indices + frees variable-size turn buffers WITHOUT
  /// touching the long-lived fixed buffers. Called on every IDLE transition so
  /// the next turn reuses the pre-allocated PSRAM. Virtual so subclasses can
  /// extend with their own turn-scoped state.
  virtual void reset_turn_state_();

  // --- HTTP task plumbing (runs on a dedicated FreeRTOS task) ---
  void start_http_task_();
  /// Non-blocking: returns true if the task is suspended (or not created),
  /// false if still running (retry on next loop).
  bool stop_http_task_();
  static void http_task_fn_(void *arg);
  void close_http_();
  /// Packs [ERROR_][code_len][code][msg_len][msg] into the message buffer.
  void send_http_error_(MessageBufferHandle_t buf, const char *code, const char *message);
  /// Static trampoline for esp_http_client events. Dispatches response headers
  /// to on_http_header_().
  static esp_err_t http_event_handler_(esp_http_client_event_t *event);

  // --- VAD / recording ---
  /// Pulls audio from the ring buffer into the recording buffer and updates
  /// the RMS-based VAD. One bounded chunk per call.
  void drain_ring_buffer_to_recording_();
  /// RMS amplitude of a block of int16 samples, relative to full-scale (0..1).
  static float compute_rms_(const int16_t *samples, size_t count);

  // --- SSE processing ---
  /// Feeds bytes into sse_line_buffer_ and, on each complete '\n'-terminated
  /// line, calls process_sse_line_().
  void process_sse_bytes_(const uint8_t *data, size_t len);

 protected:
  // --- Virtual hooks (implemented by derived classes) ---
  /// Fill `url` (capacity buf_size) with the request URL and set
  /// `content_type`. Return false if no valid request target is set (aborts
  /// the HTTP task before opening).
  virtual bool build_http_url_and_content_type_(char *url, size_t buf_size,
                                                const char *&content_type) const = 0;
  /// Set request-specific headers (Authorization, Mcp-Session-Id, Accept, …).
  /// Called after Content-Type is set.
  virtual void set_http_extra_headers_(esp_http_client_handle_t client) = 0;
  /// Return true if the HTTP status code is acceptable for this target
  /// (e.g. 200, or 202 for MCP notifications).
  virtual bool is_http_status_acceptable_(int status) const = 0;
  /// Return true if the response body should be fed directly to the speaker
  /// (non-streaming TTS path) instead of being sent as DATA messages.
  virtual bool http_feeds_speaker_() const = 0;
  /// Feed the HTTP response body directly to the speaker (TTS path). Runs on
  /// the HTTP task; reads until EOF or http_task_should_exit_.
  virtual void http_feed_speaker_(esp_http_client_handle_t client) = 0;
  /// Handle one complete SSE line (`data:` or `event:` payload). Called by
  /// process_sse_bytes_() for each reassembled line.
  virtual void process_sse_line_(const char *line, size_t len) = 0;
  /// Called for each response header captured during the HTTP event callback.
  /// Default implementation is a no-op.
  virtual void on_http_header_(const char *key, const char *value) {}

  // --- HTTP task ---
  StaticTask http_task_;
  static constexpr uint32_t HTTP_TASK_STACK_SIZE = 8192;
  static constexpr UBaseType_t HTTP_TASK_PRIORITY = 3;
  static constexpr size_t HTTP_MSG_BUFFER_SIZE = 8192;
  MessageBufferHandle_t http_msg_buffer_{nullptr};
  volatile bool http_task_should_exit_{false};
  static constexpr size_t HTTP_TASK_READ_CHUNK = 2048;
  static constexpr size_t HTTP_WRITE_CHUNK = 4096;
  static constexpr int HTTP_TIMEOUT_MS = 60000;

  /// The active esp_http_client (owned by the HTTP task during a request).
  esp_http_client_handle_t http_client_{nullptr};

  // --- Mic ring buffer (mic callback → main loop) ---
  std::unique_ptr<ring_buffer::RingBuffer> ring_buffer_;
  static constexpr size_t RING_BUFFER_SIZE = 16384;

  // --- Recording buffer (contiguous PSRAM WAV PCM) ---
  // 16 kHz / 16-bit / mono = 32 bytes per millisecond.
  static constexpr uint32_t MIC_BYTES_PER_MS = 32;
  uint8_t *recording_buffer_{nullptr};
  size_t recording_capacity_{0};
  size_t recording_size_{0};

  // --- Request body (PSRAM, built by derived class, drained by HTTP task) ---
  uint8_t *request_body_{nullptr};
  size_t request_body_capacity_{0};
  size_t request_body_size_{0};
  size_t request_body_sent_{0};

  // --- SSE line reassembly buffer ---
  static constexpr size_t SSE_LINE_MAX = 32768;
  char *sse_line_buffer_{nullptr};
  size_t sse_line_len_{0};
  std::string sse_event_type_;

  // --- VAD state (energy-based) ---
  uint32_t vad_last_check_ms_{0};
  uint32_t silence_since_ms_{0};   // start of the current silent run
  uint32_t speech_onset_ms_{0};     // start of the current above-threshold run
  bool speech_active_{false};       // true once speech onset has been confirmed
  bool speech_ended_{false};        // set when end-of-speech is detected
  uint32_t listening_start_ms_{0};
};

}  // namespace esphome::openai_common

#endif  // USE_OPENAI_COMMON
