#include "openai_responses.h"

#ifdef USE_OPENAI_RESPONSES

#include "esphome/core/defines.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/core/util.h"

#include "esphome/components/json/json_util.h"

#ifdef USE_OPENAI_RESPONSES_TOOLS
#include "openai_responses_tools.h"  // generated header with TOOLS_JSON (raw string literal)
#endif
#ifdef USE_OPENAI_RESPONSES_MCP
#include "mcp_client.h"
#endif

#include <esp_crt_bundle.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace esphome::openai_responses {

static const char *const TAG = "openai_responses";

#ifdef USE_PSRAM
// --- PSRAM-backed ArduinoJson allocator --------------------------------------
// ArduinoJson's JsonDocument allocates its internal buffer via the Allocator
// interface. The default SpiRamAllocator in json_util.h uses RAMAllocator with
// no flags (ALLOC_INTERNAL | ALLOC_EXTERNAL), which prefers internal RAM.
// We force ALLOC_EXTERNAL so the JSON document buffer lives in PSRAM, keeping
// the internal heap free for the smaller allocations that can't use PSRAM
// (FreeRTOS task stacks, lwIP socket buffers, etc.). This significantly
// reduces internal heap fragmentation during MCP tool calls, where the
// zim_query tool's deeply-nested schema can allocate 10KB+ of JSON document.
struct PsRamJsonAllocator : ArduinoJson::Allocator {
  void *allocate(size_t size) override {
    esphome::RAMAllocator<uint8_t> allocator(esphome::RAMAllocator<uint8_t>::ALLOC_EXTERNAL);
    return allocator.allocate(size);
  }
  void deallocate(void *ptr) override {
    // ESP-IDF's free() tracks the heap region and routes to the correct pool.
    free(ptr);  // NOLINT
  }
  void *reallocate(void *ptr, size_t new_size) override {
    esphome::RAMAllocator<uint8_t> allocator(esphome::RAMAllocator<uint8_t>::ALLOC_EXTERNAL);
    return allocator.reallocate(static_cast<uint8_t *>(ptr), new_size);
  }
};
#endif

// --- Constants -------------------------------------------------------------

// Mic sample rate / format is fixed: 16 kHz, 16-bit, mono (validated by the
// FINAL_VALIDATE_SCHEMA in Python). The WAV header written for the recording
// uses these values.
static constexpr uint32_t MIC_SAMPLE_RATE = 16000;
static constexpr uint8_t MIC_BITS_PER_SAMPLE = 16;
static constexpr uint8_t MIC_CHANNELS = 1;
// 16 kHz * 2 bytes * 1 channel = 32 bytes per millisecond.
static constexpr uint32_t MIC_BYTES_PER_MS = (MIC_SAMPLE_RATE * MIC_BITS_PER_SAMPLE * MIC_CHANNELS) / (8 * 1000);

// HTTP read/write chunk sizes. Kept small so one loop() pass does a bounded
// amount of work and never trips the main-loop watchdog.
static constexpr size_t HTTP_WRITE_CHUNK = 4096;
static constexpr size_t HTTP_READ_CHUNK = 2048;
// SSE line buffer. The Responses API's response.created and response.completed
// events contain the full response object (including the tools array, which
// can be 10KB+), so this must be much larger than the Chat Completions delta
// chunks (which are tiny). 32KB is in PSRAM and handles most tool schemas.
static constexpr size_t SSE_LINE_MAX = 32768;

// HTTP timeouts. LLM + tool execution can be slow, so be generous.
static constexpr int HTTP_TIMEOUT_MS = 60000;

// VAD is checked at most once per this interval to bound CPU in loop().
static constexpr uint32_t VAD_CHECK_INTERVAL_MS = 50;
// Speech must be sustained above the threshold for this long before we commit
// to RECORDING (filters out transient clicks).
static constexpr uint32_t SPEECH_ONSET_MS = 60;

// --- Base64 (streaming, in-place into a caller buffer) --------------------

/// Encode `len` bytes from `data` into base64, writing into `out` (capacity
/// `out_cap`). Returns the number of base64 characters written (no NUL
/// terminator), or 0 if the buffer is too small. Writes directly into a PSRAM
/// buffer so we avoid the heap-allocating base64_encode() helper.
static size_t base64_encode_to(const uint8_t *data, size_t len, char *out, size_t out_cap) {
  static const char *const TABLE = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  // 3 bytes -> 4 chars, rounded up.
  size_t needed = ((len + 2) / 3) * 4;
  if (needed > out_cap) {
    return 0;
  }
  size_t j = 0;
  size_t i = 0;
  while (i + 3 <= len) {
    uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | uint32_t(data[i + 2]);
    out[j++] = TABLE[(v >> 18) & 0x3f];
    out[j++] = TABLE[(v >> 12) & 0x3f];
    out[j++] = TABLE[(v >> 6) & 0x3f];
    out[j++] = TABLE[v & 0x3f];
    i += 3;
  }
  size_t rem = len - i;
  if (rem == 1) {
    uint32_t v = uint32_t(data[i]) << 16;
    out[j++] = TABLE[(v >> 18) & 0x3f];
    out[j++] = TABLE[(v >> 12) & 0x3f];
    out[j++] = '=';
    out[j++] = '=';
  } else if (rem == 2) {
    uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
    out[j++] = TABLE[(v >> 18) & 0x3f];
    out[j++] = TABLE[(v >> 12) & 0x3f];
    out[j++] = TABLE[(v >> 6) & 0x3f];
    out[j++] = '=';
  }
  return j;
}

/// Detects tool-call text leaks — when the model emits tool calls as text
/// instead of structured function_call items. Multiple variants observed:
///   <|tool_call>call:...<tool_call|>   (with underscores)
///   <|toolcall>call:...<toolcall|>     (no underscore)
///   <<tool_call>` (XML-style)
/// Checks for all known opening/closing tag patterns.
static bool is_tool_call_leak_(const std::string &text) {
  // Opening tags: <|tool_call>, <|toolcall>
  if (text.find("<|tool") != std::string::npos) {
    return true;
  }
  // Closing tags: <tool_call|>, <toolcall|>
  if (text.find("tool_call|>") != std::string::npos ||
      text.find("toolcall|>") != std::string::npos) {
    return true;
  }
  // XML-style: <<tool_call>
  if (text.find("<tool_call>") != std::string::npos) {
    return true;
  }
  return false;
}

/// Escapes a string for inclusion in a JSON string value (surrounding quotes
/// are NOT added). Drops control characters below 0x20.
static std::string escape_json_string_(const std::string &input) {
  std::string out;
  out.reserve(input.size() + 16);
  for (char c : input) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else if (c == '\n') {
      out.push_back('\\');
      out.push_back('n');
    } else if (c == '\r') {
      out.push_back('\\');
      out.push_back('r');
    } else if (c == '\t') {
      out.push_back('\\');
      out.push_back('t');
    } else if ((unsigned char) c < 0x20) {
      continue;
    } else {
      out.push_back(c);
    }
  }
  return out;
}

// Strip common markdown formatting from text before sending to TTS. Models
// don't always follow "no markdown" instructions, and TTS systems read
// markdown syntax literally (e.g., "hash hash hash one" for "### 1.").
// This removes: headers (###), bold/italic markers (**, *, __, _), bullet
// points (-, *), numbered lists (1.), and extra blank lines.
static std::string strip_markdown_(const std::string &input) {
  std::string out;
  out.reserve(input.size());
  size_t i = 0;
  bool at_line_start = true;
  while (i < input.size()) {
    char c = input[i];

    // Strip markdown headers at line start: #, ##, ###, etc.
    if (at_line_start && c == '#') {
      while (i < input.size() && input[i] == '#') {
        i++;
      }
      // Skip the space after the hashes.
      while (i < input.size() && input[i] == ' ') {
        i++;
      }
      at_line_start = false;
      continue;
    }

    // Strip bullet points at line start: - or * followed by space
    if (at_line_start && (c == '-' || c == '*') && i + 1 < input.size() && input[i + 1] == ' ') {
      i += 2;
      at_line_start = false;
      continue;
    }

    // Strip numbered list at line start: "1. ", "2. ", etc.
    if (at_line_start && c >= '0' && c <= '9') {
      size_t j = i;
      while (j < input.size() && input[j] >= '0' && input[j] <= '9') {
        j++;
      }
      if (j < input.size() && input[j] == '.' && j + 1 < input.size() && input[j + 1] == ' ') {
        i = j + 2;
        at_line_start = false;
        continue;
      }
    }

    // Strip bold/italic markers: **text**, *text*, __text__, _text_
    if (c == '*' || c == '_') {
      // Check for double (**) or single (*)
      if (i + 1 < input.size() && input[i + 1] == c) {
        i += 2;
        continue;
      }
      i++;
      continue;
    }

    // Track line position for at_line_start detection.
    if (c == '\n') {
      at_line_start = true;
      // Collapse multiple blank lines into one.
      if (!out.empty() && out.back() == '\n') {
        i++;
        continue;
      }
    } else if (c != ' ' && c != '\t') {
      at_line_start = false;
    }

    out.push_back(c);
    i++;
  }
  return out;
}
// --- Lifecycle -------------------------------------------------------------

OpenAIResponses::OpenAIResponses() = default;

OpenAIResponses::~OpenAIResponses() {
  // Kill the HTTP task if still running and clean up its resources.
  this->stop_http_task_();
  this->deallocate_buffers_();
  // Free the pre-allocated message buffer (only in full teardown, not per-turn).
  if (this->http_msg_buffer_ != nullptr) {
    vMessageBufferDelete(this->http_msg_buffer_);
    this->http_msg_buffer_ = nullptr;
  }
}

void OpenAIResponses::setup() {
  // Register the microphone data callback. The MicrophoneSource calls this
  // from the microphone's own task/thread, so we write into the ring buffer
  // (thread-safe) and consume it from the main loop. This avoids any locking
  // in the hot mic path and keeps the main loop the single owner of the
  // recording buffer.
  this->mic_source_->add_data_callback([this](const std::vector<uint8_t> &data) {
    if (this->ring_buffer_ != nullptr &&
        (this->state_ == State::LISTENING || this->state_ == State::RECORDING)) {
      this->ring_buffer_->write(data.data(), data.size());
    }
  });

  // Pre-allocate the fixed long-lived buffers (ring, recording, speaker, SSE)
  // once at setup so we don't free ~1MB of PSRAM and reallocate it on every
  // turn — that allocation churn was the main source of the main-loop
  // watchdog warnings. allocate_buffers_() is idempotent (guards each alloc
  // with if (nullptr)). Variable-size turn buffers (request_body_,
  // retained_b64_audio_) are still allocated/freed per turn.
  if (!this->allocate_buffers_()) {
    this->mark_failed();
    return;
  }

  // Pre-allocate the message buffer for HTTP task → loop communication. This
  // stays resident for the component lifetime — creating/deleting 8KB of
  // internal RAM every turn causes heap fragmentation. The buffer is reset
  // (drained) at the start of each turn in start_http_task_().
  this->http_msg_buffer_ = xMessageBufferCreate(HTTP_MSG_BUFFER_SIZE);
  if (this->http_msg_buffer_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create message buffer at setup");
    this->mark_failed();
    return;
  }

  // Streaming TTS: allocate the 2MB PSRAM ring buffer and semaphores.
  // These stay resident for the component lifetime — reused across turns.
  ExternalRAMAllocator<uint8_t> ext(ExternalRAMAllocator<uint8_t>::ALLOC_EXTERNAL);
  this->tts_audio_buffer_ = ext.allocate(TTS_AUDIO_BUFFER_SIZE);
  if (this->tts_audio_buffer_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate %u-byte TTS audio buffer", (unsigned) TTS_AUDIO_BUFFER_SIZE);
    this->mark_failed();
    return;
  }
  this->tts_queue_mutex_ = xSemaphoreCreateMutex();
  this->tts_audio_data_ready_ = xSemaphoreCreateBinary();
  this->tts_audio_space_available_ = xSemaphoreCreateBinary();
  if (this->tts_queue_mutex_ == nullptr || this->tts_audio_data_ready_ == nullptr ||
      this->tts_audio_space_available_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create TTS semaphores at setup");
    this->mark_failed();
    return;
  }
 }

float OpenAIResponses::get_setup_priority() const { return setup_priority::AFTER_CONNECTION; }

void OpenAIResponses::dump_config() {
  ESP_LOGCONFIG(TAG, "OpenAI Responses:");
  ESP_LOGCONFIG(TAG, "  Endpoint: %s", this->endpoint_base_.c_str());
  ESP_LOGCONFIG(TAG, "  Chat model: %s", this->chat_model_.c_str());
  if (this->multimodal_) {
    ESP_LOGCONFIG(TAG, "  Mode: multimodal (audio sent to chat model)");
  } else {
    ESP_LOGCONFIG(TAG, "  Mode: STT + LLM (stt_model=%s)", this->stt_model_.c_str());
  }
  ESP_LOGCONFIG(TAG, "  TTS model: %s voice=%s sample_rate=%" PRIu32,
                this->tts_model_.c_str(), this->tts_voice_.c_str(), this->tts_sample_rate_);
  ESP_LOGCONFIG(TAG, "  Silence threshold: %.4f duration_ms: %" PRIu32 " max_recording_ms: %" PRIu32,
                this->silence_threshold_, this->silence_duration_ms_, this->max_recording_ms_);
  ESP_LOGCONFIG(TAG, "  Volume multiplier: %.2f", this->volume_multiplier_);
  ESP_LOGCONFIG(TAG, "  Tools: %s", this->has_tools_ ? "yes" : "no");
#ifdef USE_OPENAI_RESPONSES_MCP
  ESP_LOGCONFIG(TAG, "  MCP servers: %u", (unsigned) this->mcp_servers_.size());
  for (const auto &srv : this->mcp_servers_) {
    ESP_LOGCONFIG(TAG, "    %s: %s", srv.name.c_str(), srv.url.c_str());
  }
  ESP_LOGCONFIG(TAG, "  Tools cache TTL: %" PRIu32 " ms", this->tools_cache_ttl_ms_);
#endif
}

// --- Buffer allocation ------------------------------------------------------

bool OpenAIResponses::allocate_buffers_() {
  // All long-lived buffers live in PSRAM (the component requires psram). Each
  // is allocated once and freed in deallocate_buffers_() on teardown. No heap
  // allocation happens after setup() in steady state.
  ExternalRAMAllocator<uint8_t> ext(ExternalRAMAllocator<uint8_t>::ALLOC_EXTERNAL);

  if (this->ring_buffer_ == nullptr) {
    this->ring_buffer_ = ring_buffer::RingBuffer::create(RING_BUFFER_SIZE);
    if (this->ring_buffer_ == nullptr) {
      ESP_LOGE(TAG, "Failed to allocate ring buffer (%d bytes)", RING_BUFFER_SIZE);
      return false;
    }
  }

  // Recording buffer: worst case max_recording_ms of audio. 16 kHz/16-bit/mono
  // = 32 bytes/ms.
  this->recording_capacity_ = this->max_recording_ms_ * MIC_BYTES_PER_MS;
  if (this->recording_buffer_ == nullptr) {
    this->recording_buffer_ = ext.allocate(this->recording_capacity_);
    if (this->recording_buffer_ == nullptr) {
      ESP_LOGE(TAG, "Failed to allocate recording buffer (%d bytes)", (int) this->recording_capacity_);
      return false;
    }
  }
  this->recording_size_ = 0;

  if (this->speaker_buffer_ == nullptr) {
    this->speaker_buffer_ = ext.allocate(SPEAKER_BUFFER_SIZE);
    if (this->speaker_buffer_ == nullptr) {
      ESP_LOGE(TAG, "Failed to allocate speaker buffer (%d bytes)", SPEAKER_BUFFER_SIZE);
      return false;
    }
  }
  this->speaker_buffer_index_ = 0;

  if (this->sse_line_buffer_ == nullptr) {
    this->sse_line_buffer_ = reinterpret_cast<char *>(ext.allocate(SSE_LINE_MAX));
    if (this->sse_line_buffer_ == nullptr) {
      ESP_LOGE(TAG, "Failed to allocate SSE line buffer (%d bytes)", SSE_LINE_MAX);
      return false;
    }
  }
  this->sse_line_len_ = 0;

  return true;
}

void OpenAIResponses::deallocate_buffers_() {
  ExternalRAMAllocator<uint8_t> ext(ExternalRAMAllocator<uint8_t>::ALLOC_EXTERNAL);

  this->ring_buffer_.reset();

  if (this->recording_buffer_ != nullptr) {
    ext.deallocate(this->recording_buffer_, this->recording_capacity_);
    this->recording_buffer_ = nullptr;
  }
  this->recording_capacity_ = 0;
  this->recording_size_ = 0;

  if (this->request_body_ != nullptr) {
    ext.deallocate(this->request_body_, this->request_body_capacity_);
    this->request_body_ = nullptr;
  }
  this->request_body_capacity_ = 0;
  this->request_body_size_ = 0;
  this->request_body_sent_ = 0;

  if (this->sse_line_buffer_ != nullptr) {
    ext.deallocate(reinterpret_cast<uint8_t *>(this->sse_line_buffer_), SSE_LINE_MAX);
    this->sse_line_buffer_ = nullptr;
  }
  this->sse_line_len_ = 0;

  if (this->speaker_buffer_ != nullptr) {
    ext.deallocate(this->speaker_buffer_, SPEAKER_BUFFER_SIZE);
    this->speaker_buffer_ = nullptr;
  }
  this->speaker_buffer_index_ = 0;

  // Stop streaming TTS tasks if still running.
  this->stop_tts_producer_task_();
  this->stop_speaker_feeder_task_();

  // Free the PSRAM audio buffer (allocated in setup, freed on shutdown).
  if (this->tts_audio_buffer_ != nullptr) {
    ext.deallocate(this->tts_audio_buffer_, TTS_AUDIO_BUFFER_SIZE);
    this->tts_audio_buffer_ = nullptr;
  }
  this->tts_queue_.clear();
  this->tts_pending_text_.clear();

  this->response_text_.clear();
  this->response_text_.shrink_to_fit();
  this->reasoning_text_.clear();
  this->reasoning_text_.shrink_to_fit();
  this->stt_response_text_.clear();
  this->stt_response_text_.shrink_to_fit();
#ifdef USE_OPENAI_RESPONSES_MCP
  ExternalRAMAllocator<char> ext_char(ExternalRAMAllocator<char>::ALLOC_EXTERNAL);
  if (this->retained_b64_audio_ != nullptr) {
    ext_char.deallocate(this->retained_b64_audio_, this->retained_b64_capacity_);
    this->retained_b64_audio_ = nullptr;
  }
  this->retained_b64_len_ = 0;
  this->retained_b64_capacity_ = 0;
  this->turn_messages_.clear();
  this->turn_messages_.shrink_to_fit();
  this->turn_messages_sent_count_ = 0;
  this->mcp_response_text_.clear();
  this->mcp_response_text_.shrink_to_fit();
  this->user_text_.clear();
  this->user_text_.shrink_to_fit();
  this->raw_tools_per_server_.clear();
  this->raw_tools_per_server_.shrink_to_fit();
  this->reset_accumulated_tool_calls_();
  this->had_tool_calls_ = false;
  this->previous_response_id_.clear();
  this->previous_response_id_.shrink_to_fit();
  this->tool_round_ = 0;
  this->current_tool_index_ = 0;
  this->mcp_phase_ = McpPhase::IDLE;
  this->current_mcp_call_type_ = McpCallType::INITIALIZE;
  // Clear runtime MCP connection state (URL, auth token, session ID).
  // These are set fresh before each MCP HTTP call.
  this->current_mcp_url_.clear();
  this->current_mcp_url_.shrink_to_fit();
  this->current_mcp_auth_.clear();
  this->current_mcp_auth_.shrink_to_fit();
  this->current_mcp_session_id_.clear();
  this->current_mcp_session_id_.shrink_to_fit();
  // Reset FETCHING_TOOLS bookkeeping flags.
  this->tools_prefetch_ = false;
  this->tools_reinit_ = false;
  this->reinit_server_index_ = 0;
  this->mcp_route_server_index_ = 0;
  this->round1_b64_offset_ = 0;
  this->round1_b64_len_ = 0;
  // cached_tools_json_, tool_routes_, tools_cache_timestamp_ms_, and
  // mcp_servers_[].session_id/initialized/stateless persist across turns
  // (the cache is refreshed on a TTL basis, not per-turn).
#endif
}

void OpenAIResponses::reset_turn_state_() {
  // Reset turn-scoped indices + free variable-size turn buffers WITHOUT
  // touching the long-lived fixed buffers (ring/recording/speaker/sse),
  // which stay allocated for the component lifetime (allocated once in
  // setup()). This avoids ~1MB of PSRAM alloc/free churn per turn that was
  // the primary cause of the main-loop watchdog warnings.

  // Fixed buffers: reset indices only (do NOT free).
  this->recording_size_ = 0;
  this->sse_line_len_ = 0;
  this->speaker_buffer_index_ = 0;
  this->tts_header_skipped_ = false;
  this->tts_stream_started_ = false;

  // Reset streaming TTS state (the PSRAM buffer itself stays allocated).
  this->tts_pending_text_.clear();
  this->tts_pending_text_.shrink_to_fit();
  this->tts_queue_.clear();
  this->tts_queue_done_ = false;
  this->tts_streaming_active_ = false;
  this->tts_audio_producer_done_ = false;
  this->tts_audio_write_offset_ = 0;
  this->tts_audio_read_offset_ = 0;
  this->tts_task_should_exit_ = false;

  // Variable-size turn buffers: free (these can be ~1MB+ for multimodal
  // base64 and are rebuilt per turn anyway).
  ExternalRAMAllocator<uint8_t> ext(ExternalRAMAllocator<uint8_t>::ALLOC_EXTERNAL);
  if (this->request_body_ != nullptr) {
    ext.deallocate(this->request_body_, this->request_body_capacity_);
    this->request_body_ = nullptr;
  }
  this->request_body_capacity_ = 0;
  this->request_body_size_ = 0;
  this->request_body_sent_ = 0;

  // Clear turn-scoped strings (release capacity back to the heap so it can
  // be reused by other allocations — esp_http_client/lwIP churn the same
  // pools, and shrinking lets those holes get recycled).
  this->response_text_.clear();
  this->response_text_.shrink_to_fit();
  this->reasoning_text_.clear();
  this->reasoning_text_.shrink_to_fit();
  this->stt_response_text_.clear();
  this->stt_response_text_.shrink_to_fit();
#ifdef USE_OPENAI_RESPONSES_MCP
  ExternalRAMAllocator<char> ext_char(ExternalRAMAllocator<char>::ALLOC_EXTERNAL);
  if (this->retained_b64_audio_ != nullptr) {
    ext_char.deallocate(this->retained_b64_audio_, this->retained_b64_capacity_);
    this->retained_b64_audio_ = nullptr;
  }
  this->retained_b64_len_ = 0;
  this->retained_b64_capacity_ = 0;
  this->turn_messages_.clear();
  this->turn_messages_.shrink_to_fit();
  this->turn_messages_sent_count_ = 0;
  this->mcp_response_text_.clear();
  this->mcp_response_text_.shrink_to_fit();
  this->user_text_.clear();
  this->user_text_.shrink_to_fit();
  this->raw_tools_per_server_.clear();
  this->raw_tools_per_server_.shrink_to_fit();
  this->reset_accumulated_tool_calls_();
  this->had_tool_calls_ = false;
  this->previous_response_id_.clear();
  this->previous_response_id_.shrink_to_fit();
  this->tool_round_ = 0;
  this->current_tool_index_ = 0;
  this->mcp_phase_ = McpPhase::IDLE;
  this->current_mcp_call_type_ = McpCallType::INITIALIZE;
  this->current_mcp_url_.clear();
  this->current_mcp_url_.shrink_to_fit();
  this->current_mcp_auth_.clear();
  this->current_mcp_auth_.shrink_to_fit();
  this->current_mcp_session_id_.clear();
  this->current_mcp_session_id_.shrink_to_fit();
  this->tools_prefetch_ = false;
  this->tools_reinit_ = false;
  this->reinit_server_index_ = 0;
  this->mcp_route_server_index_ = 0;
  this->round1_b64_offset_ = 0;
  this->round1_b64_len_ = 0;
  // cached_tools_json_, tool_routes_, tools_cache_timestamp_ms_, and
  // mcp_servers_[].session_id/initialized/stateless persist across turns.
#endif
}

// --- State / mic helpers ----------------------------------------------------

void OpenAIResponses::set_state_(State s) {
  if (this->state_ != s) {
    ESP_LOGD(TAG, "State %u -> %u", (unsigned) this->state_, (unsigned) s);
    this->state_ = s;
  }
}

void OpenAIResponses::start_microphone_() {
  if (this->mic_source_ != nullptr) {
    this->mic_source_->start();
  }
}

void OpenAIResponses::stop_microphone_() {
  if (this->mic_source_ != nullptr) {
    this->mic_source_->stop();
  }
}

float OpenAIResponses::compute_rms_(const int16_t *samples, size_t count) {
  if (count == 0) {
    return 0.0f;
  }
  // Accumulate sum-of-squares in 64-bit to avoid overflow. Normalise to
  // full-scale (1.0 = 32767) so the threshold is a fraction independent of
  // the int16 range.
  uint64_t sum_sq = 0;
  for (size_t i = 0; i < count; i++) {
    int32_t s = samples[i];
    sum_sq += (uint64_t) s * (uint64_t) s;
  }
  // RMS = sqrt(mean(sample^2)); divide by 32767.0 to get a 0..1 fraction.
  float rms = sqrtf((float) sum_sq / (float) count) / 32767.0f;
  return rms;
}

void OpenAIResponses::drain_ring_buffer_to_recording_() {
  if (this->ring_buffer_ == nullptr || this->recording_buffer_ == nullptr) {
    return;
  }
  // Drain whatever the mic callback has written, in receive_acquire() chunks
  // (zero-copy views into the ring buffer's storage), copying into the
  // contiguous recording buffer. We process one acquired chunk per loop pass
  // to bound work; remaining data stays in the ring buffer for next pass.
  size_t length = 0;
  void *item = this->ring_buffer_->receive_acquire(length, HTTP_READ_CHUNK);
  if (item == nullptr || length == 0) {
    return;
  }
  // Append to the recording buffer, clamping to capacity (shouldn't happen
  // before max_recording_ms thanks to the timeout, but guard anyway).
  size_t free = this->recording_capacity_ - this->recording_size_;
  size_t to_copy = (length < free) ? length : free;
  if (to_copy > 0) {
    memcpy(this->recording_buffer_ + this->recording_size_, item, to_copy);
    this->recording_size_ += to_copy;
  }
  this->ring_buffer_->receive_release(item);

  // --- VAD: check RMS at most once per VAD_CHECK_INTERVAL_MS ---
  uint32_t now = millis();
  if (now - this->vad_last_check_ms_ < VAD_CHECK_INTERVAL_MS) {
    return;
  }
  this->vad_last_check_ms_ = now;

  // Compute RMS over the last ~20 ms of audio (320 samples at 16 kHz). We look
  // at the tail of the recording buffer so the VAD reflects the most recent
  // mic input.
  const size_t samples_for_vad = 320;
  size_t bytes_for_vad = samples_for_vad * sizeof(int16_t);
  if (this->recording_size_ < bytes_for_vad) {
    bytes_for_vad = this->recording_size_;
  }
  if (bytes_for_vad == 0) {
    return;
  }
  const int16_t *vad_samples =
      reinterpret_cast<const int16_t *>(this->recording_buffer_ + this->recording_size_ - bytes_for_vad);
  float rms = this->compute_rms_(vad_samples, bytes_for_vad / sizeof(int16_t));

  // Log the current RMS so the user can observe the mic's baseline noise floor
  // and tune silence_threshold accordingly. Throttled to once per VAD check.
  ESP_LOGV(TAG, "VAD rms=%.4f threshold=%.4f active=%d (recording=%u bytes)",
           rms, this->silence_threshold_, this->speech_active_ ? 1 : 0,
           (unsigned) this->recording_size_);

  if (rms > this->silence_threshold_) {
    // Speech above threshold.
    if (!this->speech_active_) {
      // Require sustained speech for SPEECH_ONSET_MS before transitioning.
      if (this->speech_onset_ms_ == 0) {
        this->speech_onset_ms_ = now;
      }
      if (now - this->speech_onset_ms_ >= SPEECH_ONSET_MS) {
        this->speech_active_ = true;
        ESP_LOGD(TAG, "Speech detected (rms=%.4f)", rms);
      }
    }
    // While speech is active, reset the silence timer so the silence window
    // only counts consecutive quiet samples.
    this->silence_since_ms_ = 0;
  } else {
    // Below threshold.
    if (this->speech_active_) {
      if (this->silence_since_ms_ == 0) {
        this->silence_since_ms_ = now;
      } else if (now - this->silence_since_ms_ >= this->silence_duration_ms_) {
        ESP_LOGD(TAG, "Silence for %" PRIu32 " ms, end of speech", now - this->silence_since_ms_);
        this->speech_ended_ = true;
      }
    }
  }
}

// --- Public API ------------------------------------------------------------

void OpenAIResponses::request_start(bool silence_detection) {
  if (this->state_ != State::IDLE) {
    ESP_LOGW(TAG, "Cannot start while not idle (state=%u)", (unsigned) this->state_);
    return;
  }
  ESP_LOGV(TAG, "request_start (silence_detection=%d)", silence_detection ? 1 : 0);

  this->silence_detection_ = silence_detection;
  this->response_text_.clear();
  this->reasoning_text_.clear();
  this->stt_response_text_.clear();
#ifdef USE_OPENAI_RESPONSES_MCP
  this->reset_accumulated_tool_calls_();
  this->had_tool_calls_ = false;
  this->previous_response_id_.clear();
  this->turn_messages_.clear();
  this->turn_messages_sent_count_ = 0;
  this->mcp_response_text_.clear();
  this->user_text_.clear();
  this->current_tool_index_ = 0;
  this->tool_round_ = 0;
  this->mcp_phase_ = McpPhase::IDLE;
  this->current_mcp_call_type_ = McpCallType::INITIALIZE;
  this->current_mcp_url_.clear();
  this->current_mcp_auth_.clear();
  this->current_mcp_session_id_.clear();
  this->tools_prefetch_ = false;
  this->tools_reinit_ = false;
  this->reinit_server_index_ = 0;
  this->mcp_route_server_index_ = 0;
  this->round1_b64_offset_ = 0;
  this->round1_b64_len_ = 0;
#endif
  this->speech_active_ = false;
  this->speech_ended_ = false;
  this->speech_onset_ms_ = 0;
  this->silence_since_ms_ = 0;
  this->request_target_ = RequestTarget::NONE;
  this->tts_stream_started_ = false;
  this->tts_header_skipped_ = false;

  // Pre-build the auth header value so its pointer stays valid for the
  // lifetime of any http client we open (esp_http_client_set_header copies
  // the key but keeps a pointer to the value).
  if (!this->api_key_.empty()) {
    this->auth_header_value_ = "Bearer " + this->api_key_;
  } else {
    this->auth_header_value_.clear();
  }

#ifdef USE_OPENAI_RESPONSES_MCP
  // Check if the tools cache needs refreshing before starting the turn.
  // The refresh runs in loop() (FETCHING_TOOLS state) and calls
  // continue_request_start_() when done. If the cache is valid, proceed directly.
  if (!this->mcp_servers_.empty()) {
    uint32_t now = millis();
    bool stale = (this->tools_cache_timestamp_ms_ == 0) ||
                 ((now - this->tools_cache_timestamp_ms_) >= this->tools_cache_ttl_ms_);
    if (stale) {
      ESP_LOGV(TAG, "Tools cache stale; refreshing before turn starts");
      this->mcp_phase_ = McpPhase::FETCHING_TOOLS;
      this->mcp_route_server_index_ = 0;
      this->tool_routes_.clear();
      this->raw_tools_per_server_.clear();
      this->cached_tools_json_.clear();
      // Determine the first call type: initialize if the first server hasn't
      // been initialized yet (and isn't stateless), tools/list otherwise.
      this->current_mcp_call_type_ =
          (this->mcp_servers_[0].initialized || this->mcp_servers_[0].stateless)
              ? McpCallType::TOOLS_LIST
              : McpCallType::INITIALIZE;
      this->current_mcp_session_id_.clear();
      this->tools_prefetch_ = false;
      this->set_state_(State::FETCHING_TOOLS);
      return;  // loop() handles the fetch; it calls continue_request_start_()
    }
  }
#endif

  // Defer continue_request_start_() into loop() via STARTING_TURN. This
  // avoids running the on_start callback + MWW stop + buffer reset inside
  // MWW's wake-word trigger (which fires from MWW's loop() and inflates its
  // operation time past the watchdog threshold).
  this->set_state_(State::STARTING_TURN);
}

void OpenAIResponses::continue_request_start_() {
  // Stop micro_wake_word: it owns the mic while detecting wake words, and we
  // need exclusive access to the mic to record. MWW.stop_after_detection may
  // already have stopped it; this is defensive (handles stop_after_detection=false).
  // NOTE: on_start / on_wake_word_detected callbacks are deferred to after the
  // mic starts (see STARTING_MICROPHONE case in loop()) so the display update
  // (which can take ~580ms for a full 320x240 framebuffer redraw) doesn't block
  // the time-critical wake→listen path.
#ifdef USE_MICRO_WAKE_WORD
  if (this->micro_wake_word_ != nullptr && this->micro_wake_word_->is_running()) {
    this->micro_wake_word_->stop();
  }
#endif

  if (!this->allocate_buffers_()) {
    this->fail_("alloc_failed", "Could not allocate PSRAM buffers");
    return;
  }

  this->listening_start_ms_ = millis();
  this->set_state_(State::STARTING_MICROPHONE);
}

#ifdef USE_OPENAI_RESPONSES_MCP
void OpenAIResponses::prefetch_tools() {
  if (this->state_ != State::IDLE) {
    ESP_LOGV(TAG, "prefetch_tools: skipping (not idle, state=%u)", (unsigned) this->state_);
    return;
  }
  if (this->mcp_servers_.empty()) {
    return;
  }
  uint32_t now = millis();
  if (this->tools_cache_timestamp_ms_ != 0 &&
      (now - this->tools_cache_timestamp_ms_) < this->tools_cache_ttl_ms_) {
    ESP_LOGV(TAG, "prefetch_tools: cache already valid");
    return;
  }
  ESP_LOGV(TAG, "prefetch_tools: refreshing tools cache");
  this->mcp_phase_ = McpPhase::FETCHING_TOOLS;
  this->mcp_route_server_index_ = 0;
  this->tool_routes_.clear();
  this->raw_tools_per_server_.clear();
  this->cached_tools_json_.clear();
  this->current_mcp_call_type_ =
      (this->mcp_servers_[0].initialized || this->mcp_servers_[0].stateless)
          ? McpCallType::TOOLS_LIST
          : McpCallType::INITIALIZE;
  this->current_mcp_session_id_.clear();
  this->tools_prefetch_ = true;
  this->set_state_(State::FETCHING_TOOLS);
}

#endif

void OpenAIResponses::prewarm_models_task_(void *arg) {
  auto *self = static_cast<OpenAIResponses *>(arg);
  // Build auth header before any HTTP calls.
  std::string auth;
  if (!self->api_key_.empty()) {
    auth = "Bearer " + self->api_key_;
  }
  const std::string *models[] = {&self->chat_model_, &self->stt_model_, &self->tts_model_};
  for (int m = 0; m < 3; m++) {
    if (models[m]->empty()) {
      continue;
    }
    std::string body = "{\"model\":\"" + *models[m] + "\"}";
    char url[512];
    snprintf(url, sizeof(url), "%s/backend/load", self->endpoint_base_.c_str());
    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 10000;
    config.disable_auto_redirect = false;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
      ESP_LOGW(TAG, "prewarm: init failed for %s", models[m]->c_str());
      continue;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (!auth.empty()) {
      esp_http_client_set_header(client, "Authorization", auth.c_str());
    }
    esp_err_t err = esp_http_client_open(client, body.size());
    if (err == ESP_OK) {
      esp_http_client_write(client, body.c_str(), body.size());
      esp_http_client_fetch_headers(client);
      int status = esp_http_client_get_status_code(client);
      if (status == 200 || status == 201 || status == 204) {
        ESP_LOGI(TAG, "prewarm: %s loaded (HTTP %d)", models[m]->c_str(), status);
      } else {
        ESP_LOGW(TAG, "prewarm: %s returned HTTP %d", models[m]->c_str(), status);
      }
    } else {
      ESP_LOGW(TAG, "prewarm: %s open failed: %s", models[m]->c_str(), esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
  }
  vTaskDelete(nullptr);  // self-terminate
}

void OpenAIResponses::prewarm_models() {
  // Spawn a short-lived FreeRTOS task so the synchronous HTTP calls don't
  // block the main loop. The task self-terminates after all models are loaded.
  // Uses xTaskCreate (dynamic allocation) since this is a one-shot boot task.
  xTaskCreate(prewarm_models_task_, "oai_prewarm", HTTP_TASK_STACK_SIZE, this,
              HTTP_TASK_PRIORITY, nullptr);
}

void OpenAIResponses::request_stop() {
  if (this->state_ == State::IDLE) {
    return;
  }
  ESP_LOGI(TAG, "Stop requested (state=%u)", (unsigned) this->state_);
  // Immediately stop the speaker so audio doesn't keep playing while we wait
  // for the loop to process the ERROR_TEARDOWN state. The HTTP task (if
  // running) is killed by ERROR_TEARDOWN on the next loop pass.
  if (this->speaker_ != nullptr && !this->speaker_->is_stopped()) {
    this->speaker_->stop();
  }
  // Cancel streaming TTS tasks if active.
  if (this->tts_streaming_active_) {
    this->tts_task_should_exit_ = true;
    if (this->tts_queue_mutex_ != nullptr) {
      xSemaphoreTake(this->tts_queue_mutex_, portMAX_DELAY);
      this->tts_queue_.clear();
      this->tts_queue_done_ = true;
      xSemaphoreGive(this->tts_queue_mutex_);
    }
    if (this->tts_producer_task_.is_created()) {
      xTaskNotifyGive(this->tts_producer_task_.get_handle());
    }
    xSemaphoreGive(this->tts_audio_data_ready_);
    xSemaphoreGive(this->tts_audio_space_available_);
    this->tts_audio_write_offset_ = 0;
    this->tts_audio_read_offset_ = 0;
    this->tts_audio_producer_done_ = false;
  }
  // An explicit stop is not an error: teardown without firing on_error.
  this->set_state_(State::ERROR_TEARDOWN);
}

void OpenAIResponses::fail_(const std::string &code, const std::string &message) {
  ESP_LOGE(TAG, "Error: %s - %s", code.c_str(), message.c_str());
  this->on_error_cb_.call(code, message);
  this->set_state_(State::ERROR_TEARDOWN);
}

void OpenAIResponses::teardown_to_idle_() {
  // Kill the HTTP task if still running and clean up its resources. This is
  // a safety net — normally the task has already been stopped by the
  // READING_* state that received DONE/ERROR, or by ERROR_TEARDOWN.
  this->stop_http_task_();

  // Stop the speaker if it is running, then wait for it to fully stop before
  // restarting the mic/wake word (shared i2s bus). The actual wait happens in
  // the DRAINING_AUDIO / ERROR_TEARDOWN loop states; teardown_to_idle_() is
  // only called once the speaker has stopped.
  if (this->speaker_ != nullptr && !this->speaker_->is_stopped()) {
    this->speaker_->stop();
    return;  // caller will re-enter teardown once stopped
  }

  // Make sure the mic is stopped. Normally the mic is already stopped by the
  // time we get here (ERROR_TEARDOWN waits for it, STOPPING_MICROPHONE waits
  // for it, DRAINING_AUDIO is after TTS so the mic was stopped earlier).
  // This is a safety net only — if the mic IS still running here, we call
  // stop() but don't wait (no multi-pass wait in teardown_to_idle_). The
  // caller is responsible for ensuring the mic has stopped before calling.
  if (this->mic_source_ != nullptr && this->mic_source_->is_running()) {
    this->stop_microphone_();
  }

  // Reset turn-scoped state only (indices + variable-size buffers). The
  // fixed long-lived buffers stay allocated for the component lifetime to
  // avoid PSRAM churn (allocated once in setup()).
  this->reset_turn_state_();

  this->on_end_cb_.call();

  // Restart micro_wake_word so the device is ready for the next wake. The
  // speaker has stopped by now so there is no i2s bus contention with the
  // mic that MWW will start.
#ifdef USE_MICRO_WAKE_WORD
  if (this->micro_wake_word_ != nullptr && !this->micro_wake_word_->is_running()) {
    this->micro_wake_word_->start();
  }
#endif

  this->on_idle_cb_.call();
  this->set_state_(State::IDLE);
}

// --- HTTP task (all blocking esp_http_client calls run here) ---------------

esp_err_t OpenAIResponses::http_event_handler_(esp_http_client_event_t *event) {
  // We drive all reads explicitly via esp_http_client_read() in the task, so
  // the only event we handle here is HTTP_EVENT_ON_HEADER — used to capture
  // the Mcp-Session-Id response header from the `initialize` handshake.
#ifdef USE_OPENAI_RESPONSES_MCP
  if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key != nullptr &&
      event->header_value != nullptr) {
    if (strcasecmp(event->header_key, "Mcp-Session-Id") == 0) {
      auto *self = static_cast<OpenAIResponses *>(event->user_data);
      if (self != nullptr) {
        self->current_mcp_session_id_ = event->header_value;
        ESP_LOGV(TAG, "Captured Mcp-Session-Id: %s", self->current_mcp_session_id_.c_str());
      }
    }
  }
#endif
  return ESP_OK;
}

void OpenAIResponses::start_http_task_() {
  this->http_task_should_exit_ = false;

  // The message buffer is pre-allocated in setup() and reused across turns.
  // Drain any stale data from a previous turn before starting.
  if (this->http_msg_buffer_ != nullptr) {
    uint8_t drain[64];
    while (xMessageBufferReceive(this->http_msg_buffer_, drain, sizeof(drain), 0) > 0) {
      // discard
    }
  } else {
    // Should not happen (allocated in setup), but handle gracefully.
    this->fail_("msg_buffer_alloc", "Message buffer is null");
    return;
  }

  // Create the HTTP task with an internal-RAM stack. PSRAM stacks crash
  // during interrupt-heavy network I/O (lwip/socket callbacks preempt the
  // task and the cache may not have the PSRAM stack lines, causing
  // IllegalInstruction). 8KB of internal RAM is affordable on the S3-Box-3.
  // The StaticTask::destroy() called in stop_http_task_() keeps the stack
  // buffer allocated for reuse by the next create() call.
  if (!this->http_task_.create(OpenAIResponses::http_task_fn_, "oai_http",
                               HTTP_TASK_STACK_SIZE, this, HTTP_TASK_PRIORITY, false)) {
    this->fail_("task_alloc", "Failed to create HTTP task");
    return;
  }
}

bool OpenAIResponses::stop_http_task_() {
  // Signal the task to exit. The task checks http_task_should_exit_ in its
  // read/write loops and, when it sees the flag, calls close_http_() itself
  // (cleaning up the esp_http_client + TLS socket from the task that owns
  // them) before reaching vTaskSuspend(nullptr) at the end of http_task_fn_.
  //
  // This function is NON-BLOCKING. If the task hasn't suspended yet, it
  // returns false — ERROR_TEARDOWN retries on the next loop pass. This
  // avoids blocking the main loop (watchdog) and avoids force-deleting the
  // task while it's blocked in lwIP recv_tcp() (which corrupts lwIP state
  // and crashes the tcpip_thread with an Interrupt WDT timeout).
  //
  // During active SSE streaming, the task exits within one read-chunk
  // interval (~10-50ms). If there's a gap between deltas, it takes longer
  // but ERROR_TEARDOWN keeps retrying until the task exits.
  this->http_task_should_exit_ = true;

  if (!this->http_task_.is_created()) {
    this->http_task_should_exit_ = false;
    return true;  // nothing to stop
  }

  // Drain the message buffer so the task's xMessageBufferSend (if blocked on
  // a full buffer) unblocks and can check the exit flag.
  if (this->http_msg_buffer_ != nullptr) {
    uint8_t drain[64];
    while (xMessageBufferReceive(this->http_msg_buffer_, drain, sizeof(drain), 0) > 0) {
      // discard
    }
  }

  // Non-blocking check: is the task suspended?
  if (eTaskGetState(this->http_task_.get_handle()) != eSuspended) {
    return false;  // not yet — ERROR_TEARDOWN will retry on the next loop pass
  }

  // Task suspended cleanly. It already called close_http_() itself, so
  // http_client_ is null. Safe to destroy (not mid-syscall).
  this->http_task_.destroy();
  this->http_task_should_exit_ = false;
  // close_http_() is a no-op if http_client_ is already null.
  this->close_http_();

  // Drain any messages the task sent before suspending (DONE/ERROR/etc).
  if (this->http_msg_buffer_ != nullptr) {
    uint8_t drain[64];
    while (xMessageBufferReceive(this->http_msg_buffer_, drain, sizeof(drain), 0) > 0) {
      // discard
    }
  }
  return true;
}

void OpenAIResponses::http_task_fn_(void *arg) {
  // This task runs on its own FreeRTOS task, separate from the main loop.
  // It performs the full HTTP request lifecycle:
  //   1. Build URL + content type from request_target_
  //   2. esp_http_client_init (allocates client)
  //   3. esp_http_client_open (DNS + TCP + TLS handshake — blocking)
  //   4. esp_http_client_write loop (sends request body — blocking per chunk)
  //   5. esp_http_client_fetch_headers (waits for response start — blocking,
  //      this is the ~1.3s "time to first token" for LLM endpoints)
  //   6. esp_http_client_read loop (reads response chunks — blocking)
  //      Each chunk is sent to the main loop via the message buffer.
  //   7. On EOF: send DONE. On error: send ERROR. On cancel: just exit.
  //   8. esp_http_client_cleanup (closes sockets, frees client)
  //
  // The main loop never blocks on HTTP I/O — it does 0-timeout
  // xMessageBufferReceive in the READING_* states.
  OpenAIResponses *self = static_cast<OpenAIResponses *>(arg);

  // 1) Build URL and content type from the current request target.
  //    Use a C-style char buffer (not std::string) to avoid heap allocation
  //    in the task — the std::string destructor crashed at function return
  //    due to heap corruption from the esp_http_client call chain.
  //
  // The entire body is wrapped in a single scope so that the `task_done:`
  // label is outside all local variable scopes. Otherwise `goto task_done`
  // would cross initializations, which is a compile error in C++.
  {
  char url[512];
  const char *content_type = "application/json";
  if (self->request_target_ == RequestTarget::CHAT) {
    snprintf(url, sizeof(url), "%s/v1/responses", self->endpoint_base_.c_str());
    content_type = "application/json";
  } else if (self->request_target_ == RequestTarget::STT) {
    snprintf(url, sizeof(url), "%s/v1/audio/transcriptions", self->endpoint_base_.c_str());
    content_type = "multipart/form-data; boundary=----esphome_openai_conv";
  } else if (self->request_target_ == RequestTarget::TTS) {
    snprintf(url, sizeof(url), "%s/v1/audio/speech", self->endpoint_base_.c_str());
    content_type = "application/json";
#ifdef USE_OPENAI_RESPONSES_MCP
  } else if (self->request_target_ == RequestTarget::MCP_CALL) {
    snprintf(url, sizeof(url), "%s", self->current_mcp_url_.c_str());
    content_type = "application/json";
#endif
  } else {
    // No valid target — shouldn't happen, but handle gracefully.
    goto task_done;
  }

  ESP_LOGV(TAG, "HTTP POST %s (content_length=%u)", url, (unsigned) self->request_body_size_);

  // 2) Configure and init the HTTP client. The url char buffer lives on the
  //    task stack and is valid for the entire function — the client is
  //    created and destroyed within this function.
  esp_http_client_config_t config = {};
  config.url = url;
  config.cert_pem = nullptr;
  config.disable_auto_redirect = false;
  config.max_redirection_count = 5;
  config.event_handler = OpenAIResponses::http_event_handler_;
  config.user_data = self;  // passed to event_handler via event->user_data
  config.buffer_size = HTTP_TASK_READ_CHUNK * 2;
  config.buffer_size_tx = HTTP_WRITE_CHUNK * 2;
  config.timeout_ms = HTTP_TIMEOUT_MS;

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
  if (strstr(url, "https:") != nullptr) {
    config.crt_bundle_attach = esp_crt_bundle_attach;
  }
#endif

  self->http_client_ = esp_http_client_init(&config);
  if (self->http_client_ == nullptr) {
    ESP_LOGE(TAG, "esp_http_client_init failed");
    self->send_http_error_(self->http_msg_buffer_, "http_init", "esp_http_client_init failed");
    goto task_done;
  }

  esp_http_client_set_method(self->http_client_, HTTP_METHOD_POST);
  esp_http_client_set_header(self->http_client_, "Content-Type", content_type);
#ifdef USE_OPENAI_RESPONSES_MCP
  if (self->request_target_ == RequestTarget::MCP_CALL) {
    // MCP servers may return either a single JSON body or an SSE stream.
    esp_http_client_set_header(self->http_client_, "Accept",
                               "application/json, text/event-stream");
    if (!self->current_mcp_auth_.empty()) {
      esp_http_client_set_header(self->http_client_, "Authorization",
                                 self->current_mcp_auth_.c_str());
    }
    // Include the session ID on all calls after `initialize`.
    if (!self->current_mcp_session_id_.empty()) {
      esp_http_client_set_header(self->http_client_, "Mcp-Session-Id",
                                 self->current_mcp_session_id_.c_str());
    }
  } else {
#endif
    if (!self->auth_header_value_.empty()) {
      esp_http_client_set_header(self->http_client_, "Authorization",
                                 self->auth_header_value_.c_str());
    }
#ifdef USE_OPENAI_RESPONSES_MCP
  }
#endif

  // 3) Open the connection with the exact content length (Content-Length
  //    header is set automatically by open() when write_len > 0).
  esp_err_t err = esp_http_client_open(self->http_client_, self->request_body_size_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_http_client_open failed: %s", esp_err_to_name(err));
    self->close_http_();
    self->send_http_error_(self->http_msg_buffer_, "http_open", esp_err_to_name(err));
    goto task_done;
  }

  // 4) Write the request body in chunks. Each write may block on the socket
  //    send buffer.
  size_t sent = 0;
  while (sent < self->request_body_size_ && !self->http_task_should_exit_) {
    size_t remaining = self->request_body_size_ - sent;
    size_t to_write = (remaining < HTTP_WRITE_CHUNK) ? remaining : HTTP_WRITE_CHUNK;
    int written = esp_http_client_write(self->http_client_,
                                        reinterpret_cast<const char *>(self->request_body_ + sent), to_write);
    if (written < 0) {
      ESP_LOGE(TAG, "esp_http_client_write failed");
      self->close_http_();
      self->send_http_error_(self->http_msg_buffer_, "http_write", "Failed to write request body");
      goto task_done;
    }
    sent += written;
  }

  // Check if we were cancelled during the write loop.
  if (self->http_task_should_exit_) {
    self->close_http_();
    goto task_done;  // no message sent; the loop called stop_http_task_
  }

  // 5) Fetch response headers. This is the biggest blocking call — it waits
  //    for the server to begin responding. For LLM endpoints this is the
  //    "time to first token" (observed ~1.3s for a local Gemma model).
  int64_t header_len = esp_http_client_fetch_headers(self->http_client_);
  if (header_len < 0) {
    ESP_LOGE(TAG, "esp_http_client_fetch_headers failed");
    self->close_http_();
    self->send_http_error_(self->http_msg_buffer_, "http_headers", "Failed to fetch response headers");
    goto task_done;
  }

  // 5b) For MCP calls, the Mcp-Session-Id response header is captured by the
  //     HTTP_EVENT_ON_HEADER event handler (set during `initialize`).
  //     No action needed here.

  // 6) Check the HTTP status code. Accept 200 for normal responses and 202
  //    for MCP notifications (which have no JSON-RPC response body).
  int status = esp_http_client_get_status_code(self->http_client_);
#ifdef USE_OPENAI_RESPONSES_MCP
  bool is_notification = (self->request_target_ == RequestTarget::MCP_CALL &&
                          self->current_mcp_call_type_ == McpCallType::INITIALIZED_NOTIF);
  if (status != 200 && !(is_notification && status == 202)) {
#else
  if (status != 200) {
#endif
    ESP_LOGE(TAG, "HTTP status %d", status);
    self->close_http_();
    char status_msg[32];
    snprintf(status_msg, sizeof(status_msg), "HTTP %d", status);
    self->send_http_error_(self->http_msg_buffer_, "http_status", status_msg);
    goto task_done;
  }

  // 7) Read the response body. For chat/STT, each chunk is sent to the main
  //    loop via the message buffer for SSE/JSON parsing. For TTS, the task
  //    feeds the speaker directly — play(portMAX_DELAY) blocks until the
  //    speaker consumes the data (natural backpressure), and the main loop
  //    never blocks on speaker I/O.
  if (self->request_target_ == RequestTarget::TTS) {
    // --- TTS: feed speaker directly ---
    // play(data, len, portMAX_DELAY) handles the full speaker lifecycle:
    //   - If not started: calls start() internally
    //   - If STARTING: waits portMAX_DELAY for STATE_RUNNING
    //   - If RUNNING: writes to ring buffer (blocks if full = backpressure)
    //   - If STOPPED: returns 0 (data dropped — shouldn't happen)
    bool speaker_started = false;
    size_t header_skip = 44;  // WAV header bytes to skip
    uint8_t read_buf[HTTP_TASK_READ_CHUNK];

    while (!self->http_task_should_exit_) {
      int got = esp_http_client_read(self->http_client_,
                                     reinterpret_cast<char *>(read_buf), HTTP_TASK_READ_CHUNK);
      if (got < 0) {
        ESP_LOGE(TAG, "esp_http_client_read failed (TTS)");
        self->close_http_();
        self->send_http_error_(self->http_msg_buffer_, "http_read", "Failed to read TTS response");
        goto task_done;
      }
      if (got == 0) {
        break;  // EOF
      }

      size_t off = 0;

      // Start the speaker on the first chunk (data is already in hand — no
      // underrun window). Signal the main loop to fire on_tts_stream_start.
      if (!speaker_started) {
        speaker_started = true;
        if (self->speaker_ != nullptr) {
          ESP_LOGV(TAG, "Starting speaker (sample_rate=%u bits=%u ch=%u)",
                   (unsigned) self->tts_sample_rate_, (unsigned) MIC_BITS_PER_SAMPLE, (unsigned) MIC_CHANNELS);
          self->speaker_->set_audio_stream_info(
              audio::AudioStreamInfo(MIC_BITS_PER_SAMPLE, MIC_CHANNELS, self->tts_sample_rate_));
          // Explicitly start the speaker. play() would call start() internally,
          // but only if state_ is not STATE_RUNNING or STATE_STARTING — and our
          // is_stopped() guard would bail before play() ever runs.
          self->speaker_->start();
        }
        uint8_t start_msg = (uint8_t) HttpMsgType::TTS_STREAM_START;
        xMessageBufferSend(self->http_msg_buffer_, &start_msg, 1, portMAX_DELAY);
      }

      // Skip the 44-byte WAV header (may span multiple chunks).
      while (off < (size_t) got && header_skip > 0) {
        size_t to_skip = (header_skip < (size_t) got - off) ? header_skip : ((size_t) got - off);
        off += to_skip;
        header_skip -= to_skip;
      }

        if (off < (size_t) got) {
          size_t pcm_len = (size_t) got - off;
          uint8_t *pcm = read_buf + off;

          // Apply volume multiplier to int16 samples.
        if (self->volume_multiplier_ != 1.0f) {
          size_t num_samples = pcm_len / sizeof(int16_t);
          int16_t *samples = reinterpret_cast<int16_t *>(pcm);
          for (size_t i = 0; i < num_samples; i++) {
            float scaled = (float) samples[i] * self->volume_multiplier_;
            if (scaled > 32767.0f) {
              scaled = 32767.0f;
            } else if (scaled < -32768.0f) {
              scaled = -32768.0f;
            }
            samples[i] = (int16_t) scaled;
          }
        }

        // Feed directly to the speaker. We use play(data, len, 0) — the
        // non-blocking variant — because the 3-arg play() with ticks_to_wait>0
        // calls vTaskDelay(ticks_to_wait) while the speaker is in
        // STATE_STARTING. During that delay the speaker's 100ms buffer
        // underruns and it stops before we ever write any data.
        //
        // Instead: play(data, len, 0) auto-starts the speaker if needed, then
        // tries to write to the ring buffer immediately (0 = non-blocking).
        // If the speaker isn't ready yet (ring buffer not created), it
        // returns 0. We retry after a short 10ms delay — this gives the
        // speaker task time to create its ring buffer without underrunning.
        if (self->speaker_ != nullptr) {
          size_t pcm_off = 0;
          while (pcm_off < pcm_len && !self->http_task_should_exit_) {
            // Only check is_stopped() after play() has been called at least
            // once. Before that, the speaker may still be in STATE_STARTING
            // (not STOPPED), and we need play() to trigger the ring buffer
            // write once STATE_RUNNING is reached.
            if (pcm_off > 0 && self->speaker_->is_stopped()) {
              ESP_LOGW(TAG, "Speaker stopped during TTS playback at byte %u/%u",
                       (unsigned) pcm_off, (unsigned) pcm_len);
              break;
            }
            size_t written = self->speaker_->play(pcm + pcm_off, pcm_len - pcm_off, 0);
            pcm_off += written;
            if (written == 0) {
              // Ring buffer not ready or full. Wait 10ms for the speaker
              // task to create its ring buffer or consume data, then retry.
              vTaskDelay(pdMS_TO_TICKS(10));
            }
          }
        }
      }
    }
  } else {
    // --- Chat/STT: send DATA messages via message buffer ---
    uint8_t send_buf[HTTP_TASK_READ_CHUNK + 1];
    send_buf[0] = (uint8_t) HttpMsgType::DATA;

    while (!self->http_task_should_exit_) {
      int got = esp_http_client_read(self->http_client_,
                                     reinterpret_cast<char *>(send_buf + 1), HTTP_TASK_READ_CHUNK);
      if (got < 0) {
        ESP_LOGE(TAG, "esp_http_client_read failed");
        self->close_http_();
        self->send_http_error_(self->http_msg_buffer_, "http_read", "Failed to read response");
        goto task_done;
      }
      if (got == 0) {
        break;  // EOF
      }
      xMessageBufferSend(self->http_msg_buffer_, send_buf, (size_t) got + 1, portMAX_DELAY);
    }
  }

  // 8) Clean up the HTTP client.
  self->close_http_();

  // 9) Send DONE to signal the loop that the response is complete. If we were
  //    cancelled, skip this — the loop called stop_http_task_ which already
  //    cleaned up.
  if (!self->http_task_should_exit_) {
    uint8_t done = (uint8_t) HttpMsgType::DONE;
    xMessageBufferSend(self->http_msg_buffer_, &done, 1, portMAX_DELAY);
  }

  // Log the stack high-water mark so we can detect if we're close to
  // overflow. This is the minimum free stack (in words) ever seen.
  UBaseType_t high_water = uxTaskGetStackHighWaterMark(nullptr);
  ESP_LOGV(TAG, "HTTP task stack high-water mark: %u words free", (unsigned) high_water);
  }  // end of scope for all locals

task_done:
  // A FreeRTOS task must NEVER return from its entry function — doing so
  // causes an IllegalInstruction crash (the return address is 0x00000000).
  // Suspend ourselves and wait for stop_http_task_() to call vTaskDelete(),
  // which safely deletes a suspended task.
  vTaskSuspend(nullptr);
}

void OpenAIResponses::close_http_() {
  if (this->http_client_ != nullptr) {
    esp_http_client_cleanup(this->http_client_);
    this->http_client_ = nullptr;
  }
}

void OpenAIResponses::send_http_error_(MessageBufferHandle_t buf, const char *code, const char *message) {
  // Pack the error code and message into a single message buffer message:
  //   [ERROR_ type byte] [code_len byte] [code bytes] [msg_len byte] [msg bytes]
  uint8_t code_len = (uint8_t) strlen(code);
  uint8_t msg_len = (uint8_t) strlen(message);
  size_t total = 3 + code_len + msg_len;
  uint8_t err_buf[128];
  if (total > sizeof(err_buf)) {
    // Truncate the message if it doesn't fit.
    msg_len = (uint8_t)(sizeof(err_buf) - 3 - code_len);
    total = 3 + code_len + msg_len;
  }
  err_buf[0] = (uint8_t) HttpMsgType::ERROR_;
  err_buf[1] = code_len;
  memcpy(err_buf + 2, code, code_len);
  err_buf[2 + code_len] = msg_len;
  memcpy(err_buf + 3 + code_len, message, msg_len);
  xMessageBufferSend(buf, err_buf, total, portMAX_DELAY);
}

// --- Request body builders -------------------------------------------------

/// Writes a 44-byte canonical PCM WAV header into `out` (must be >= 44 bytes)
/// for the given PCM data length (bytes), sample rate, bits, channels.
static void write_wav_header_(uint8_t *out, uint32_t data_len, uint32_t sample_rate,
                             uint16_t bits_per_sample, uint16_t channels) {
  uint32_t byte_rate = sample_rate * channels * bits_per_sample / 8;
  uint16_t block_align = channels * bits_per_sample / 8;
  uint32_t chunk_size = 36 + data_len;
  uint32_t fmt_chunk_size = 16;
  uint16_t audio_format = 1;  // PCM

  auto put32 = [&](size_t off, uint32_t v) {
    out[off] = (uint8_t) v;
    out[off + 1] = (uint8_t) (v >> 8);
    out[off + 2] = (uint8_t) (v >> 16);
    out[off + 3] = (uint8_t) (v >> 24);
  };
  auto put16 = [&](size_t off, uint16_t v) {
    out[off] = (uint8_t) v;
    out[off + 1] = (uint8_t) (v >> 8);
  };
  auto putstr = [&](size_t off, const char *s) {
    memcpy(out + off, s, 4);
  };

  putstr(0, "RIFF");
  put32(4, chunk_size);
  putstr(8, "WAVE");
  putstr(12, "fmt ");
  put32(16, fmt_chunk_size);
  put16(20, audio_format);
  put16(22, channels);
  put32(24, sample_rate);
  put32(28, byte_rate);
  put16(32, block_align);
  put16(34, bits_per_sample);
  putstr(36, "data");
  put32(40, data_len);
}

void OpenAIResponses::build_chat_request_body_multimodal_() {
  // Builds the Responses API JSON for Mode 1 (multimodal, round 1). The user
  // input is a message item whose content includes an input_file part with a
  // base64-encoded WAV (header + PCM) sent as a data URL.
  //
  //   {"model":"<chat_model>","stream":true,"store":true,
  //    "instructions":"<system_prompt>",
  //    "input":[{"type":"message","role":"user","content":[
  //      {"type":"input_text","text":""},
  //      {"type":"input_file","file_data":"data:audio/wav;base64,<b64>","filename":"audio.wav"}]}],
  //    "tool_choice":"auto"[,"tools":[...]]}
  //
  // We assemble the body directly into a PSRAM buffer to avoid std::string
  // churn: JSON prefix, then base64 of the WAV (encoded in-place), then JSON
  // suffix.

  // 1) Build the WAV (header + recorded PCM) in the recording buffer region.
  //    We reuse the first 44 bytes of recording_buffer_ as scratch for the
  //    header, but the PCM starts at offset 0 currently. So we shift the PCM
  //    right by 44 bytes to make room for the header. recording_buffer_ has
  //    capacity for max_recording_ms; recording_size_ <= capacity - we need
  //    capacity >= recording_size_ + 44, which holds because the ring drain
  //    stops when recording_size_ hits capacity and we leave margin.
  size_t pcm_len = this->recording_size_;
  if (pcm_len + 44 > this->recording_capacity_) {
    this->fail_("recording_too_large", "Recording exceeds buffer capacity for WAV header");
    return;
  }
  memmove(this->recording_buffer_ + 44, this->recording_buffer_, pcm_len);
  write_wav_header_(this->recording_buffer_, pcm_len, MIC_SAMPLE_RATE, MIC_BITS_PER_SAMPLE, MIC_CHANNELS);
  size_t wav_len = 44 + pcm_len;

  // 2) JSON-escape the system prompt and chat model (small strings).
  std::string escaped_prompt = escape_json_string_(this->system_prompt_);
  std::string escaped_model = escape_json_string_(this->chat_model_);

  // Prefix: everything up to the base64 audio data. The Responses API uses a
  // data URL in input_file.file_data for inline audio (the LocalAI-compatible
  // form). input_text carries an (empty) text part alongside the audio.
  std::string prefix =
      "{\"model\":\"" + escaped_model + "\",\"stream\":true,\"store\":true,"
      "\"instructions\":\"" + escaped_prompt + "\","
      "\"input\":[{\"type\":\"message\",\"role\":\"user\",\"content\":["
      "{\"type\":\"input_text\",\"text\":\"\"},"
      "{\"type\":\"input_file\",\"file_data\":\"data:audio/wav;base64;";

  // Suffix: closes the data URL, the file part, the content array, the input
  // item + input array, then tool_choice and optionally the tools array.
  std::string suffix = "\",\"filename\":\"audio.wav\"}]}],\"tool_choice\":\"auto\"";
#ifdef USE_OPENAI_RESPONSES_MCP
  if (!this->cached_tools_json_.empty()) {
    suffix += ",\"tools\":" + this->cached_tools_json_;
  } else
#endif
#ifdef USE_OPENAI_RESPONSES_TOOLS
  if (this->has_tools_) {
    suffix += ",\"tools\":" + std::string(TOOLS_JSON);
  }
#endif
  suffix += "}";

  size_t b64_len = ((wav_len + 2) / 3) * 4;
  size_t total = prefix.size() + b64_len + suffix.size();

  // 3) Allocate the request body buffer in PSRAM.
  ExternalRAMAllocator<uint8_t> ext(ExternalRAMAllocator<uint8_t>::ALLOC_EXTERNAL);
  if (this->request_body_ != nullptr) {
    ext.deallocate(this->request_body_, this->request_body_capacity_);
  }
  this->request_body_capacity_ = total + 1;
  this->request_body_ = ext.allocate(this->request_body_capacity_);
  if (this->request_body_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate request body (%d bytes)", (int) this->request_body_capacity_);
    this->fail_("alloc_failed", "Could not allocate chat request body");
    return;
  }

  // 4) Copy prefix, encode base64 in-place, copy suffix.
  memcpy(this->request_body_, prefix.data(), prefix.size());
  size_t encoded = base64_encode_to(this->recording_buffer_, wav_len,
                                   reinterpret_cast<char *>(this->request_body_ + prefix.size()),
                                   this->request_body_capacity_ - prefix.size() - suffix.size());
  if (encoded == 0) {
    this->fail_("b64_failed", "Base64 encoding failed");
    return;
  }
  memcpy(this->request_body_ + prefix.size() + encoded, suffix.data(), suffix.size());
  this->request_body_size_ = prefix.size() + encoded + suffix.size();
  this->request_body_sent_ = 0;
#ifdef USE_OPENAI_RESPONSES_MCP
  // Save the base64 offset + length so we can retain it for round 2+ if the
  // LLM requests tool execution (fallback path only — the previous_response_id
  // path doesn't resend the audio).
  this->round1_b64_offset_ = prefix.size();
  this->round1_b64_len_ = encoded;
#endif

  ESP_LOGV(TAG, "Built multimodal chat body: %u bytes (wav=%u b64=%u)",
           (unsigned) this->request_body_size_, (unsigned) wav_len, (unsigned) encoded);
}

void OpenAIResponses::build_chat_request_body_text_(const std::string &user_text) {
  // Mode 2 (round 1): Responses API text-only request.
  //   {"model":"<chat_model>","stream":true,"store":true,
  //    "instructions":"<system_prompt>",
  //    "input":[{"type":"message","role":"user","content":"<transcript>"}],
  //    "tool_choice":"auto"[,"tools":[...]]}
#ifdef USE_OPENAI_RESPONSES_MCP
  this->user_text_ = user_text;  // saved for round 2+ fallback re-inclusion
#endif
  std::string escaped_prompt = escape_json_string_(this->system_prompt_);
  std::string escaped_text = escape_json_string_(user_text);
  std::string body =
      "{\"model\":\"" + this->chat_model_ + "\",\"stream\":true,\"store\":true,"
      "\"instructions\":\"" + escaped_prompt + "\","
      "\"input\":[{\"type\":\"message\",\"role\":\"user\",\"content\":\"" + escaped_text + "\"}],"
      "\"tool_choice\":\"auto\"";
#ifdef USE_OPENAI_RESPONSES_MCP
  if (!this->cached_tools_json_.empty()) {
    body += ",\"tools\":" + this->cached_tools_json_;
  } else
#endif
#ifdef USE_OPENAI_RESPONSES_TOOLS
  if (this->has_tools_) {
    body += ",\"tools\":" + std::string(TOOLS_JSON);
  }
#endif
  body += "}";

  ExternalRAMAllocator<uint8_t> ext(ExternalRAMAllocator<uint8_t>::ALLOC_EXTERNAL);
  if (this->request_body_ != nullptr) {
    ext.deallocate(this->request_body_, this->request_body_capacity_);
  }
  this->request_body_capacity_ = body.size() + 1;
  this->request_body_ = ext.allocate(this->request_body_capacity_);
  if (this->request_body_ == nullptr) {
    this->fail_("alloc_failed", "Could not allocate chat text body");
    return;
  }
  memcpy(this->request_body_, body.data(), body.size());
  this->request_body_size_ = body.size();
  this->request_body_sent_ = 0;
}

void OpenAIResponses::build_stt_multipart_body_() {
  // Builds a multipart/form-data body for POST /v1/audio/transcriptions:
  //   --<boundary>\r\n
  //   Content-Disposition: form-data; name="model"\r\n\r\n
  //   <stt_model>\r\n
  //   --<boundary>\r\n
  //   Content-Disposition: form-data; name="file"; filename="audio.wav"\r\n
  //   Content-Type: audio/wav\r\n\r\n
  //   <WAV header + PCM>\r\n
  //   --<boundary>--\r\n
  // The WAV (header + PCM) is built in the recording buffer (shift PCM right
  // by 44 for the header, same as multimodal).
  size_t pcm_len = this->recording_size_;
  if (pcm_len + 44 > this->recording_capacity_) {
    this->fail_("recording_too_large", "Recording exceeds buffer capacity for WAV header");
    return;
  }
  memmove(this->recording_buffer_ + 44, this->recording_buffer_, pcm_len);
  write_wav_header_(this->recording_buffer_, pcm_len, MIC_SAMPLE_RATE, MIC_BITS_PER_SAMPLE, MIC_CHANNELS);
  size_t wav_len = 44 + pcm_len;

  const char *boundary = "----esphome_openai_conv";
  std::string part_model = std::string("--") + boundary +
                           "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n" + this->stt_model_ + "\r\n";
  std::string part_language;
  if (!this->stt_language_.empty()) {
    part_language = std::string("--") + boundary +
                    "\r\nContent-Disposition: form-data; name=\"language\"\r\n\r\n" + this->stt_language_ + "\r\n";
  }
  std::string part_file_head = std::string("--") + boundary +
                               "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
                               "Content-Type: audio/wav\r\n\r\n";
  std::string closing = std::string("\r\n--") + boundary + "--\r\n";

  size_t total = part_model.size() + part_language.size() + part_file_head.size() + wav_len + closing.size();

  ExternalRAMAllocator<uint8_t> ext(ExternalRAMAllocator<uint8_t>::ALLOC_EXTERNAL);
  if (this->request_body_ != nullptr) {
    ext.deallocate(this->request_body_, this->request_body_capacity_);
  }
  this->request_body_capacity_ = total + 1;
  this->request_body_ = ext.allocate(this->request_body_capacity_);
  if (this->request_body_ == nullptr) {
    this->fail_("alloc_failed", "Could not allocate STT multipart body");
    return;
  }
  size_t off = 0;
  memcpy(this->request_body_ + off, part_model.data(), part_model.size());
  off += part_model.size();
  if (!part_language.empty()) {
    memcpy(this->request_body_ + off, part_language.data(), part_language.size());
    off += part_language.size();
  }
  memcpy(this->request_body_ + off, part_file_head.data(), part_file_head.size());
  off += part_file_head.size();
  memcpy(this->request_body_ + off, this->recording_buffer_, wav_len);
  off += wav_len;
  memcpy(this->request_body_ + off, closing.data(), closing.size());
  off += closing.size();
  this->request_body_size_ = off;
  this->request_body_sent_ = 0;

  ESP_LOGV(TAG, "Built STT multipart body: %u bytes (wav=%u)", (unsigned) this->request_body_size_,
           (unsigned) wav_len);
}

void OpenAIResponses::build_tts_request_body_() {
  this->build_tts_request_body_for_(this->response_text_);
}

void OpenAIResponses::build_tts_request_body_for_(const std::string &text) {
  // POST /v1/audio/speech body:
  //   {"model":"<tts_model>","input":"<text>","voice":"<tts_voice>","response_format":"wav"}
  std::string escaped_input = escape_json_string_(text);
  std::string body =
      "{\"model\":\"" + this->tts_model_ + "\",\"input\":\"" + escaped_input +
      "\",\"voice\":\"" + this->tts_voice_ + "\",\"response_format\":\"wav\"}";

  ExternalRAMAllocator<uint8_t> ext(ExternalRAMAllocator<uint8_t>::ALLOC_EXTERNAL);
  if (this->request_body_ != nullptr) {
    ext.deallocate(this->request_body_, this->request_body_capacity_);
  }
  this->request_body_capacity_ = body.size() + 1;
  this->request_body_ = ext.allocate(this->request_body_capacity_);
  if (this->request_body_ == nullptr) {
    this->fail_("alloc_failed", "Could not allocate TTS body");
    return;
  }
  memcpy(this->request_body_, body.data(), body.size());
  this->request_body_size_ = body.size();
  this->request_body_sent_ = 0;
}

// --- Streaming TTS helpers --------------------------------------------------

size_t OpenAIResponses::find_sentence_boundary_(const std::string &text) {
  if (text.size() < 40) {
    return 0;
  }
  // Scan backward for the last sentence boundary: . ? ! \n
  // For '.', require the next char to be a space + uppercase letter (avoid
  // abbreviations like "Dr. Smith" or decimals like "3.14").
  for (size_t i = text.size() - 1; i > 0; i--) {
    char c = text[i];
    if (c == '\n') {
      return i + 1;
    }
    if (i + 2 < text.size()) {
      if (c == '.' && text[i + 1] == ' ' && text[i + 2] >= 'A' && text[i + 2] <= 'Z') {
        return i + 2;
      }
      if ((c == '?' || c == '!') && text[i + 1] == ' ') {
        return i + 2;
      }
    }
  }
  return 0;
}

void OpenAIResponses::push_tts_sentence_(const std::string &sentence) {
  if (sentence.empty()) {
    return;
  }
  // Start the TTS producer + feeder tasks on the first sentence of a turn.
  if (!this->tts_streaming_active_) {
    this->tts_streaming_active_ = true;
    this->tts_audio_producer_done_ = false;
    this->tts_audio_write_offset_ = 0;
    this->tts_audio_read_offset_ = 0;
    this->start_tts_producer_task_();
    this->start_speaker_feeder_task_();
    // Fire on_tts_start with the partial response text accumulated so far.
    this->on_tts_start_cb_.call(this->response_text_);
  }
  // Push the sentence to the thread-safe queue.
  if (this->tts_queue_mutex_ != nullptr) {
    xSemaphoreTake(this->tts_queue_mutex_, portMAX_DELAY);
    this->tts_queue_.push_back(sentence);
    xSemaphoreGive(this->tts_queue_mutex_);
  }
  // Wake the producer task if it's waiting for a sentence.
  if (this->tts_producer_task_.is_created()) {
    xTaskNotifyGive(this->tts_producer_task_.get_handle());
  }
}

void OpenAIResponses::flush_tts_pending_() {
  if (!this->tts_pending_text_.empty()) {
    std::string text = strip_markdown_(this->tts_pending_text_);
    if (!is_tool_call_leak_(text)) {
      this->push_tts_sentence_(text);
    }
    this->tts_pending_text_.clear();
  }
  this->tts_queue_done_ = true;
  if (this->tts_producer_task_.is_created()) {
    xTaskNotifyGive(this->tts_producer_task_.get_handle());
  }
}

void OpenAIResponses::write_to_audio_buffer_(const uint8_t *data, size_t len) {
  // Write PCM data to the PSRAM ring buffer. Blocks if the buffer is full
  // (natural backpressure — TTS pauses until the speaker catches up).
  size_t off = 0;
  while (off < len) {
    // Check exit flag to avoid deadlock during stop (the feeder may have
    // been stopped, so no one is draining the buffer).
    if (this->tts_task_should_exit_) {
      return;
    }
    size_t w = this->tts_audio_write_offset_.load(std::memory_order_relaxed);
    size_t r = this->tts_audio_read_offset_.load(std::memory_order_relaxed);
    size_t available_space = (r - w - 1 + TTS_AUDIO_BUFFER_SIZE) % TTS_AUDIO_BUFFER_SIZE;
    if (available_space == 0) {
      // Buffer full — wait for the feeder to drain some data.
      xSemaphoreTake(this->tts_audio_space_available_, pdMS_TO_TICKS(100));
      continue;
    }
    size_t to_write = (len - off < available_space) ? (len - off) : available_space;
    // Handle wraparound: write in two parts if needed.
    size_t first_part = (w + to_write > TTS_AUDIO_BUFFER_SIZE) ? (TTS_AUDIO_BUFFER_SIZE - w) : to_write;
    memcpy(this->tts_audio_buffer_ + w, data + off, first_part);
    if (to_write > first_part) {
      memcpy(this->tts_audio_buffer_, data + off + first_part, to_write - first_part);
    }
    this->tts_audio_write_offset_ = (w + to_write) % TTS_AUDIO_BUFFER_SIZE;
    off += to_write;
    // Wake the feeder if it's waiting for data.
    xSemaphoreGive(this->tts_audio_data_ready_);
  }
}

// --- TTS producer task ------------------------------------------------------

void OpenAIResponses::start_tts_producer_task_() {
  this->tts_task_should_exit_ = false;
  if (!this->tts_producer_task_.create(OpenAIResponses::tts_producer_task_fn_, "oai_tts_prod",
                                        TTS_PRODUCER_STACK_SIZE, this, TTS_TASK_PRIORITY, false)) {
    ESP_LOGE(TAG, "Failed to create TTS producer task");
    this->fail_("task_alloc", "Failed to create TTS producer task");
  }
}

void OpenAIResponses::stop_tts_producer_task_() {
  this->tts_task_should_exit_ = true;
  if (this->tts_producer_task_.is_created()) {
    xTaskNotifyGive(this->tts_producer_task_.get_handle());
    xSemaphoreGive(this->tts_audio_space_available_);  // unblock if waiting on full buffer
    for (int i = 0; i < 50; i++) {
      if (eTaskGetState(this->tts_producer_task_.get_handle()) == eSuspended) {
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    this->tts_producer_task_.destroy();
  }
}

void OpenAIResponses::tts_producer_task_fn_(void *arg) {
  OpenAIResponses *self = (OpenAIResponses *) arg;

  while (!self->tts_task_should_exit_) {
    // Wait for a sentence or done signal. Use pdFALSE (counting semaphore)
    // so multiple notifications don't collapse into one wake. The 100ms
    // timeout is a safety net: if a notification is missed (e.g. given
    // while the producer was between ulTaskNotifyTake and the queue check),
    // the producer still wakes periodically to check for queued sentences.
    ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(100));
    if (self->tts_task_should_exit_) {
      break;
    }

    // Pop a sentence from the queue.
    std::string sentence;
    if (self->tts_queue_mutex_ != nullptr) {
      xSemaphoreTake(self->tts_queue_mutex_, portMAX_DELAY);
      if (self->tts_queue_.empty()) {
        bool done = self->tts_queue_done_;
        xSemaphoreGive(self->tts_queue_mutex_);
        if (done) {
          break;
        }
        continue;
      }
      sentence = std::move(self->tts_queue_.front());
      self->tts_queue_.pop_front();
      xSemaphoreGive(self->tts_queue_mutex_);
    } else {
      break;
    }

    if (sentence.empty()) {
      continue;
    }

    ESP_LOGD(TAG, "TTS producer: generating audio for sentence (%u chars)",
             (unsigned) sentence.size());

    // Build TTS request body for this sentence.
    self->build_tts_request_body_for_(sentence);
    if (self->request_body_ == nullptr) {
      ESP_LOGW(TAG, "TTS producer: failed to build request body, skipping sentence");
      continue;
    }

    // Build the TTS URL.
    char url[256];
    snprintf(url, sizeof(url), "%s/v1/audio/speech", self->endpoint_base_.c_str());

    // Configure the HTTP client.
    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = HTTP_TIMEOUT_MS;
    config.event_handler = OpenAIResponses::http_event_handler_;
    config.transport_type = HTTP_TRANSPORT_OVER_TCP;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    if (strstr(url, "https:") != nullptr) {
      config.transport_type = HTTP_TRANSPORT_OVER_SSL;
      config.crt_bundle_attach = esp_crt_bundle_attach;
    }
#endif

    self->tts_http_client_ = esp_http_client_init(&config);
    if (self->tts_http_client_ == nullptr) {
      ESP_LOGE(TAG, "TTS producer: esp_http_client_init failed");
      continue;
    }

    esp_http_client_set_method(self->tts_http_client_, HTTP_METHOD_POST);
    esp_http_client_set_header(self->tts_http_client_, "Content-Type", "application/json");
    if (!self->auth_header_value_.empty()) {
      esp_http_client_set_header(self->tts_http_client_, "Authorization",
                                 self->auth_header_value_.c_str());
    }

    // Open connection and write the request body.
    esp_err_t err = esp_http_client_open(self->tts_http_client_, self->request_body_size_);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "TTS producer: esp_http_client_open failed: %s", esp_err_to_name(err));
      esp_http_client_cleanup(self->tts_http_client_);
      self->tts_http_client_ = nullptr;
      continue;
    }

    size_t sent = 0;
    while (sent < self->request_body_size_ && !self->tts_task_should_exit_) {
      size_t remaining = self->request_body_size_ - sent;
      size_t to_write = (remaining < HTTP_WRITE_CHUNK) ? remaining : HTTP_WRITE_CHUNK;
      int written = esp_http_client_write(self->tts_http_client_,
                                          reinterpret_cast<const char *>(self->request_body_ + sent), to_write);
      if (written < 0) {
        ESP_LOGE(TAG, "TTS producer: esp_http_client_write failed");
        break;
      }
      sent += written;
    }

    if (self->tts_task_should_exit_) {
      esp_http_client_cleanup(self->tts_http_client_);
      self->tts_http_client_ = nullptr;
      break;
    }

    // Fetch response headers.
    int64_t header_len = esp_http_client_fetch_headers(self->tts_http_client_);
    if (header_len < 0) {
      ESP_LOGE(TAG, "TTS producer: fetch_headers failed");
      esp_http_client_cleanup(self->tts_http_client_);
      self->tts_http_client_ = nullptr;
      continue;
    }

    int status = esp_http_client_get_status_code(self->tts_http_client_);
    if (status != 200) {
      ESP_LOGE(TAG, "TTS producer: HTTP status %d", status);
      esp_http_client_cleanup(self->tts_http_client_);
      self->tts_http_client_ = nullptr;
      continue;
    }

    // Read the WAV response: skip 44-byte header, apply volume, write PCM to
    // the PSRAM ring buffer.
    size_t header_skip = 44;
    uint8_t read_buf[HTTP_TASK_READ_CHUNK];
    bool first_write = true;

    while (!self->tts_task_should_exit_) {
      int got = esp_http_client_read(self->tts_http_client_,
                                     reinterpret_cast<char *>(read_buf), HTTP_TASK_READ_CHUNK);
      if (got < 0) {
        ESP_LOGE(TAG, "TTS producer: esp_http_client_read failed");
        break;
      }
      if (got == 0) {
        break;  // EOF
      }

      size_t off = 0;
      // Skip the 44-byte WAV header (may span multiple chunks).
      while (off < (size_t) got && header_skip > 0) {
        size_t to_skip = (header_skip < (size_t) got - off) ? header_skip : ((size_t) got - off);
        off += to_skip;
        header_skip -= to_skip;
      }

      if (off < (size_t) got) {
        size_t pcm_len = (size_t) got - off;
        uint8_t *pcm = read_buf + off;

        // Apply volume multiplier to int16 samples.
        if (self->volume_multiplier_ != 1.0f) {
          size_t num_samples = pcm_len / sizeof(int16_t);
          int16_t *samples = reinterpret_cast<int16_t *>(pcm);
          for (size_t i = 0; i < num_samples; i++) {
            float scaled = (float) samples[i] * self->volume_multiplier_;
            if (scaled > 32767.0f) {
              scaled = 32767.0f;
            } else if (scaled < -32768.0f) {
              scaled = -32768.0f;
            }
            samples[i] = (int16_t) scaled;
          }
        }

        // Write PCM to the PSRAM ring buffer (blocks if full = backpressure).
        self->write_to_audio_buffer_(pcm, pcm_len);

        if (first_write) {
          first_write = false;
          // Wake the feeder to start playing.
          xSemaphoreGive(self->tts_audio_data_ready_);
        }
      }
    }

    esp_http_client_cleanup(self->tts_http_client_);
    self->tts_http_client_ = nullptr;
  }

  // All sentences processed (or exit flag set).
  self->tts_audio_producer_done_ = true;
  xSemaphoreGive(self->tts_audio_data_ready_);  // wake feeder to check done flag
  ESP_LOGD(TAG, "TTS producer task finished");
  vTaskSuspend(nullptr);
}

// --- Speaker feeder task ----------------------------------------------------

void OpenAIResponses::start_speaker_feeder_task_() {
  if (!this->speaker_feeder_task_.create(OpenAIResponses::speaker_feeder_task_fn_, "oai_spk_feed",
                                          SPEAKER_FEEDER_STACK_SIZE, this, TTS_TASK_PRIORITY, false)) {
    ESP_LOGE(TAG, "Failed to create speaker feeder task");
    this->fail_("task_alloc", "Failed to create speaker feeder task");
  }
}

void OpenAIResponses::stop_speaker_feeder_task_() {
  // Ensure the feeder knows to exit (stop_tts_producer_task_ also sets this,
  // but we set it here too in case stop_speaker_feeder_task_ is called
  // independently).
  this->tts_task_should_exit_ = true;
  // Unblock the feeder if it's waiting on tts_audio_data_ready_.
  xSemaphoreGive(this->tts_audio_data_ready_);
  if (this->speaker_feeder_task_.is_created()) {
    for (int i = 0; i < 50; i++) {
      if (eTaskGetState(this->speaker_feeder_task_.get_handle()) == eSuspended) {
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    this->speaker_feeder_task_.destroy();
  }
}

void OpenAIResponses::speaker_feeder_task_fn_(void *arg) {
  OpenAIResponses *self = (OpenAIResponses *) arg;
  bool speaker_started = false;
  uint8_t read_buf[SPEAKER_FEEDER_CHUNK];

  while (!self->tts_task_should_exit_) {
    size_t w = self->tts_audio_write_offset_.load(std::memory_order_relaxed);
    size_t r = self->tts_audio_read_offset_.load(std::memory_order_relaxed);
    size_t available = (w - r + TTS_AUDIO_BUFFER_SIZE) % TTS_AUDIO_BUFFER_SIZE;

    if (available == 0) {
      if (self->tts_audio_producer_done_) {
        // All audio drained.
        break;
      }
      // Buffer empty but more coming — wait for producer.
      xSemaphoreTake(self->tts_audio_data_ready_, pdMS_TO_TICKS(100));
      continue;
    }

    // Read a chunk from the ring buffer (handle wraparound).
    size_t to_read = (available < SPEAKER_FEEDER_CHUNK) ? available : SPEAKER_FEEDER_CHUNK;
    size_t first_part = (r + to_read > TTS_AUDIO_BUFFER_SIZE) ? (TTS_AUDIO_BUFFER_SIZE - r) : to_read;
    memcpy(read_buf, self->tts_audio_buffer_ + r, first_part);
    if (to_read > first_part) {
      memcpy(read_buf + first_part, self->tts_audio_buffer_, to_read - first_part);
    }
    self->tts_audio_read_offset_ = (r + to_read) % TTS_AUDIO_BUFFER_SIZE;

    // Wake producer if it was blocked on full buffer.
    xSemaphoreGive(self->tts_audio_space_available_);

    // Start the speaker on the first chunk, or restart it if it stopped
    // mid-playback (internal ring buffer drained while waiting for more
    // data from the PSRAM buffer).
    if (speaker_started && self->speaker_ != nullptr &&
        self->speaker_->is_stopped()) {
      // Speaker stopped (underrun). Restart it to accept more audio.
      self->speaker_->start();
    }
    if (!speaker_started) {
      speaker_started = true;
      if (self->speaker_ != nullptr) {
        self->speaker_->set_audio_stream_info(
            audio::AudioStreamInfo(MIC_BITS_PER_SAMPLE, MIC_CHANNELS, self->tts_sample_rate_));
        self->speaker_->start();
      }
      // Signal the main loop to fire on_tts_stream_start.
      uint8_t start_msg = (uint8_t) HttpMsgType::TTS_STREAM_START;
      xMessageBufferSend(self->http_msg_buffer_, &start_msg, 1, portMAX_DELAY);
    }

    // Feed to speaker — use non-blocking play() (timeout=0) with a retry
    // loop. The blocking variant (timeout>0) calls vTaskDelay() while the
    // speaker is in STATE_STARTING, which blocks forever with portMAX_DELAY
    // even after the speaker transitions to STATE_RUNNING. The retry loop
    // with vTaskDelay(1) yields to the speaker's task so it can transition
    // to STATE_RUNNING, then play() succeeds once the ring buffer has space.
    if (self->speaker_ != nullptr) {
      size_t off = 0;
      while (off < to_read && !self->tts_task_should_exit_) {
        size_t written = self->speaker_->play(read_buf + off, to_read - off, 0);
        if (written == 0) {
          // Speaker can't accept data right now. If it's stopped (state
          // STATE_STOPPED after draining its ring buffer), break out so the
          // feeder loop can check the PSRAM buffer for more data and either
          // restart the speaker or finish. Otherwise (STATE_STARTING or ring
          // buffer full), yield and retry.
          if (self->speaker_->is_stopped()) {
            break;
          }
          vTaskDelay(pdMS_TO_TICKS(1));
          continue;
        }
        off += written;
        // Yield to other tasks (especially the main loop) so the gt911 touch
        // component can process touch events. Without this yield, the feeder
        // runs in a tight loop (play succeeds immediately when the i2s ring
        // buffer has space) and starves the main loop, preventing the stop
        // button from being detected.
        vTaskDelay(pdMS_TO_TICKS(1));
      }
    }
  }

  // Finished: finish the speaker and signal the main loop.
  if (speaker_started && self->speaker_ != nullptr && !self->tts_task_should_exit_) {
    self->speaker_->finish();
  }
  // Send TTS_STREAM_DONE to the main loop (unless we're being cancelled —
  // request_stop handles its own teardown).
  if (!self->tts_task_should_exit_) {
    uint8_t done_msg = (uint8_t) HttpMsgType::TTS_STREAM_DONE;
    xMessageBufferSend(self->http_msg_buffer_, &done_msg, 1, portMAX_DELAY);
  }
  ESP_LOGD(TAG, "Speaker feeder task finished");
  vTaskSuspend(nullptr);
}

// --- Response processing ---------------------------------------------------

void OpenAIResponses::process_sse_bytes_(const uint8_t *data, size_t len) {
  // Append bytes to the line buffer, and whenever a '\n' is found, process the
  // complete line. The Responses API SSE stream uses two line kinds:
  //   event: <type>      — sets the event type for the next data line
  //   data: <json>        — a JSON payload, interpreted using the current event type
  // A blank line separates events. The stream terminates with
  // "event: response.completed" (no [DONE] sentinel).
  for (size_t i = 0; i < len; i++) {
    char c = (char) data[i];
    if (c == '\n') {
      // NUL-terminate and process the accumulated line.
      if (this->sse_line_len_ < SSE_LINE_MAX) {
        this->sse_line_buffer_[this->sse_line_len_] = '\0';
      } else {
        this->sse_line_buffer_[SSE_LINE_MAX - 1] = '\0';
      }
      this->process_sse_line_(this->sse_line_buffer_, this->sse_line_len_);
      this->sse_line_len_ = 0;
    } else if (c != '\r') {
      if (this->sse_line_len_ < SSE_LINE_MAX - 1) {
        this->sse_line_buffer_[this->sse_line_len_++] = c;
      } else if (this->sse_line_len_ == SSE_LINE_MAX - 1) {
        // Line exceeded the buffer — log once (not per dropped byte).
        ESP_LOGW(TAG, "SSE line exceeded %u bytes; truncated (increase SSE_LINE_MAX)",
                 (unsigned) SSE_LINE_MAX);
        this->sse_line_len_++;  // prevent re-warning; excess is dropped below
      }
      // else: line too long; drop excess until the next newline.
    }
  }
}

void OpenAIResponses::process_sse_line_(const char *line, size_t len) {
  if (len == 0) {
    return;  // blank line separates events
  }

  // "event: <type>" line: remember the type for the next data line.
  const char *event_prefix = "event:";
  const size_t event_prefix_len = 6;
  if (len >= event_prefix_len && strncmp(line, event_prefix, event_prefix_len) == 0) {
    const char *type = line + event_prefix_len;
    size_t type_len = len - event_prefix_len;
    // Skip a single leading space.
    if (type_len > 0 && type[0] == ' ') {
      type++;
      type_len--;
    }
    this->sse_event_type_.assign(type, type_len);
    return;
  }

  // "data: <json>" line: parse using the current event type.
  const char *prefix = "data:";
  const size_t prefix_len = 5;
  if (len < prefix_len || strncmp(line, prefix, prefix_len) != 0) {
    return;  // ignore comments, id:, etc.
  }
  const char *payload = line + prefix_len;
  size_t payload_len = len - prefix_len;
  // Skip a single leading space.
  if (payload_len > 0 && payload[0] == ' ') {
    payload++;
    payload_len--;
  }

  // Some servers emit a literal "[DONE]" sentinel after the stream.
  if (payload_len == 6 && strncmp(payload, "[DONE]", 6) == 0) {
    ESP_LOGV(TAG, "SSE [DONE] received");
    this->speech_ended_ = true;  // reuse as "stream done" signal for the chat read loop
    return;
  }

  const std::string &evt = this->sse_event_type_;
  // DEBUG-level log for event types so we can diagnose which events the
  // server actually sends (LocalAI's Responses API may differ from OpenAI's).
  ESP_LOGD(TAG, "SSE event=%s data(%u bytes)", evt.c_str(), (unsigned) payload_len);

  // --- response.created / response.completed: capture the response id ---
  // The id (resp_...) is used as previous_response_id on round 2+.
  // Per the Responses API spec, the response object is nested under
  // data.response (not at the top level): the SSE data payload is
  // {"type":"response.created","sequence_number":N,"response":{"id":"resp_...",...}}
  //
  // We skip response.in_progress entirely — it carries the same id as
  // response.created (which arrives first) and has no new data. Skipping it
  // avoids parsing a ~17KB JSON payload for nothing.
  if (evt == "response.created" || evt == "response.completed") {
    auto doc = json::parse_json(reinterpret_cast<const uint8_t *>(payload), payload_len);
    if (!doc.isNull()) {
      JsonObject root = doc.as<JsonObject>();
      // The response object is nested under "response".
      JsonObject response_obj = root["response"].as<JsonObject>();
      if (response_obj.isNull()) {
        // Fallback: some servers may put id at the top level.
        response_obj = root;
      }
      const char *id = response_obj["id"];
      if (id != nullptr && id[0] != '\0') {
        this->previous_response_id_ = id;
        ESP_LOGI(TAG, "Captured response id: %s (from %s)", id, evt.c_str());
      } else {
        ESP_LOGD(TAG, "Lifecycle event %s has no 'id' field", evt.c_str());
      }
#ifdef USE_OPENAI_RESPONSES_MCP
      if (evt == "response.completed") {
        // Inspect the output array for function_call items.
        JsonArray output = response_obj["output"].as<JsonArray>();
        if (!output.isNull()) {
          for (JsonObject item : output) {
            const char *type = item["type"];
            if (type != nullptr && strcmp(type, "function_call") == 0) {
              this->had_tool_calls_ = true;
              break;
            }
          }
        }
      }
#endif
    } else {
      ESP_LOGW(TAG, "Lifecycle event %s JSON parse failed", evt.c_str());
    }
    if (evt == "response.completed") {
      this->speech_ended_ = true;  // signal the chat read loop the stream is done
    }
    return;
  }

  // --- response.output_text.delta: accumulate response text ---
  if (evt == "response.output_text.delta") {
    auto doc = json::parse_json(reinterpret_cast<const uint8_t *>(payload), payload_len);
    if (doc.isNull()) {
      ESP_LOGW(TAG, "SSE JSON parse failed (%u bytes)", (unsigned) payload_len);
      return;
    }
    JsonObject root = doc.as<JsonObject>();
    const char *delta = root["delta"];
    if (delta != nullptr && delta[0] != '\0') {
      bool first_delta = this->response_text_.empty();
      this->response_text_.append(delta);
#ifdef USE_TEXT_SENSOR
      // Only publish on the first delta (for initial display) and at DONE
      // (for final text). Publishing on every delta generates massive log
      // spam for long responses and the text doesn't all fit on the screen.
      if (first_delta && this->text_response_sensor_ != nullptr) {
        this->text_response_sensor_->publish_state(this->response_text_);
      }
#endif
      // Streaming TTS: accumulate text and push sentences to the TTS queue
      // as they complete. The TTS producer task generates audio concurrently
      // with the SSE stream, and the feeder task plays it continuously.
      if (this->streaming_tts_) {
        this->tts_pending_text_.append(delta);
        this->tts_pending_text_ = strip_markdown_(this->tts_pending_text_);
        size_t split_pos = this->find_sentence_boundary_(this->tts_pending_text_);
        if (split_pos > 0) {
          std::string sentence = this->tts_pending_text_.substr(0, split_pos);
          this->tts_pending_text_ = this->tts_pending_text_.substr(split_pos);
          if (is_tool_call_leak_(sentence)) {
            ESP_LOGW(TAG, "Tool call text leak detected, suppressing from TTS: %.80s",
                     sentence.c_str());
          } else {
            this->push_tts_sentence_(sentence);
          }
        } else if (this->tts_pending_text_.size() >= 200) {
          if (!is_tool_call_leak_(this->tts_pending_text_)) {
            this->push_tts_sentence_(this->tts_pending_text_);
          }
          this->tts_pending_text_.clear();
        }
      }
    }
    return;
  }

  // --- response.output_text.done: nothing to do (text already accumulated) ---
  if (evt == "response.output_text.done") {
    return;
  }

#ifdef USE_OPENAI_RESPONSES_MCP
  // --- response.output_item.added: start a new function_call tool call ---
  // Some servers (e.g. LocalAI) send the complete function_call item —
  // including arguments — in this event, with no subsequent
  // function_call_arguments.delta/.done events. We capture arguments here
  // if present; the .done handler below will overwrite with the final value
  // if the server sends incremental arguments.
  if (evt == "response.output_item.added" || evt == "response.output_item.done") {
    auto doc = json::parse_json(reinterpret_cast<const uint8_t *>(payload), payload_len);
    if (doc.isNull()) {
      return;
    }
    JsonObject root = doc.as<JsonObject>();
    int output_index = root["output_index"] | -1;
    JsonObject item = root["item"].as<JsonObject>();
    if (item.isNull()) {
      return;
    }
    const char *type = item["type"];
    if (type == nullptr || strcmp(type, "function_call") != 0) {
      return;  // only function_call items are tool calls
    }
    // For output_item.added, use output_index to find/create the entry.
    // For output_item.done, use output_index or item id to find the entry.
    AccumulatedToolCall *acc = nullptr;
    if (output_index >= 0 && output_index < (int) MAX_PARALLEL_TOOLS) {
      for (size_t i = 0; i < this->accumulated_tool_call_count_; i++) {
        if (this->accumulated_tool_calls_[i].index == output_index) {
          acc = &this->accumulated_tool_calls_[i];
          break;
        }
      }
      if (acc == nullptr && evt == "response.output_item.added" &&
          this->accumulated_tool_call_count_ < MAX_PARALLEL_TOOLS) {
        acc = &this->accumulated_tool_calls_[this->accumulated_tool_call_count_++];
        acc->index = output_index;
      }
    }
    // Fallback for output_item.done: match by item id.
    if (acc == nullptr) {
      const char *item_id = item["id"];
      if (item_id != nullptr) {
        for (size_t i = 0; i < this->accumulated_tool_call_count_; i++) {
          if (this->accumulated_tool_calls_[i].item_id == item_id) {
            acc = &this->accumulated_tool_calls_[i];
            break;
          }
        }
      }
    }
    if (acc == nullptr) {
      return;
    }
    const char *call_id = item["call_id"];
    if (call_id != nullptr && call_id[0] != '\0') {
      acc->id = call_id;
    }
    const char *item_id = item["id"];
    if (item_id != nullptr && item_id[0] != '\0') {
      acc->item_id = item_id;
    }
    const char *name = item["name"];
    if (name != nullptr && name[0] != '\0') {
      acc->name = name;
    }
    // Capture arguments if present (LocalAI sends them in output_item.added).
    // The .done event always has the final arguments; overwrite if present.
    const char *arguments = item["arguments"];
    if (arguments != nullptr) {
      acc->arguments = arguments;
    }
    if (evt == "response.output_item.done") {
      ESP_LOGD(TAG, "Tool call item done: %s (call_id=%s, args=%u chars)",
               acc->name.c_str(), acc->id.c_str(), (unsigned) acc->arguments.size());
    }
    return;
  }

  // --- response.function_call_arguments.delta: append argument fragment ---
  if (evt == "response.function_call_arguments.delta") {
    auto doc = json::parse_json(reinterpret_cast<const uint8_t *>(payload), payload_len);
    if (doc.isNull()) {
      return;
    }
    JsonObject root = doc.as<JsonObject>();
    const char *delta = root["delta"];
    if (delta == nullptr) {
      return;
    }
    // Match the fragment to its tool call by item_id (preferred) or
    // output_index (fallback).
    const char *item_id = root["item_id"];
    int output_index = root["output_index"] | -1;
    AccumulatedToolCall *acc = nullptr;
    for (size_t i = 0; i < this->accumulated_tool_call_count_; i++) {
      if (item_id != nullptr && !this->accumulated_tool_calls_[i].item_id.empty() &&
          this->accumulated_tool_calls_[i].item_id == item_id) {
        acc = &this->accumulated_tool_calls_[i];
        break;
      }
    }
    if (acc == nullptr && output_index >= 0 && output_index < (int) MAX_PARALLEL_TOOLS) {
      for (size_t i = 0; i < this->accumulated_tool_call_count_; i++) {
        if (this->accumulated_tool_calls_[i].index == output_index) {
          acc = &this->accumulated_tool_calls_[i];
          break;
        }
      }
    }
    if (acc == nullptr) {
      return;
    }
    acc->arguments.append(delta);
    return;
  }

  // --- response.function_call_arguments.done: finalize arguments ---
  // Per the spec, the done event has arguments + item_id at the top level:
  //   {"type":"response.function_call_arguments.done","sequence_number":N,
  //    "item_id":"fc_...","output_index":N,"arguments":"{...}"}
  if (evt == "response.function_call_arguments.done") {
    auto doc = json::parse_json(reinterpret_cast<const uint8_t *>(payload), payload_len);
    if (doc.isNull()) {
      return;
    }
    JsonObject root = doc.as<JsonObject>();
    const char *item_id = root["item_id"];
    const char *arguments = root["arguments"];
    // Fallback: some servers may nest under "item".
    if (arguments == nullptr) {
      JsonObject item = root["item"].as<JsonObject>();
      if (!item.isNull()) {
        arguments = item["arguments"];
        if (item_id == nullptr) {
          item_id = item["id"];
        }
      }
    }
    if (arguments == nullptr) {
      return;
    }
    // Replace any partial accumulation with the final arguments.
    AccumulatedToolCall *acc = nullptr;
    if (item_id != nullptr) {
      for (size_t i = 0; i < this->accumulated_tool_call_count_; i++) {
        if (this->accumulated_tool_calls_[i].item_id == item_id) {
          acc = &this->accumulated_tool_calls_[i];
          break;
        }
      }
    }
    if (acc == nullptr) {
      // Fallback: match by output_index if present.
      int output_index = root["output_index"] | -1;
      if (output_index >= 0) {
        for (size_t i = 0; i < this->accumulated_tool_call_count_; i++) {
          if (this->accumulated_tool_calls_[i].index == output_index) {
            acc = &this->accumulated_tool_calls_[i];
            break;
          }
        }
      }
    }
    if (acc == nullptr) {
      return;
    }
    acc->arguments = arguments;
    ESP_LOGD(TAG, "Tool call %s arguments finalized: %.100s", acc->name.c_str(), arguments);
    return;
  }
#endif  // USE_OPENAI_RESPONSES_MCP

  // --- error event: surface the error ---
  if (evt == "error") {
    auto doc = json::parse_json(reinterpret_cast<const uint8_t *>(payload), payload_len);
    if (!doc.isNull()) {
      JsonObject root = doc.as<JsonObject>();
      const char *message = root["message"];
      if (message != nullptr) {
        ESP_LOGW(TAG, "Responses API error event: %s", message);
      } else {
        ESP_LOGW(TAG, "Responses API error event (no message)");
      }
    } else {
      ESP_LOGW(TAG, "Responses API error event: %.*s", (int) payload_len, payload);
    }
    return;
  }

  // Unknown event types (response.created already handled above; others like
  // response.refusal.delta, response.file_part.added, etc.) are safely ignored.
  ESP_LOGV(TAG, "SSE ignoring event %s", evt.c_str());
}

void OpenAIResponses::process_stt_response_(const uint8_t *data, size_t len) {
  // Append to the accumulating STT response text; the full body is parsed once
  // the stream ends (handled in loop READING_STT).
  this->stt_response_text_.append(reinterpret_cast<const char *>(data), len);
}

// --- Speaker output --------------------------------------------------------

void OpenAIResponses::feed_speaker_(const uint8_t *data, size_t len) {
  if (this->speaker_ == nullptr || this->speaker_buffer_ == nullptr) {
    return;
  }
  size_t offset = 0;
  while (offset < len) {
    size_t free = SPEAKER_BUFFER_SIZE - this->speaker_buffer_index_;
    size_t to_copy = (len - offset < free) ? (len - offset) : free;
    memcpy(this->speaker_buffer_ + this->speaker_buffer_index_, data + offset, to_copy);
    this->speaker_buffer_index_ += to_copy;
    offset += to_copy;

    // Apply volume multiplier to the newly copied int16 samples.
    if (this->volume_multiplier_ != 1.0f && to_copy > 0) {
      size_t start_sample_index = (this->speaker_buffer_index_ - to_copy) / sizeof(int16_t);
      size_t num_samples = to_copy / sizeof(int16_t);
      int16_t *samples = reinterpret_cast<int16_t *>(this->speaker_buffer_);
      for (size_t i = 0; i < num_samples; i++) {
        float scaled = (float) samples[start_sample_index + i] * this->volume_multiplier_;
        if (scaled > 32767.0f) {
          scaled = 32767.0f;
        } else if (scaled < -32768.0f) {
          scaled = -32768.0f;
        }
        samples[start_sample_index + i] = (int16_t) scaled;
      }
    }

    if (this->speaker_buffer_index_ >= SPEAKER_BUFFER_SIZE) {
      this->flush_speaker_buffer_();
    }
  }
}

void OpenAIResponses::flush_speaker_buffer_() {
  if (this->speaker_ == nullptr || this->speaker_buffer_ == nullptr || this->speaker_buffer_index_ == 0) {
    return;
  }
  // If the speaker has stopped (e.g. underrun on a short TTS response),
  // play() would block forever waiting for a dead speaker task to consume
  // data. Drop the buffer and let the loop transition to teardown.
  if (this->speaker_->is_stopped()) {
    ESP_LOGW(TAG, "Speaker stopped during TTS playback; dropping %u bytes",
             (unsigned) this->speaker_buffer_index_);
    this->speaker_buffer_index_ = 0;
    return;
  }
  size_t written = this->speaker_->play(this->speaker_buffer_, this->speaker_buffer_index_);
  // Move any unwritten bytes to the front of the buffer.
  if (written > 0 && written < this->speaker_buffer_index_) {
    memmove(this->speaker_buffer_, this->speaker_buffer_ + written, this->speaker_buffer_index_ - written);
  }
  this->speaker_buffer_index_ -= (written <= this->speaker_buffer_index_) ? written : this->speaker_buffer_index_;
}

// --- Main loop state machine ------------------------------------------------

#ifdef USE_OPENAI_RESPONSES_MCP
// --- Tool execution helpers -------------------------------------------------

void OpenAIResponses::retain_b64_audio_(size_t offset, size_t len) {
  ExternalRAMAllocator<char> ext(ExternalRAMAllocator<char>::ALLOC_EXTERNAL);
  this->retained_b64_capacity_ = len + 1;
  this->retained_b64_audio_ = ext.allocate(this->retained_b64_capacity_);
  if (this->retained_b64_audio_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate retained b64 audio (%d bytes)", (int) len);
    return;
  }
  memcpy(this->retained_b64_audio_, reinterpret_cast<const char *>(this->request_body_) + offset, len);
  this->retained_b64_len_ = len;
  ESP_LOGV(TAG, "Retained base64 audio: %u bytes", (unsigned) len);
}

void OpenAIResponses::reset_accumulated_tool_calls_() {
  for (size_t i = 0; i < this->accumulated_tool_call_count_; i++) {
    this->accumulated_tool_calls_[i].index = -1;
    this->accumulated_tool_calls_[i].id.clear();
    this->accumulated_tool_calls_[i].item_id.clear();
    this->accumulated_tool_calls_[i].name.clear();
    this->accumulated_tool_calls_[i].arguments.clear();
  }
  this->accumulated_tool_call_count_ = 0;
}

void OpenAIResponses::append_tool_result_message_(const std::string &tool_call_id,
                                                       const std::string &result) {
  // Responses API tool result: a function_call_output input item.
  //   {"type":"function_call_output","call_id":"<call_id>","output":"<result>"}
  std::string msg = "{\"type\":\"function_call_output\",\"call_id\":\"" + escape_json_string_(tool_call_id);
  msg += "\",\"output\":\"" + escape_json_string_(result) + "\"}";
  this->turn_messages_.push_back(std::move(msg));
}

void OpenAIResponses::build_chat_request_body_from_history_() {
  // Round 2+ Responses API request. Two paths:
  //  1. previous_response_id_ set (the happy path): the server stored the
  //     prior response, so we only send the function_call_output items. No
  //     tools array, no instructions, no user input — the server already has
  //     all of that.
  //  2. previous_response_id_ empty (fallback, e.g. server doesn't support
  //     store/previous_response_id): rebuild the full round-1 context
  //     (instructions, user input, tools) plus the function_call_output items.
  std::string escaped_model = escape_json_string_(this->chat_model_);

  // --- Path 1: previous_response_id is set — send only NEW tool outputs ---
  // Per the spec, the server loads the prior response's input+output, so we
  // only need to send the function_call_output items added since the last
  // round (turn_messages_sent_count_ .. end). Re-sending earlier outputs
  // causes the model to see stale/duplicate results and re-call tools.
  // We also include the tools array + tool_choice as a workaround for servers
  // (e.g. LocalAI) that don't restore tools from the stored response.
  if (!this->previous_response_id_.empty()) {
    std::string body =
        "{\"model\":\"" + escaped_model + "\",\"stream\":true,\"store\":true,"
        "\"previous_response_id\":\"" + escape_json_string_(this->previous_response_id_) + "\","
        "\"input\":[";
    bool first = true;
    for (size_t i = this->turn_messages_sent_count_; i < this->turn_messages_.size(); i++) {
      if (!first) {
        body += ",";
      }
      first = false;
      body += this->turn_messages_[i];
    }
    body += "]";
    // Mark all current messages as sent so the next round only sends new ones.
    this->turn_messages_sent_count_ = this->turn_messages_.size();
    // Per the spec, previous_response_id restores the prior response's full
    // input (including tools and instructions), so we don't re-send them.
    // This shrinks the round-2 request by ~16KB and the server's lifecycle
    // events (response.created/completed) shrink accordingly since they no
    // longer echo the tools array back.
    body += "}";

    ExternalRAMAllocator<uint8_t> ext(ExternalRAMAllocator<uint8_t>::ALLOC_EXTERNAL);
    if (this->request_body_ != nullptr) {
      ext.deallocate(this->request_body_, this->request_body_capacity_);
    }
    this->request_body_capacity_ = body.size() + 1;
    this->request_body_ = ext.allocate(this->request_body_capacity_);
    if (this->request_body_ == nullptr) {
      this->fail_("alloc_failed", "Could not allocate responses round-2 body");
      return;
    }
    memcpy(this->request_body_, body.data(), body.size());
    this->request_body_size_ = body.size();
    this->request_body_sent_ = 0;
    ESP_LOGV(TAG, "Built round-2 responses body (previous_response_id): %u bytes (%u outputs)",
             (unsigned) this->request_body_size_, (unsigned) this->turn_messages_.size());
    return;
  }

  // --- Path 2: fallback — rebuild the full round-1 context + tool calls + tool outputs ---
  // Without previous_response_id, the server has no context. We must send the
  // user input, the function_call items (so the server sees what was called),
  // and the function_call_output items (the results). Per the spec, the
  // logical order is: user input → function_call items → function_call_output
  // items.
  std::string escaped_prompt = escape_json_string_(this->system_prompt_);

  // Build the function_call items from accumulated_tool_calls_ (these are the
  // calls the LLM made in the previous round; the server needs them to match
  // the call_ids in the function_call_output items).
  std::string function_call_items;
  for (size_t i = 0; i < this->accumulated_tool_call_count_; i++) {
    const auto &tc = this->accumulated_tool_calls_[i];
    if (tc.index < 0 || tc.id.empty()) {
      continue;
    }
    if (!function_call_items.empty()) {
      function_call_items += ",";
    }
    function_call_items += "{\"type\":\"function_call\"";
    function_call_items += ",\"call_id\":\"" + escape_json_string_(tc.id) + "\"";
    function_call_items += ",\"name\":\"" + escape_json_string_(tc.name) + "\"";
    function_call_items += ",\"arguments\":\"" + escape_json_string_(tc.arguments) + "\"";
    function_call_items += "}";
  }

  // Build the function_call_output items (already in turn_messages_ as
  // serialized JSON function_call_output objects).
  std::string tool_output_items;
  for (const auto &msg : this->turn_messages_) {
    if (!tool_output_items.empty()) {
      tool_output_items += ",";
    }
    tool_output_items += msg;
  }

  // Suffix: closes the input array + tool_choice + tools.
  std::string suffix = "],\"tool_choice\":\"auto\"";
  if (!this->cached_tools_json_.empty()) {
    suffix += ",\"tools\":" + this->cached_tools_json_;
  } else
#ifdef USE_OPENAI_RESPONSES_TOOLS
  if (this->has_tools_) {
    suffix += ",\"tools\":" + std::string(TOOLS_JSON);
  }
#endif
  suffix += "}";

  // Assemble the tool items portion of the input array (function_call items
  // followed by function_call_output items), comma-separated.
  std::string tool_items;
  if (!function_call_items.empty()) {
    tool_items += function_call_items;
  }
  if (!tool_output_items.empty()) {
    if (!tool_items.empty()) {
      tool_items += ",";
    }
    tool_items += tool_output_items;
  }

  if (this->multimodal_ && this->retained_b64_audio_ != nullptr) {
    // Multimodal: user input message contains the retained base64 audio.
    std::string prefix =
        "{\"model\":\"" + escaped_model + "\",\"stream\":true,\"store\":true,"
        "\"instructions\":\"" + escaped_prompt + "\","
        "\"input\":[{\"type\":\"message\",\"role\":\"user\",\"content\":["
        "{\"type\":\"input_text\",\"text\":\"\"},"
        "{\"type\":\"input_file\",\"file_data\":\"data:audio/wav;base64;";
    std::string user_suffix = "\",\"filename\":\"audio.wav\"}]}";
    // Tool items follow the user message in the input array.
    std::string tool_items_prefix;
    if (!tool_items.empty()) {
      tool_items_prefix = "," + tool_items;
    }

    size_t b64_len = this->retained_b64_len_;
    size_t total = prefix.size() + b64_len + user_suffix.size() + tool_items_prefix.size() + suffix.size();

    ExternalRAMAllocator<uint8_t> ext(ExternalRAMAllocator<uint8_t>::ALLOC_EXTERNAL);
    if (this->request_body_ != nullptr) {
      ext.deallocate(this->request_body_, this->request_body_capacity_);
    }
    this->request_body_capacity_ = total + 1;
    this->request_body_ = ext.allocate(this->request_body_capacity_);
    if (this->request_body_ == nullptr) {
      this->fail_("alloc_failed", "Could not allocate responses history body");
      return;
    }

    size_t off = 0;
    memcpy(this->request_body_ + off, prefix.data(), prefix.size());
    off += prefix.size();
    memcpy(this->request_body_ + off, this->retained_b64_audio_, b64_len);
    off += b64_len;
    memcpy(this->request_body_ + off, user_suffix.data(), user_suffix.size());
    off += user_suffix.size();
    if (!tool_items_prefix.empty()) {
      memcpy(this->request_body_ + off, tool_items_prefix.data(), tool_items_prefix.size());
      off += tool_items_prefix.size();
    }
    memcpy(this->request_body_ + off, suffix.data(), suffix.size());
    off += suffix.size();

    this->request_body_size_ = off;
    this->request_body_sent_ = 0;
    ESP_LOGV(TAG, "Built responses history body (multimodal fallback): %u bytes (b64=%u calls=%u outputs=%u)",
             (unsigned) this->request_body_size_, (unsigned) b64_len,
             (unsigned) this->accumulated_tool_call_count_,
             (unsigned) this->turn_messages_.size());
  } else {
    // Text mode: user input message is text.
    std::string escaped_text = escape_json_string_(this->user_text_);
    std::string body =
        "{\"model\":\"" + escaped_model + "\",\"stream\":true,\"store\":true,"
        "\"instructions\":\"" + escaped_prompt + "\","
        "\"input\":[{\"type\":\"message\",\"role\":\"user\",\"content\":\"" + escaped_text + "\"}";
    if (!tool_items.empty()) {
      body += "," + tool_items;
    }
    body += suffix;

    ExternalRAMAllocator<uint8_t> ext(ExternalRAMAllocator<uint8_t>::ALLOC_EXTERNAL);
    if (this->request_body_ != nullptr) {
      ext.deallocate(this->request_body_, this->request_body_capacity_);
    }
    this->request_body_capacity_ = body.size() + 1;
    this->request_body_ = ext.allocate(this->request_body_capacity_);
    if (this->request_body_ == nullptr) {
      this->fail_("alloc_failed", "Could not allocate responses history body");
      return;
    }
    memcpy(this->request_body_, body.data(), body.size());
    this->request_body_size_ = body.size();
    this->request_body_sent_ = 0;
    ESP_LOGV(TAG, "Built responses history body (text fallback): %u bytes (calls=%u outputs=%u)",
             (unsigned) this->request_body_size_,
             (unsigned) this->accumulated_tool_call_count_,
             (unsigned) this->turn_messages_.size());
  }
}

std::string OpenAIResponses::extract_mcp_json_() {
  const std::string &raw = this->mcp_response_text_;

  // Check if it's SSE (contains "data: ").
  size_t data_pos = raw.find("data: ");
  if (data_pos != std::string::npos) {
    // Find the last "data: " line containing a JSON object.
    size_t search_from = 0;
    std::string last_json;
    while (true) {
      size_t pos = raw.find("data: ", search_from);
      if (pos == std::string::npos) {
        break;
      }
      size_t start = pos + 6;
      size_t end = raw.find('\n', start);
      if (end == std::string::npos) {
        end = raw.size();
      }
      std::string line = raw.substr(start, end - start);
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
        line.pop_back();
      }
      if (!line.empty() && line[0] == '{') {
        last_json = line;
      }
      search_from = end;
    }
    return last_json;
  }

  // Not SSE: trim leading/trailing whitespace and return the raw body.
  size_t start = raw.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    return "";
  }
  size_t end = raw.find_last_not_of(" \t\r\n");
  return raw.substr(start, end - start + 1);
}

void OpenAIResponses::process_mcp_response_() {
  // Handle `initialize` and `notifications/initialized` responses first
  // (they don't carry tool data, just handshake bookkeeping).
  if (this->current_mcp_call_type_ == McpCallType::INITIALIZED_NOTIF) {
    // notifications/initialized response (may be empty or 202 Accepted).
    // The server is now fully initialized. Store the session ID and proceed
    // to tools/list on the next FETCHING_TOOLS pass.
    auto &srv = this->mcp_servers_[this->mcp_route_server_index_];
    srv.session_id = this->current_mcp_session_id_;
    srv.initialized = true;
    ESP_LOGV(TAG, "MCP server %u initialized (session=%s)",
             (unsigned) this->mcp_route_server_index_,
             srv.session_id.c_str());
    this->current_mcp_call_type_ = McpCallType::TOOLS_LIST;
    this->set_state_(State::FETCHING_TOOLS);
    return;
  }

  if (this->current_mcp_call_type_ == McpCallType::INITIALIZE) {
    // initialize response: the http_task already captured Mcp-Session-Id.
    auto &srv = this->mcp_servers_[this->mcp_route_server_index_];
    srv.session_id = this->current_mcp_session_id_;
    ESP_LOGV(TAG, "MCP server %u initialize response received (session=%s)",
             (unsigned) this->mcp_route_server_index_, srv.session_id.c_str());
    if (srv.session_id.empty()) {
      // Server didn't return a Mcp-Session-Id: it's stateless. Skip the
      // handshake (no notifications/initialized) and go straight to tools/list.
      // Calling initialize on a stateless server creates a server-side session
      // that expires, causing 400s on later calls.
      ESP_LOGV(TAG, "MCP server %u is stateless (no session ID); skipping handshake",
               (unsigned) this->mcp_route_server_index_);
      srv.stateless = true;
      srv.initialized = true;
      this->current_mcp_call_type_ = McpCallType::TOOLS_LIST;
    } else {
      // Server returned a session ID: send notifications/initialized next.
      this->current_mcp_call_type_ = McpCallType::INITIALIZED_NOTIF;
    }
    this->set_state_(State::FETCHING_TOOLS);
    return;
  }

  // For tools/list and tools/call responses, parse the JSON.
  std::string json_str = this->extract_mcp_json_();
  if (json_str.empty()) {
    ESP_LOGW(TAG, "MCP response was empty");
    if (this->mcp_phase_ == McpPhase::FETCHING_TOOLS) {
      this->mcp_route_server_index_++;
      // Set call type for the next server (or finish if no more servers).
      if (this->mcp_route_server_index_ < this->mcp_servers_.size()) {
        const auto &next_srv = this->mcp_servers_[this->mcp_route_server_index_];
        this->current_mcp_call_type_ = (next_srv.initialized || next_srv.stateless)
                                           ? McpCallType::TOOLS_LIST
                                           : McpCallType::INITIALIZE;
      }
      this->set_state_(State::FETCHING_TOOLS);
    } else {
      const auto &tool = this->accumulated_tool_calls_[this->current_tool_index_];
      this->append_tool_result_message_(tool.id, "MCP response was empty");
      this->current_tool_index_++;
      this->set_state_(State::EXECUTING_TOOLS);
    }
    return;
  }

  ESP_LOGV(TAG, "Parsing MCP response JSON (%u bytes)", (unsigned) json_str.size());
  // Parse with a higher nesting limit than the default (10) — the zim_query
  // tool's inputSchema has deeply nested anyOf/properties that exceed it.
  // Use a PSRAM-backed allocator so the JSON document buffer doesn't fragment
  // the internal heap (zim_query schemas can be 10KB+).
#ifdef USE_PSRAM
  PsRamJsonAllocator psram_alloc;
  JsonDocument doc(&psram_alloc);
#else
  JsonDocument doc;
#endif
  DeserializationError err =
      deserializeJson(doc, json_str.c_str(), json_str.size(),
                      DeserializationOption::NestingLimit(20));
  if (err != DeserializationError::Ok) {
    ESP_LOGE(TAG, "MCP JSON parse failed: %s", err.c_str());
    if (this->mcp_phase_ == McpPhase::FETCHING_TOOLS) {
      this->mcp_route_server_index_++;
      if (this->mcp_route_server_index_ < this->mcp_servers_.size()) {
        const auto &next_srv = this->mcp_servers_[this->mcp_route_server_index_];
        this->current_mcp_call_type_ = (next_srv.initialized || next_srv.stateless)
                                           ? McpCallType::TOOLS_LIST
                                           : McpCallType::INITIALIZE;
      }
      this->set_state_(State::FETCHING_TOOLS);
    } else {
      const auto &tool = this->accumulated_tool_calls_[this->current_tool_index_];
      this->append_tool_result_message_(tool.id, "MCP JSON parse failed");
      this->current_tool_index_++;
      this->set_state_(State::EXECUTING_TOOLS);
    }
    return;
  }

  JsonObject root = doc.as<JsonObject>();

  // Check for JSON-RPC error response (transport/server-level error).
  JsonObject error = root["error"].as<JsonObject>();
  if (!error.isNull()) {
    const char *msg = error["message"];
    std::string err_msg = (msg != nullptr) ? std::string(msg) : "unknown MCP error";
    ESP_LOGW(TAG, "MCP error: %s", err_msg.c_str());
    if (this->mcp_phase_ == McpPhase::FETCHING_TOOLS) {
      this->mcp_route_server_index_++;
      this->set_state_(State::FETCHING_TOOLS);
    } else {
      const auto &tool = this->accumulated_tool_calls_[this->current_tool_index_];
      this->append_tool_result_message_(tool.id, "error: " + err_msg);
      this->current_tool_index_++;
      this->set_state_(State::EXECUTING_TOOLS);
    }
    return;
  }

  JsonObject result = root["result"].as<JsonObject>();
  if (result.isNull()) {
    ESP_LOGW(TAG, "MCP response has no result");
    if (this->mcp_phase_ == McpPhase::FETCHING_TOOLS) {
      this->mcp_route_server_index_++;
      this->set_state_(State::FETCHING_TOOLS);
    } else {
      const auto &tool = this->accumulated_tool_calls_[this->current_tool_index_];
      this->append_tool_result_message_(tool.id, "MCP response had no result");
      this->current_tool_index_++;
      this->set_state_(State::EXECUTING_TOOLS);
    }
    return;
  }

  if (this->mcp_phase_ == McpPhase::FETCHING_TOOLS) {
    // tools/list response: store the raw JSON for schema conversion later,
    // and extract tool names into the routing map.
    // Skip this during re-init (routes are already built; we only needed
    // to refresh the session ID).
    if (!this->tools_reinit_) {
      this->raw_tools_per_server_.push_back(json_str);
      JsonArray tools = result["tools"].as<JsonArray>();
      if (!tools.isNull()) {
        for (JsonObject tool : tools) {
          const char *name = tool["name"];
          if (name != nullptr && name[0] != '\0') {
            this->tool_routes_.push_back({name, this->mcp_route_server_index_});
            ESP_LOGV(TAG, "MCP route: %s -> server %u", name,
                     (unsigned) this->mcp_route_server_index_);
          }
        }
      }
    }
    this->mcp_route_server_index_++;
    // Set call type for the next server: initialize if not yet initialized
    // (and not stateless), tools/list otherwise.
    if (this->mcp_route_server_index_ < this->mcp_servers_.size()) {
      const auto &next_srv = this->mcp_servers_[this->mcp_route_server_index_];
      this->current_mcp_call_type_ = (next_srv.initialized || next_srv.stateless)
                                         ? McpCallType::TOOLS_LIST
                                         : McpCallType::INITIALIZE;
    }
    this->set_state_(State::FETCHING_TOOLS);
  } else {
    // tools/call response: extract result text (all text content blocks).
    JsonArray content = result["content"].as<JsonArray>();
    std::string text_result;
    if (!content.isNull()) {
      for (JsonObject item : content) {
        const char *type = item["type"];
        if (type != nullptr && strcmp(type, "text") == 0) {
          const char *text = item["text"];
          if (text != nullptr) {
            if (!text_result.empty()) {
              text_result += "\n";
            }
            text_result += text;
          }
        }
      }
    }
    // Truncate large results (e.g. Wikipedia articles).
    if (text_result.size() > MAX_TOOL_RESULT_BYTES) {
      text_result.resize(MAX_TOOL_RESULT_BYTES);
    }
    bool is_error = result["isError"] | false;
    const auto &tool = this->accumulated_tool_calls_[this->current_tool_index_];
    if (is_error) {
      text_result = "tool error: " + text_result;
    }
    ESP_LOGV(TAG, "Tool %s result (%u chars): %.100s", tool.name.c_str(),
             (unsigned) text_result.size(), text_result.c_str());
    this->append_tool_result_message_(tool.id, text_result);
    this->current_tool_index_++;
    this->set_state_(State::EXECUTING_TOOLS);
  }
}

void OpenAIResponses::build_cached_tools_json_() {
  // Convert raw MCP tools/list responses into a Responses-API-format tools JSON
  // array (flatter than the Chat Completions shape).
  // MCP shape:        {"name":"X","description":"Y","inputSchema":{...}}
  // Responses shape:  {"type":"function","name":"X","description":"Y","parameters":{...}}
  std::string out = "[";
  bool first = true;

  for (const auto &raw : this->raw_tools_per_server_) {
    ESP_LOGV(TAG, "Parsing raw tools JSON (%u bytes)", (unsigned) raw.size());
#ifdef USE_PSRAM
    PsRamJsonAllocator psram_alloc;
    JsonDocument doc(&psram_alloc);
#else
    JsonDocument doc;
#endif
    DeserializationError err =
        deserializeJson(doc, raw.c_str(), raw.size(),
                        DeserializationOption::NestingLimit(20));
    if (err != DeserializationError::Ok) {
      ESP_LOGE(TAG, "Raw tools JSON parse failed: %s", err.c_str());
      continue;
    }
    JsonArray tools = doc["result"]["tools"].as<JsonArray>();
    if (tools.isNull()) {
      continue;
    }
    for (JsonObject tool : tools) {
      const char *name = tool["name"];
      if (name == nullptr || name[0] == '\0') {
        continue;
      }
      const char *description = tool["description"];

      // Build the flattened Responses-shape tool object, then serialize.
#ifdef USE_PSRAM
      PsRamJsonAllocator out_psram_alloc;
      JsonDocument out_doc(&out_psram_alloc);
#else
      JsonDocument out_doc;
#endif
      JsonObject func_obj = out_doc.to<JsonObject>();
      func_obj["type"] = "function";
      func_obj["name"] = name;
      if (description != nullptr) {
        func_obj["description"] = description;
      }
      // Move inputSchema -> parameters (copy the subtree).
      JsonObject input_schema = tool["inputSchema"].as<JsonObject>();
      if (!input_schema.isNull()) {
        func_obj["parameters"] = input_schema;
      }

      std::string serialized;
      serialized.reserve(512);
      serializeJson(out_doc, serialized);

      if (!first) {
        out += ",";
      }
      first = false;
      out += serialized;
    }
  }
  out += "]";
  this->cached_tools_json_ = std::move(out);
  this->raw_tools_per_server_.clear();
  this->tools_cache_timestamp_ms_ = millis();
  ESP_LOGV(TAG, "Cached tools JSON: %u bytes, %u routes",
           (unsigned) this->cached_tools_json_.size(),
           (unsigned) this->tool_routes_.size());
}
#endif  // USE_OPENAI_RESPONSES_MCP

void OpenAIResponses::loop() {
  switch (this->state_) {
    case State::IDLE:
      break;

    case State::STARTING_TURN: {
      // Deferred from request_start() so the on_start callback + MWW stop +
      // buffer reset run in our own loop context, not inside MWW's wake-word
      // trigger. continue_request_start_() transitions to STARTING_MICROPHONE.
      this->continue_request_start_();
      break;
    }

    case State::STARTING_MICROPHONE: {
      // Start the mic source; once it is running, begin listening. The
      // MicrophoneSource::start() is non-blocking (it signals the mic to
      // start), and is_running() reflects the mic task state.
      this->start_microphone_();
      if (this->mic_source_ == nullptr || !this->mic_source_->is_running()) {
        // Not running yet; retry next loop pass. (The mic may take a few
        // passes to start on some platforms.)
        break;
      }
      // CRITICAL: set state to LISTENING before firing display callbacks.
      // The mic data callback only writes to the ring buffer when
      // state_ == LISTENING || state_ == RECORDING. If we fire the display
      // update (which blocks ~580ms for a full framebuffer redraw) while
      // still in STARTING_MICROPHONE, all audio during that window is
      // discarded. By switching to LISTENING first, the ring buffer captures
      // audio while the display redraws in this same loop pass.
      this->vad_last_check_ms_ = millis();
      this->speech_active_ = false;
      this->speech_ended_ = false;
      this->speech_onset_ms_ = 0;
      this->silence_since_ms_ = 0;
      // Reset the request target so BUILDING_REQUEST picks the right endpoint
      // (CHAT or STT). This is important after FETCHING_TOOLS, which leaves
      // request_target_ set to MCP_CALL.
      this->request_target_ = RequestTarget::NONE;
      this->set_state_(State::LISTENING);
      // Fire on_start + on_wake_word_detected + on_listening now. The display
      // update blocks loop() for ~580ms, but the mic callback is capturing
      // audio into the ring buffer during that time (state is already
      // LISTENING). VAD will process the buffered audio on the next pass.
      // Only on_listening fires a display update — on_start's display was
      // removed (it showed the same page as on_listening).
      this->on_start_cb_.call();
      if (!this->wake_word_.empty()) {
        this->on_wake_word_detected_cb_.call();
      }
      this->on_listening_cb_.call();
      break;
    }

    case State::LISTENING:
    case State::RECORDING: {
      // Drain mic data from the ring buffer into the recording buffer and
      // run the VAD. This does one bounded chunk per pass.
      this->drain_ring_buffer_to_recording_();

      // Promote LISTENING -> RECORDING once speech onset is confirmed.
      if (this->state_ == State::LISTENING && this->speech_active_) {
        this->set_state_(State::RECORDING);
      }

      // End of speech: stop the mic and build the request.
      if (this->speech_ended_) {
        this->speech_ended_ = false;
        this->set_state_(State::STOPPING_MICROPHONE);
        break;
      }

      // No-speech timeout in LISTENING: silently return to idle (a user
      // cancel, not an error).
      if (this->state_ == State::LISTENING &&
          (millis() - this->listening_start_ms_) >= this->max_recording_ms_) {
        ESP_LOGD(TAG, "No speech detected within max_recording_ms; returning to idle");
        this->set_state_(State::STOPPING_MICROPHONE);
        // Mark as a silent teardown so STOPPING_MICROPHONE -> idle (not error).
        this->response_text_.clear();
        break;
      }

      // Recording length safety cap.
      if (this->state_ == State::RECORDING && this->recording_size_ >= this->recording_capacity_) {
        ESP_LOGW(TAG, "Recording buffer full; forcing end of speech");
        this->speech_ended_ = true;
      }
      break;
    }

    case State::STOPPING_MICROPHONE: {
      // Stop the mic; once stopped, build the next request body.
      this->stop_microphone_();
      if (this->mic_source_ != nullptr && this->mic_source_->is_running()) {
        break;  // wait for the mic task to stop
      }
      // If we got here via no-speech timeout with no recording, tear down.
      if (this->recording_size_ == 0) {
        this->set_state_(State::ERROR_TEARDOWN);
        break;
      }
      this->set_state_(State::BUILDING_REQUEST);
      break;
    }

    case State::BUILDING_REQUEST: {
      // Build the next request body based on the current target. The initial
      // target is CHAT (multimodal) or STT (Mode 2).
      if (this->request_target_ == RequestTarget::NONE) {
        if (this->multimodal_) {
          this->request_target_ = RequestTarget::CHAT;
          this->build_chat_request_body_multimodal_();
        } else {
          this->request_target_ = RequestTarget::STT;
          this->build_stt_multipart_body_();
        }
      } else if (this->request_target_ == RequestTarget::CHAT) {
        // Built inline at the STT->CHAT transition (see READING_STT); nothing
        // to do here except proceed to send.
      }
      if (this->request_body_ == nullptr) {
        // build_* already called fail_().
        break;
      }
      this->set_state_(State::SENDING_REQUEST);
      break;
    }

    case State::SENDING_REQUEST: {
      // Start the HTTP task which handles: init, open, write body, fetch
      // headers, read response. All blocking I/O runs on the task; the main
      // loop just drains the message buffer in the READING_* states.
      // Reset SSE line buffer for a fresh chat read.
      this->sse_line_len_ = 0;
      this->sse_event_type_.clear();
      this->speech_ended_ = false;
      if (this->request_target_ == RequestTarget::CHAT) {
        this->response_text_.clear();
        this->reasoning_text_.clear();
#ifdef USE_OPENAI_RESPONSES_MCP
        this->reset_accumulated_tool_calls_();
        this->had_tool_calls_ = false;
#endif
      } else if (this->request_target_ == RequestTarget::STT) {
        this->stt_response_text_.clear();
#ifdef USE_OPENAI_RESPONSES_MCP
      } else if (this->request_target_ == RequestTarget::MCP_CALL) {
        this->mcp_response_text_.clear();
#endif
      }
      // TTS: no reset needed — the task handles speaker start, WAV header
      // skip, and volume internally.

      this->start_http_task_();
      if (this->state_ != State::SENDING_REQUEST) {
        break;  // start_http_task_ called fail_()
      }

      // Transition immediately to the appropriate read state. The task will
      // send DATA messages as response data arrives, DONE on completion, or
      // ERROR on failure.
      if (this->request_target_ == RequestTarget::CHAT) {
        this->set_state_(State::READING_CHAT);
      } else if (this->request_target_ == RequestTarget::STT) {
        this->set_state_(State::READING_STT);
      } else if (this->request_target_ == RequestTarget::TTS) {
        this->set_state_(State::READING_TTS);
#ifdef USE_OPENAI_RESPONSES_MCP
      } else if (this->request_target_ == RequestTarget::MCP_CALL) {
        this->set_state_(State::READING_MCP);
#endif
      }
      break;
    }

    case State::READING_STT: {
      // Mode 2: drain the message buffer for the transcription JSON response.
      uint8_t recv_buf[HTTP_TASK_READ_CHUNK + 1];
      size_t received = xMessageBufferReceive(this->http_msg_buffer_, recv_buf, sizeof(recv_buf), 0);
      if (received == 0) {
        break;  // no data yet, try next loop pass
      }
      HttpMsgType type = (HttpMsgType) recv_buf[0];
      if (type == HttpMsgType::DATA) {
        this->process_stt_response_(recv_buf + 1, received - 1);
        break;  // keep draining
      }
      // DONE or ERROR: the HTTP task has finished. Clean up and process.
      this->stop_http_task_();
      if (type == HttpMsgType::ERROR_) {
        size_t off = 1;
        uint8_t code_len = recv_buf[off++];
        std::string code((const char *) (recv_buf + off), code_len);
        off += code_len;
        uint8_t msg_len = recv_buf[off++];
        std::string message((const char *) (recv_buf + off), msg_len);
        this->fail_(code, message);
        break;
      }
      // DONE: parse the full STT response.
      if (this->stt_response_text_.empty()) {
        this->fail_("stt_empty", "Empty STT response");
        break;
      }
      auto doc = json::parse_json(this->stt_response_text_);
      const char *text = doc["text"];
      std::string transcript = (text != nullptr) ? std::string(text) : std::string();
      if (transcript.empty()) {
        this->fail_("stt_no_text", "STT response had no text field");
        break;
      }
      ESP_LOGD(TAG, "STT transcript: %s", transcript.c_str());
      this->request_target_ = RequestTarget::CHAT;
      this->build_chat_request_body_text_(transcript);
      if (this->request_body_ == nullptr) {
        break;  // fail_ already called
      }
      // Start the chat HTTP task BEFORE firing on_stt_end. The HTTP task runs
      // on a separate FreeRTOS task, so once start_http_task_() returns, the
      // network request is in flight. We can then fire on_stt_end (which
      // redraws the display, ~412ms blocking) in parallel with the HTTP
      // request — saving ~412ms on the critical path.
      // We must do the SENDING_REQUEST resets + start_http_task_ here rather
      // than in the SENDING_REQUEST state because the display callback blocks
      // loop() and the SENDING_REQUEST case wouldn't run until after it.
      this->sse_line_len_ = 0;
      this->sse_event_type_.clear();
      this->speech_ended_ = false;
      this->response_text_.clear();
      this->reasoning_text_.clear();
#ifdef USE_OPENAI_RESPONSES_MCP
      this->reset_accumulated_tool_calls_();
      this->had_tool_calls_ = false;
#endif
      this->set_state_(State::SENDING_REQUEST);
      this->start_http_task_();
      if (this->state_ != State::SENDING_REQUEST) {
        break;  // start_http_task_ called fail_()
      }
      // Transition immediately to READING_CHAT. The HTTP task is now in
      // flight on its own FreeRTOS task, reading the SSE response into the
      // message buffer. The main loop will drain it.
      this->set_state_(State::READING_CHAT);
      // Fire on_stt_end now — the display update (~412ms) overlaps with the
      // chat HTTP request running on the HTTP task.
      this->on_stt_end_cb_.call(transcript);
#ifdef USE_TEXT_SENSOR
      if (this->text_request_sensor_ != nullptr) {
        this->text_request_sensor_->publish_state(transcript);
      }
#endif
      break;
    }

    case State::READING_CHAT: {
      // Drain the message buffer for SSE chunks and accumulate the chat
      // response text.
      uint8_t recv_buf[HTTP_TASK_READ_CHUNK + 1];
      size_t received = xMessageBufferReceive(this->http_msg_buffer_, recv_buf, sizeof(recv_buf), 0);
      if (received == 0) {
        break;  // no data yet, try next loop pass
      }
      HttpMsgType type = (HttpMsgType) recv_buf[0];
      if (type == HttpMsgType::DATA) {
        this->process_sse_bytes_(recv_buf + 1, received - 1);
        break;  // keep draining
      }
      // TTS_STREAM_START can arrive during READING_CHAT (the feeder task
      // starts the speaker while SSE is still streaming).
      if (type == HttpMsgType::TTS_STREAM_START) {
        this->on_tts_stream_start_cb_.call();
        break;  // keep draining SSE messages
      }
      // DONE or ERROR: the HTTP task has finished. Clean up and process.
      this->stop_http_task_();
      if (type == HttpMsgType::ERROR_) {
        size_t off = 1;
        uint8_t code_len = recv_buf[off++];
        std::string code((const char *) (recv_buf + off), code_len);
        off += code_len;
        uint8_t msg_len = recv_buf[off++];
        std::string message((const char *) (recv_buf + off), msg_len);
        this->fail_(code, message);
        break;
      }
      // DONE: stream ended (response.completed or EOF).
#ifdef USE_OPENAI_RESPONSES_MCP
      // Check if the LLM requested tool execution. If so, do NOT TTS or use
      // reasoning — route to the tool execution state instead. The Responses
      // API signals this via function_call items in the response.completed
      // output array (had_tool_calls_) and/or accumulated tool calls.
      if (this->had_tool_calls_ || this->accumulated_tool_call_count_ > 0) {
        if (this->mcp_servers_.empty()) {
          this->fail_("no_mcp_servers", "LLM requested tool calls but no MCP servers configured");
          break;
        }
        // Abort any streaming TTS that may have started on preamble text.
        if (this->tts_streaming_active_) {
          this->stop_tts_producer_task_();
          this->stop_speaker_feeder_task_();
          this->tts_queue_.clear();
          this->tts_queue_done_ = false;
          this->tts_streaming_active_ = false;
          this->tts_audio_producer_done_ = false;
          this->tts_audio_write_offset_ = 0;
          this->tts_audio_read_offset_ = 0;
          this->tts_pending_text_.clear();
        }
        this->tool_round_++;
        if (this->tool_round_ > MAX_TOOL_ROUNDS) {
          this->fail_("max_tool_rounds", "Exceeded maximum tool call rounds");
          break;
        }
        ESP_LOGD(TAG, "Tool calls requested (round %u, %u tools, response_id=%s)",
                 (unsigned) this->tool_round_, (unsigned) this->accumulated_tool_call_count_,
                 this->previous_response_id_.c_str());
        // Retain base64 audio (multimodal) only for the fallback path (when
        // previous_response_id_ is empty). On the previous_response_id path the
        // server keeps the context, so we don't resend the audio.
        if (this->previous_response_id_.empty() && this->multimodal_ &&
            this->retained_b64_audio_ == nullptr) {
          this->retain_b64_audio_(this->round1_b64_offset_, this->round1_b64_len_);
        }
        // No assistant tool_calls message is appended — the server tracks it
        // via previous_response_id_. The fallback path rebuilds context from
        // scratch (it doesn't need the assistant message either, since the
        // function_call_output items reference the call_id which the server
        // saw in this response).
        // Clear response/reasoning — they are not spoken when tools are called.
        this->response_text_.clear();
        this->reasoning_text_.clear();
        // Routes were already built during FETCHING_TOOLS; go straight to execution.
        this->current_tool_index_ = 0;
        this->mcp_phase_ = McpPhase::EXECUTING;
        this->on_tool_start_cb_.call();
        this->set_state_(State::EXECUTING_TOOLS);
        break;
      }
#endif
      // No tool calls: normal response handling.
      // If no content was received (some non-standard servers send all text
      // via delta.reasoning with content=null), fall back to reasoning_text_.
      if (this->response_text_.empty()) {
        if (!this->reasoning_text_.empty()) {
          ESP_LOGV(TAG, "No content received; using reasoning as response (%u chars)",
                   (unsigned) this->reasoning_text_.size());
          this->response_text_ = std::move(this->reasoning_text_);
#ifdef USE_TEXT_SENSOR
          if (this->text_response_sensor_ != nullptr) {
            this->text_response_sensor_->publish_state(this->response_text_);
          }
#endif
        } else {
          // Empty response with no reasoning: use a default acknowledgment
          // instead of erroring. This handles broadcast/tool scenarios where
          // the LLM has nothing to say after executing a tool.
          this->response_text_ = "Done.";
#ifdef USE_TEXT_SENSOR
          if (this->text_response_sensor_ != nullptr) {
            this->text_response_sensor_->publish_state(this->response_text_);
          }
#endif
        }
      }
      // Strip markdown formatting from the full response text for display.
      this->response_text_ = strip_markdown_(this->response_text_);
      // Check for tool-call text leak — if the response is a leaked tool call,
      // don't TTS it. Use "Done." as a fallback acknowledgment.
      if (is_tool_call_leak_(this->response_text_)) {
        ESP_LOGW(TAG, "Tool call text leak in full response, replacing with Done.");
        this->response_text_ = "Done.";
      }
#ifdef USE_TEXT_SENSOR
      if (this->text_response_sensor_ != nullptr) {
        this->text_response_sensor_->publish_state(this->response_text_);
      }
#endif
      ESP_LOGD(TAG, "Chat response (%u chars): %s", (unsigned) this->response_text_.size(),
               this->response_text_.c_str());

      // --- Streaming TTS path ---
      // If sentences were pushed to the TTS queue during SSE streaming,
      // flush any remaining text and transition to STREAMING_TTS_DRAIN to
      // wait for the feeder task to finish playing.
      if (this->tts_streaming_active_) {
        this->flush_tts_pending_();
        this->set_state_(State::STREAMING_TTS_DRAIN);
        break;
      }

      // --- Non-streaming fallback (streaming_tts: false or no text was
      // streamed) ---
      // Build the TTS request and start the HTTP task BEFORE firing
      // on_tts_start. The HTTP task runs on a separate FreeRTOS task, so once
      // start_http_task_() returns, the network request is in flight. We can
      // then fire on_tts_start (which redraws the display, ~200ms blocking) in
      // parallel with the HTTP request — same pattern as on_stt_end above.
      this->request_target_ = RequestTarget::TTS;
      this->build_tts_request_body_();
      if (this->request_body_ == nullptr) {
        break;  // fail_ already called
      }
      this->set_state_(State::SENDING_REQUEST);
      this->start_http_task_();
      if (this->state_ != State::SENDING_REQUEST) {
        break;  // start_http_task_ called fail_()
      }
      // Transition immediately to READING_TTS. The HTTP task is now in flight
      // on its own FreeRTOS task, fetching the TTS audio into the speaker.
      this->set_state_(State::READING_TTS);
      // Fire on_tts_start now — the display update overlaps with the TTS HTTP
      // request running on the HTTP task.
      this->on_tts_start_cb_.call(this->response_text_);
      break;
    }

    case State::STREAMING_TTS_DRAIN: {
      // SSE done; TTS producer + feeder still draining the PSRAM ring buffer.
      // Wait for TTS_STREAM_DONE from the feeder task.
      uint8_t recv_buf[128];
      size_t received = xMessageBufferReceive(this->http_msg_buffer_, recv_buf, sizeof(recv_buf), 0);
      if (received == 0) {
        break;  // no message yet
      }
      HttpMsgType type = (HttpMsgType) recv_buf[0];
      if (type == HttpMsgType::TTS_STREAM_START) {
        this->on_tts_stream_start_cb_.call();
        break;
      }
      if (type == HttpMsgType::ERROR_) {
        size_t off = 1;
        uint8_t code_len = recv_buf[off++];
        std::string code((const char *) (recv_buf + off), code_len);
        off += code_len;
        uint8_t msg_len = recv_buf[off++];
        std::string message((const char *) (recv_buf + off), msg_len);
        this->stop_tts_producer_task_();
        this->stop_speaker_feeder_task_();
        this->fail_(code, message);
        break;
      }
      // TTS_STREAM_DONE: all audio has been played by the feeder.
      this->stop_tts_producer_task_();
      this->stop_speaker_feeder_task_();
      this->on_tts_stream_end_cb_.call();
      this->on_tts_end_cb_.call(this->response_text_);
      this->set_state_(State::DRAINING_AUDIO);
      break;
    }

    case State::READING_TTS: {
      // For TTS, the HTTP task feeds the speaker directly (play() with
      // portMAX_DELAY for natural backpressure). The main loop only handles
      // control messages: TTS_STREAM_START (fire callback), DONE (finish
      // speaker + drain), ERROR (fail). No DATA messages for TTS.
      uint8_t recv_buf[128];  // small — only control messages, no data
      size_t received = xMessageBufferReceive(this->http_msg_buffer_, recv_buf, sizeof(recv_buf), 0);
      if (received == 0) {
        break;  // no message yet, try next loop pass
      }
      HttpMsgType type = (HttpMsgType) recv_buf[0];

      if (type == HttpMsgType::TTS_STREAM_START) {
        // The task has started the speaker and is about to feed audio.
        this->on_tts_stream_start_cb_.call();
        break;
      }

      // DONE or ERROR: the TTS stream has ended.
      this->stop_http_task_();
      if (type == HttpMsgType::ERROR_) {
        size_t off = 1;
        uint8_t code_len = recv_buf[off++];
        std::string code((const char *) (recv_buf + off), code_len);
        off += code_len;
        uint8_t msg_len = recv_buf[off++];
        std::string message((const char *) (recv_buf + off), msg_len);
        this->fail_(code, message);
        break;
      }
      // DONE: the task has fed all audio to the speaker. Tell the speaker to
      // drain its internal buffer and stop when empty.
      if (this->speaker_ != nullptr) {
        this->speaker_->finish();
      }
      this->on_tts_stream_end_cb_.call();
      this->on_tts_end_cb_.call(this->response_text_);
      this->set_state_(State::DRAINING_AUDIO);
      break;
    }

#ifdef USE_OPENAI_RESPONSES_MCP
    case State::FETCHING_TOOLS: {
      // Proactive tools cache refresh (before a turn starts). For each MCP
      // server: (1) send `initialize` if not yet initialized, (2) send
      // `notifications/initialized`, (3) send `tools/list`.
      // For re-init (mid-turn session refresh), only query the one failed server.
      bool server_in_range =
          this->mcp_route_server_index_ < this->mcp_servers_.size() &&
          (!this->tools_reinit_ || this->mcp_route_server_index_ <= this->reinit_server_index_);
      if (server_in_range) {
        auto &srv = this->mcp_servers_[this->mcp_route_server_index_];
        this->current_mcp_url_ = srv.url;
        this->current_mcp_auth_ = srv.auth_header;
        // Set the session ID from the server's stored value (empty if not
        // yet initialized — the http_task captures it from the response).
        this->current_mcp_session_id_ = srv.session_id;

        // Determine which call to make next.
        std::string body;
        if (!srv.initialized && !srv.stateless) {
          // Server hasn't been initialized and isn't known to be stateless.
          if (this->current_mcp_call_type_ == McpCallType::INITIALIZE) {
            body = mcp_build_initialize_request(this->mcp_request_id_++);
            ESP_LOGV(TAG, "Initializing MCP server %u (%s)",
                     (unsigned) this->mcp_route_server_index_, srv.name.c_str());
          } else if (this->current_mcp_call_type_ == McpCallType::INITIALIZED_NOTIF) {
            body = mcp_build_initialized_notification();
            ESP_LOGV(TAG, "Sending notifications/initialized to server %u",
                     (unsigned) this->mcp_route_server_index_);
          }
        } else {
          // Already initialized or stateless: send tools/list.
          this->current_mcp_call_type_ = McpCallType::TOOLS_LIST;
          body = mcp_build_tools_list_request(this->mcp_request_id_++);
          ESP_LOGV(TAG, "Fetching tools from MCP server %u (%s)",
                   (unsigned) this->mcp_route_server_index_, srv.name.c_str());
        }

        ExternalRAMAllocator<uint8_t> ext(ExternalRAMAllocator<uint8_t>::ALLOC_EXTERNAL);
        if (this->request_body_ != nullptr) {
          ext.deallocate(this->request_body_, this->request_body_capacity_);
        }
        this->request_body_capacity_ = body.size() + 1;
        this->request_body_ = ext.allocate(this->request_body_capacity_);
        if (this->request_body_ == nullptr) {
          this->fail_("alloc_failed", "Could not allocate MCP body");
          break;
        }
        memcpy(this->request_body_, body.data(), body.size());
        this->request_body_size_ = body.size();
        this->request_body_sent_ = 0;
        this->request_target_ = RequestTarget::MCP_CALL;
        this->set_state_(State::SENDING_REQUEST);
        break;
      }
      // All servers queried.
      this->mcp_phase_ = McpPhase::IDLE;
      if (this->tools_reinit_) {
        // Re-initialization complete: just return to EXECUTING_TOOLS and
        // retry the tool call. No need to rebuild the cache — the tools
        // list hasn't changed, only the session was refreshed.
        ESP_LOGV(TAG, "Re-init complete; retrying tool call");
        this->tools_reinit_ = false;
        this->mcp_phase_ = McpPhase::EXECUTING;
        this->set_state_(State::EXECUTING_TOOLS);
      } else {
        // Full refresh: build the cached tools JSON + routing map.
        this->build_cached_tools_json_();
        if (this->tools_prefetch_) {
          ESP_LOGV(TAG, "Tools prefetch complete; returning to idle");
          this->tools_prefetch_ = false;
          this->set_state_(State::IDLE);
        } else {
          // Mid-turn refresh: continue the request_start that was deferred.
          this->continue_request_start_();
        }
      }
      break;
    }

    case State::EXECUTING_TOOLS: {
      // Routes were already built during FETCHING_TOOLS; go straight to execution.
      if (this->current_tool_index_ < this->accumulated_tool_call_count_) {
        const auto &tool = this->accumulated_tool_calls_[this->current_tool_index_];
        // Look up the server for this tool.
        uint8_t server_idx = 0;
        bool found = false;
        for (const auto &route : this->tool_routes_) {
          if (route.tool_name == tool.name) {
            server_idx = route.server_index;
            found = true;
            break;
          }
        }
        if (!found) {
          ESP_LOGW(TAG, "Tool not found in routes: %s", tool.name.c_str());
          this->append_tool_result_message_(tool.id, "tool not found: " + tool.name);
          this->current_tool_index_++;
          break;  // re-enter next loop pass
        }
        const auto &srv = this->mcp_servers_[server_idx];
        this->current_mcp_url_ = srv.url;
        this->current_mcp_auth_ = srv.auth_header;
        std::string body =
            mcp_build_tools_call_request(this->mcp_request_id_++, tool.name, tool.arguments);
        ExternalRAMAllocator<uint8_t> ext(ExternalRAMAllocator<uint8_t>::ALLOC_EXTERNAL);
        if (this->request_body_ != nullptr) {
          ext.deallocate(this->request_body_, this->request_body_capacity_);
        }
        this->request_body_capacity_ = body.size() + 1;
        this->request_body_ = ext.allocate(this->request_body_capacity_);
        if (this->request_body_ == nullptr) {
          this->fail_("alloc_failed", "Could not allocate MCP tools/call body");
          break;
        }
        memcpy(this->request_body_, body.data(), body.size());
        this->request_body_size_ = body.size();
        this->request_body_sent_ = 0;
        this->request_target_ = RequestTarget::MCP_CALL;
        this->mcp_phase_ = McpPhase::EXECUTING;
        this->current_mcp_call_type_ = McpCallType::TOOLS_CALL;
        // Restore the session ID for this server (so the http_task includes
        // the Mcp-Session-Id header on the tools/call request).
        this->current_mcp_session_id_ = srv.session_id;
        ESP_LOGV(TAG, "Calling tool %s on server %u (%s)", tool.name.c_str(),
                 (unsigned) server_idx, srv.name.c_str());
        this->set_state_(State::SENDING_REQUEST);
        break;
      }

      // All tools executed for this round. Build round 2+ chat request.
      // NOTE: reset_accumulated_tool_calls_() is called AFTER
      // build_chat_request_body_from_history_() because the fallback path
      // (no previous_response_id) needs the accumulated tool calls to build
      // function_call items for the input array.
      this->current_tool_index_ = 0;
      this->build_chat_request_body_from_history_();
      this->reset_accumulated_tool_calls_();
      if (this->request_body_ == nullptr) {
        break;  // fail_ already called
      }
      this->request_target_ = RequestTarget::CHAT;
      ESP_LOGV(TAG, "Re-querying LLM with tool results (round %u, %u messages)",
               (unsigned) this->tool_round_, (unsigned) this->turn_messages_.size());
      this->set_state_(State::SENDING_REQUEST);
      break;
    }

    case State::READING_MCP: {
      // Drain MCP JSON-RPC response chunks into mcp_response_text_.
      uint8_t recv_buf[HTTP_TASK_READ_CHUNK + 1];
      size_t received = xMessageBufferReceive(this->http_msg_buffer_, recv_buf, sizeof(recv_buf), 0);
      if (received == 0) {
        break;  // no data yet
      }
      HttpMsgType type = (HttpMsgType) recv_buf[0];
      if (type == HttpMsgType::DATA) {
        this->mcp_response_text_.append(reinterpret_cast<const char *>(recv_buf + 1), received - 1);
        break;  // keep draining
      }
      // DONE or ERROR
      this->stop_http_task_();
      if (type == HttpMsgType::ERROR_) {
        size_t off = 1;
        uint8_t code_len = recv_buf[off++];
        std::string code((const char *) (recv_buf + off), code_len);
        off += code_len;
        uint8_t msg_len = recv_buf[off++];
        std::string message((const char *) (recv_buf + off), msg_len);
        if (this->mcp_phase_ == McpPhase::FETCHING_TOOLS) {
          // Server unreachable during tools/list refresh: skip it and continue.
          ESP_LOGW(TAG, "MCP server %u unreachable during fetch: %s %s",
                   (unsigned) this->mcp_route_server_index_, code.c_str(), message.c_str());
          this->mcp_route_server_index_++;
          this->set_state_(State::FETCHING_TOOLS);
        } else if (this->mcp_phase_ == McpPhase::EXECUTING &&
                   this->current_tool_index_ < this->accumulated_tool_call_count_) {
          // Check if this is a 400 from a stateful server whose session may
          // have expired. If so, re-initialize and retry the tool call.
          const auto &tool = this->accumulated_tool_calls_[this->current_tool_index_];
          uint8_t server_idx = 0;
          bool found = false;
          for (const auto &route : this->tool_routes_) {
            if (route.tool_name == tool.name) {
              server_idx = route.server_index;
              found = true;
              break;
            }
          }
          if (found && code == "http_status" && message.find("400") != std::string::npos &&
              !this->mcp_servers_[server_idx].stateless &&
              !this->mcp_servers_[server_idx].session_id.empty()) {
            // Session likely expired: reset and re-initialize.
            ESP_LOGW(TAG, "MCP server %u returned 400; re-initializing (session may have expired)",
                     (unsigned) server_idx);
            this->mcp_servers_[server_idx].initialized = false;
            this->mcp_servers_[server_idx].session_id.clear();
            // Set up to re-initialize this server, then retry the tool call.
            this->mcp_route_server_index_ = server_idx;
            this->reinit_server_index_ = server_idx;
            this->current_mcp_call_type_ = McpCallType::INITIALIZE;
            this->mcp_phase_ = McpPhase::FETCHING_TOOLS;
            this->tools_reinit_ = true;
            this->set_state_(State::FETCHING_TOOLS);
          } else {
            // Other HTTP errors: return the error as a tool result so the
            // LLM can recover gracefully.
            this->append_tool_result_message_(tool.id, "tool backend error: " + code + " " + message);
            this->current_tool_index_++;
            this->set_state_(State::EXECUTING_TOOLS);
          }
        } else {
          this->fail_(code, message);
        }
        break;
      }
      // DONE: parse the MCP response.
      this->process_mcp_response_();
      break;
    }
#endif

    case State::DRAINING_AUDIO: {
      // Wait for the speaker to finish playing buffered audio before tearing
      // down. The i2s bus is shared with the mic, so we cannot restart MWW
      // until the speaker has fully stopped.
      if (this->speaker_ != nullptr && !this->speaker_->is_stopped()) {
        break;
      }
      this->teardown_to_idle_();
      break;
    }

    case State::ERROR_TEARDOWN: {
      // Cleanup path for both errors and explicit stops. Kill the HTTP task
      // if still running, stop the speaker + mic, then wait for them to stop
      // before tearing down fully.
      // stop_http_task_() is non-blocking: returns false if the task hasn't
      // suspended yet (blocked in recv_tcp). Retry on the next loop pass.
      if (!this->stop_http_task_()) {
        break;  // retry next pass — task is still exiting
      }
      // Stop streaming TTS tasks if active (the feeder calls play() on the
      // speaker, so it must be stopped before we stop the speaker). Stop the
      // feeder first (consumer) so the producer (which may be blocked on a
      // full buffer) can exit.
      if (this->tts_streaming_active_ || this->tts_producer_task_.is_created() ||
          this->speaker_feeder_task_.is_created()) {
        this->stop_speaker_feeder_task_();
        this->stop_tts_producer_task_();
      }
      if (this->speaker_ != nullptr && !this->speaker_->is_stopped()) {
        this->speaker_->stop();
        break;  // wait for speaker to stop (shared i2s bus)
      }
      // Stop the mic if it is still running (e.g. button pressed during
      // LISTENING/RECORDING). mic->stop() is asynchronous — we must wait for
      // is_running() to return false before starting MWW, otherwise MWW's
      // mic->start() races with the ongoing stop and the mic ends up in a
      // broken state where MWW reads silence forever.
      if (this->mic_source_ != nullptr && this->mic_source_->is_running()) {
        this->stop_microphone_();
        break;  // wait for mic to stop (next loop pass re-enters ERROR_TEARDOWN)
      }
      this->teardown_to_idle_();
      break;
    }
  }
}

}  // namespace esphome::openai_responses

#endif  // USE_OPENAI_RESPONSES



