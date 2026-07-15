#include "openai_conversations.h"

#ifdef USE_OPENAI_CONVERSATIONS

#include "esphome/core/defines.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/core/util.h"

#include "esphome/components/json/json_util.h"

#ifdef USE_OPENAI_CONVERSATIONS_TOOLS
#include "openai_conversations_tools.h"  // generated header with TOOLS_JSON (raw string literal)
#endif
#ifdef USE_OPENAI_CONVERSATIONS_MCP
#include "esphome/components/openai_common/mcp_client.h"
#endif

#include <esp_crt_bundle.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace esphome::openai_conversations {

static const char *const TAG = "openai_conversations";

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
// A single SSE line buffer this large is more than enough for one delta chunk
// (OpenAI caps SSE event lines well below this).
static constexpr size_t SSE_LINE_MAX = 8192;

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

// --- Lifecycle -------------------------------------------------------------

OpenAIConversations::OpenAIConversations() = default;

OpenAIConversations::~OpenAIConversations() {
  // Kill the HTTP task if still running and clean up its resources.
  this->stop_http_task_();
  this->deallocate_buffers_();
  // Free the pre-allocated message buffer (only in full teardown, not per-turn).
  if (this->http_msg_buffer_ != nullptr) {
    vMessageBufferDelete(this->http_msg_buffer_);
    this->http_msg_buffer_ = nullptr;
  }
}

void OpenAIConversations::setup() {
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
}

float OpenAIConversations::get_setup_priority() const { return setup_priority::AFTER_CONNECTION; }

void OpenAIConversations::dump_config() {
  this->OpenAIHTTPBase::dump_config();
  ESP_LOGCONFIG(TAG, "OpenAI Conversations:");
  if (this->multimodal_) {
    ESP_LOGCONFIG(TAG, "  Mode: multimodal (audio sent to chat model)");
  } else {
    ESP_LOGCONFIG(TAG, "  Mode: STT + LLM (stt_model=%s)", this->stt_model_.c_str());
  }
  ESP_LOGCONFIG(TAG, "  TTS model: %s voice=%s sample_rate: %" PRIu32,
                this->tts_model_.c_str(), this->tts_voice_.c_str(), this->tts_sample_rate_);
  ESP_LOGCONFIG(TAG, "  Tools: %s", this->has_tools_ ? "yes" : "no");
#ifdef USE_OPENAI_CONVERSATIONS_MCP
  ESP_LOGCONFIG(TAG, "  MCP servers: %u", (unsigned) this->mcp_servers_.size());
  for (const auto &srv : this->mcp_servers_) {
    ESP_LOGCONFIG(TAG, "    %s: %s", srv.name.c_str(), srv.url.c_str());
  }
  ESP_LOGCONFIG(TAG, "  Tools cache TTL: %" PRIu32 " ms", this->tools_cache_ttl_ms_);
#endif
}

// --- Buffer allocation ------------------------------------------------------

bool OpenAIConversations::allocate_buffers_() {
  if (!this->OpenAIHTTPBase::allocate_buffers_()) {
    return false;
  }
  // Conversations-specific buffers (all in OpenAIHTTPBase now, but kept for
  // any conversations-specific additions). The base allocates ring, recording,
  // speaker, and SSE buffers.
  return true;
}

void OpenAIConversations::deallocate_buffers_() {
  // Base class frees ring, recording, speaker, SSE, and request body buffers.
  this->OpenAIHTTPBase::deallocate_buffers_();

  // Conversations-specific variable-size buffers.
  this->response_text_.clear();
  this->response_text_.shrink_to_fit();
  this->reasoning_text_.clear();
  this->reasoning_text_.shrink_to_fit();
  this->stt_response_text_.clear();
  this->stt_response_text_.shrink_to_fit();
#ifdef USE_OPENAI_CONVERSATIONS_MCP
  ExternalRAMAllocator<char> ext_char(ExternalRAMAllocator<char>::ALLOC_EXTERNAL);
  if (this->retained_b64_audio_ != nullptr) {
    ext_char.deallocate(this->retained_b64_audio_, this->retained_b64_capacity_);
    this->retained_b64_audio_ = nullptr;
  }
  this->retained_b64_len_ = 0;
  this->retained_b64_capacity_ = 0;
  this->turn_messages_.clear();
  this->turn_messages_.shrink_to_fit();
  this->mcp_response_text_.clear();
  this->mcp_response_text_.shrink_to_fit();
  this->user_text_.clear();
  this->user_text_.shrink_to_fit();
  this->raw_tools_per_server_.clear();
  this->raw_tools_per_server_.shrink_to_fit();
  this->reset_accumulated_tool_calls_();
  this->finish_reason_.clear();
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
#endif
}

void OpenAIConversations::reset_turn_state_() {
  // Base class resets recording_size, sse_line_len, speaker_buffer_index,
  // tts_header_skipped_, request_body, and VAD state.
  this->OpenAIHTTPBase::reset_turn_state_();

  // Conversations-specific turn state.
  this->tts_stream_started_ = false;

  this->response_text_.clear();
  this->response_text_.shrink_to_fit();
  this->reasoning_text_.clear();
  this->reasoning_text_.shrink_to_fit();
  this->stt_response_text_.clear();
  this->stt_response_text_.shrink_to_fit();
#ifdef USE_OPENAI_CONVERSATIONS_MCP
  ExternalRAMAllocator<char> ext_char(ExternalRAMAllocator<char>::ALLOC_EXTERNAL);
  if (this->retained_b64_audio_ != nullptr) {
    ext_char.deallocate(this->retained_b64_audio_, this->retained_b64_capacity_);
    this->retained_b64_audio_ = nullptr;
  }
  this->retained_b64_len_ = 0;
  this->retained_b64_capacity_ = 0;
  this->turn_messages_.clear();
  this->turn_messages_.shrink_to_fit();
  this->mcp_response_text_.clear();
  this->mcp_response_text_.shrink_to_fit();
  this->user_text_.clear();
  this->user_text_.shrink_to_fit();
  this->raw_tools_per_server_.clear();
  this->raw_tools_per_server_.shrink_to_fit();
  this->reset_accumulated_tool_calls_();
  this->finish_reason_.clear();
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
#endif
}

// --- State / mic helpers ----------------------------------------------------

void OpenAIConversations::set_state_(State s) {
  if (this->state_ != s) {
    ESP_LOGD(TAG, "State %u -> %u", (unsigned) this->state_, (unsigned) s);
    this->state_ = s;
  }
}

void OpenAIConversations::start_microphone_() {
  if (this->mic_source_ != nullptr) {
    this->mic_source_->start();
  }
}

void OpenAIConversations::stop_microphone_() {
  if (this->mic_source_ != nullptr) {
    this->mic_source_->stop();
  }
}

// --- Public API ------------------------------------------------------------

void OpenAIConversations::request_start(bool silence_detection) {
  if (this->state_ != State::IDLE) {
    ESP_LOGW(TAG, "Cannot start while not idle (state=%u)", (unsigned) this->state_);
    return;
  }
  ESP_LOGV(TAG, "request_start (silence_detection=%d)", silence_detection ? 1 : 0);

  this->silence_detection_ = silence_detection;
  this->response_text_.clear();
  this->reasoning_text_.clear();
  this->stt_response_text_.clear();
#ifdef USE_OPENAI_CONVERSATIONS_MCP
  this->reset_accumulated_tool_calls_();
  this->finish_reason_.clear();
  this->turn_messages_.clear();
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

#ifdef USE_OPENAI_CONVERSATIONS_MCP
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

void OpenAIConversations::continue_request_start_() {
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

#ifdef USE_OPENAI_CONVERSATIONS_MCP
void OpenAIConversations::prefetch_tools() {
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

void OpenAIConversations::prewarm_models_task_(void *arg) {
  auto *self = static_cast<OpenAIConversations *>(arg);
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

void OpenAIConversations::prewarm_models() {
  // Spawn a short-lived FreeRTOS task so the synchronous HTTP calls don't
  // block the main loop. The task self-terminates after all models are loaded.
  // Uses xTaskCreate (dynamic allocation) since this is a one-shot boot task.
  xTaskCreate(prewarm_models_task_, "oai_prewarm", HTTP_TASK_STACK_SIZE, this,
              HTTP_TASK_PRIORITY, nullptr);
}

void OpenAIConversations::request_stop() {
  if (this->state_ == State::IDLE) {
    return;
  }
  ESP_LOGV(TAG, "request_stop (state=%u)", (unsigned) this->state_);
  // An explicit stop is not an error: teardown without firing on_error.
  this->set_state_(State::ERROR_TEARDOWN);
}

void OpenAIConversations::fail_(const std::string &code, const std::string &message) {
  ESP_LOGE(TAG, "Error: %s - %s", code.c_str(), message.c_str());
  this->on_error_cb_.call(code, message);
  this->set_state_(State::ERROR_TEARDOWN);
}

void OpenAIConversations::teardown_to_idle_() {
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

  // Let the base reset its shared per-turn members (speaker buffer index,
  // tts_header_skipped_, etc.).
  this->OpenAIBase::teardown_to_idle_();

  this->set_state_(State::IDLE);
}

// --- OpenAIHTTPBase virtual hooks ------------------------------------------

bool OpenAIConversations::is_active_() const { return this->state_ != State::IDLE; }

void OpenAIConversations::on_http_header_(const char *key, const char *value) {
#ifdef USE_OPENAI_CONVERSATIONS_MCP
  if (strcasecmp(key, "Mcp-Session-Id") == 0) {
    this->current_mcp_session_id_ = value;
    ESP_LOGV(TAG, "Captured Mcp-Session-Id: %s", this->current_mcp_session_id_.c_str());
  }
#else
  (void) key;
  (void) value;
#endif
}

bool OpenAIConversations::build_http_url_and_content_type_(char *url, size_t buf_size,
                                                           const char *&content_type) const {
  content_type = "application/json";
  if (this->request_target_ == RequestTarget::CHAT) {
    snprintf(url, buf_size, "%s/v1/chat/completions", this->endpoint_base_.c_str());
  } else if (this->request_target_ == RequestTarget::STT) {
    snprintf(url, buf_size, "%s/v1/audio/transcriptions", this->endpoint_base_.c_str());
    content_type = "multipart/form-data; boundary=----esphome_openai_conv";
  } else if (this->request_target_ == RequestTarget::TTS) {
    snprintf(url, buf_size, "%s/v1/audio/speech", this->endpoint_base_.c_str());
#ifdef USE_OPENAI_CONVERSATIONS_MCP
  } else if (this->request_target_ == RequestTarget::MCP_CALL) {
    snprintf(url, buf_size, "%s", this->current_mcp_url_.c_str());
#endif
  } else {
    return false;
  }
  return true;
}

void OpenAIConversations::set_http_extra_headers_(esp_http_client_handle_t client) {
#ifdef USE_OPENAI_CONVERSATIONS_MCP
  if (this->request_target_ == RequestTarget::MCP_CALL) {
    esp_http_client_set_header(client, "Accept", "application/json, text/event-stream");
    if (!this->current_mcp_auth_.empty()) {
      esp_http_client_set_header(client, "Authorization", this->current_mcp_auth_.c_str());
    }
    if (!this->current_mcp_session_id_.empty()) {
      esp_http_client_set_header(client, "Mcp-Session-Id", this->current_mcp_session_id_.c_str());
    }
    return;
  }
#endif
  if (!this->auth_header_value_.empty()) {
    esp_http_client_set_header(client, "Authorization", this->auth_header_value_.c_str());
  }
}

bool OpenAIConversations::is_http_status_acceptable_(int status) const {
  if (status == 200) {
    return true;
  }
#ifdef USE_OPENAI_CONVERSATIONS_MCP
  if (status == 202 && this->request_target_ == RequestTarget::MCP_CALL &&
      this->current_mcp_call_type_ == McpCallType::INITIALIZED_NOTIF) {
    return true;
  }
#endif
  return false;
}

bool OpenAIConversations::http_feeds_speaker_() const {
  return this->request_target_ == RequestTarget::TTS;
}

void OpenAIConversations::http_feed_speaker_(esp_http_client_handle_t client) {
  bool speaker_started = false;
  size_t header_skip = 44;
  uint8_t read_buf[HTTP_TASK_READ_CHUNK];

  while (!this->http_task_should_exit_) {
    int got = esp_http_client_read(client, reinterpret_cast<char *>(read_buf), HTTP_TASK_READ_CHUNK);
    if (got < 0) {
      ESP_LOGE(TAG, "esp_http_client_read failed (TTS)");
      this->close_http_();
      this->send_http_error_(this->http_msg_buffer_, "http_read", "Failed to read TTS response");
      return;
    }
    if (got == 0) {
      break;
    }

    size_t off = 0;

    if (!speaker_started) {
      speaker_started = true;
      if (this->speaker_ != nullptr) {
        this->speaker_->set_audio_stream_info(
            audio::AudioStreamInfo(MIC_BITS_PER_SAMPLE, MIC_CHANNELS, this->tts_sample_rate_));
        this->speaker_->start();
      }
      uint8_t start_msg = (uint8_t) HttpMsgType::TTS_STREAM_START;
      xMessageBufferSend(this->http_msg_buffer_, &start_msg, 1, portMAX_DELAY);
    }

    while (off < (size_t) got && header_skip > 0) {
      size_t to_skip = (header_skip < (size_t) got - off) ? header_skip : ((size_t) got - off);
      off += to_skip;
      header_skip -= to_skip;
    }

    if (off < (size_t) got) {
      this->feed_speaker_(read_buf + off, (size_t) got - off);
    }
  }
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

void OpenAIConversations::build_chat_request_body_multimodal_() {
  // Builds the chat-completions JSON for Mode 1 (multimodal). The user content
  // is an audio_url data URL containing a base64-encoded WAV (header + PCM).
  //
  //   {"model":"<chat_model>","stream":true,
  //    "messages":[{"role":"system","content":"<system_prompt>"},
  //                {"role":"user","content":[
  //                  {"type":"text","text":""},
  //                  {"type":"audio_url","audio_url":{"url":"data:audio/wav;base64,<b64>"}}]}]
  //    ,"tool_choice":"auto"
  //    [,"tools":[...]]}
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

  // 2) Compute sizes for the JSON prefix/suffix and the base64 payload.
  //    JSON-escape the system prompt and chat model inline by copying char by
  //    char into a local std::string (these are small).
  std::string escaped_prompt;
  escaped_prompt.reserve(this->system_prompt_.size() + 16);
  for (char c : this->system_prompt_) {
    if (c == '"' || c == '\\') {
      escaped_prompt.push_back('\\');
      escaped_prompt.push_back(c);
    } else if (c == '\n') {
      escaped_prompt.push_back('\\');
      escaped_prompt.push_back('n');
    } else if (c == '\r') {
      escaped_prompt.push_back('\\');
      escaped_prompt.push_back('r');
    } else if (c == '\t') {
      escaped_prompt.push_back('\\');
      escaped_prompt.push_back('t');
    } else if ((unsigned char) c < 0x20) {
      continue;  // drop other control chars
    } else {
      escaped_prompt.push_back(c);
    }
  }

  std::string escaped_model;
  escaped_model.reserve(this->chat_model_.size() + 8);
  for (char c : this->chat_model_) {
    if (c == '"' || c == '\\') {
      escaped_model.push_back('\\');
    }
    escaped_model.push_back(c);
  }

  // Prefix: everything up to the base64 audio data.
  // Per the OpenAI Chat Completions spec, the audio content part is:
  //   {"type":"input_audio","input_audio":{"data":"<base64>","format":"wav"}}
  // (The "audio_url"/data-URL format used by the Responses API does NOT exist
  // in the Chat Completions API — see openapi.yaml schema
  // ChatCompletionContentPartInputAudio.)
  std::string prefix =
      "{\"model\":\"" + escaped_model + "\",\"stream\":true,\"messages\":["
      "{\"role\":\"system\",\"content\":\"" + escaped_prompt + "\"},"
      "{\"role\":\"user\",\"content\":["
      "{\"type\":\"text\",\"text\":\"\"},"
      "{\"type\":\"input_audio\",\"input_audio\":{\"data\":\"";

  // Suffix: closes the data field, sets the format, closes the content array,
  // the messages array, and the object. Includes tool_choice and optionally
  // the tools array.
  std::string suffix = "\",\"format\":\"wav\"}}]}],\"tool_choice\":\"auto\"";
#ifdef USE_OPENAI_CONVERSATIONS_MCP
  if (!this->cached_tools_json_.empty()) {
    suffix += ",\"tools\":" + this->cached_tools_json_;
  } else
#endif
#ifdef USE_OPENAI_CONVERSATIONS_TOOLS
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
#ifdef USE_OPENAI_CONVERSATIONS_MCP
  // Save the base64 offset + length so we can retain it for round 2+ if the
  // LLM requests tool execution.
  this->round1_b64_offset_ = prefix.size();
  this->round1_b64_len_ = encoded;
#endif

  ESP_LOGV(TAG, "Built multimodal chat body: %u bytes (wav=%u b64=%u)",
           (unsigned) this->request_body_size_, (unsigned) wav_len, (unsigned) encoded);
}

void OpenAIConversations::build_chat_request_body_text_(const std::string &user_text) {
  // Mode 2 chat request: text-only user message.
  //   {"model":"<chat_model>","stream":true,"messages":[
  //     {"role":"system","content":"<system_prompt>"},
  //     {"role":"user","content":"<transcript>"}],
  //    "tool_choice":"auto"[,"tools":[...]]}
#ifdef USE_OPENAI_CONVERSATIONS_MCP
  this->user_text_ = user_text;  // saved for round 2+ re-inclusion
#endif
  std::string escaped_prompt;
  escaped_prompt.reserve(this->system_prompt_.size() + 16);
  for (char c : this->system_prompt_) {
    if (c == '"' || c == '\\') {
      escaped_prompt.push_back('\\');
      escaped_prompt.push_back(c);
    } else if (c == '\n') {
      escaped_prompt.push_back('\\');
      escaped_prompt.push_back('n');
    } else if (c == '\r') {
      escaped_prompt.push_back('\\');
      escaped_prompt.push_back('r');
    } else if (c == '\t') {
      escaped_prompt.push_back('\\');
      escaped_prompt.push_back('t');
    } else if ((unsigned char) c < 0x20) {
      continue;
    } else {
      escaped_prompt.push_back(c);
    }
  }
  std::string escaped_text;
  escaped_text.reserve(user_text.size() + 16);
  for (char c : user_text) {
    if (c == '"' || c == '\\') {
      escaped_text.push_back('\\');
      escaped_text.push_back(c);
    } else if (c == '\n') {
      escaped_text.push_back('\\');
      escaped_text.push_back('n');
    } else if (c == '\r') {
      escaped_text.push_back('\\');
      escaped_text.push_back('r');
    } else if (c == '\t') {
      escaped_text.push_back('\\');
      escaped_text.push_back('t');
    } else if ((unsigned char) c < 0x20) {
      continue;
    } else {
      escaped_text.push_back(c);
    }
  }
  std::string body =
      "{\"model\":\"" + this->chat_model_ + "\",\"stream\":true,\"messages\":["
      "{\"role\":\"system\",\"content\":\"" + escaped_prompt + "\"},"
      "{\"role\":\"user\",\"content\":\"" + escaped_text + "\"}],"
      "\"tool_choice\":\"auto\"";
#ifdef USE_OPENAI_CONVERSATIONS_MCP
  if (!this->cached_tools_json_.empty()) {
    body += ",\"tools\":" + this->cached_tools_json_;
  } else
#endif
#ifdef USE_OPENAI_CONVERSATIONS_TOOLS
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

void OpenAIConversations::build_stt_multipart_body_() {
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

void OpenAIConversations::build_tts_request_body_() {
  // POST /v1/audio/speech body:
  //   {"model":"<tts_model>","input":"<response_text>","voice":"<tts_voice>","response_format":"wav"}
  std::string escaped_input;
  escaped_input.reserve(this->response_text_.size() + 16);
  for (char c : this->response_text_) {
    if (c == '"' || c == '\\') {
      escaped_input.push_back('\\');
      escaped_input.push_back(c);
    } else if (c == '\n') {
      escaped_input.push_back('\\');
      escaped_input.push_back('n');
    } else if (c == '\r') {
      escaped_input.push_back('\\');
      escaped_input.push_back('r');
    } else if (c == '\t') {
      escaped_input.push_back('\\');
      escaped_input.push_back('t');
    } else if ((unsigned char) c < 0x20) {
      continue;
    } else {
      escaped_input.push_back(c);
    }
  }
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

// --- Response processing ---------------------------------------------------

void OpenAIConversations::process_sse_line_(const char *line, size_t len) {
  if (len == 0) {
    return;  // blank line separates events
  }
  // Lines we care about start with "data: ".
  const char *prefix = "data: ";
  const size_t prefix_len = 6;
  if (len < prefix_len || strncmp(line, prefix, prefix_len) != 0) {
    return;  // ignore comments, event:, id:, etc.
  }
  const char *payload = line + prefix_len;
  size_t payload_len = len - prefix_len;

  // Termination sentinel.
  if (payload_len == 6 && strncmp(payload, "[DONE]", 6) == 0) {
    ESP_LOGV(TAG, "SSE [DONE] received");
    this->speech_ended_ = true;  // reuse as "stream done" signal for the chat read loop
    return;
  }

  // Parse the JSON delta chunk. The Chat Completions SSE format is:
  //   {"choices":[{"delta":{"content":"Hello"}}]}
  //
  // Standard servers send the response text via delta.content. Some
  // non-standard servers (e.g. reasoning models via llama.cpp/GGUF) also send
  // delta.reasoning — the model's internal thinking, which should NOT be
  // spoken. We accumulate them separately:
  //   - delta.content → response_text_ (the actual response to speak)
  //   - delta.reasoning → reasoning_text_ (internal thinking, discarded unless
  //     no content was ever received, in which case reasoning is the fallback)
  ESP_LOGV(TAG, "SSE line (%u bytes): %s", (unsigned) payload_len, payload);
  auto doc = json::parse_json(reinterpret_cast<const uint8_t *>(payload), payload_len);
  if (doc.isNull()) {
    ESP_LOGW(TAG, "SSE JSON parse failed (%u bytes)", (unsigned) payload_len);
    return;
  }
  JsonObject root = doc.as<JsonObject>();
  JsonArray choices = root["choices"].as<JsonArray>();
  if (choices.isNull() || choices.size() == 0) {
    ESP_LOGW(TAG, "SSE: no choices in chunk");
    return;
  }
  JsonObject first_choice = choices[0].as<JsonObject>();

  // Capture finish_reason early — it may appear on a chunk with no delta.
#ifdef USE_OPENAI_CONVERSATIONS_MCP
  const char *finish_reason = first_choice["finish_reason"];
  if (finish_reason != nullptr && finish_reason[0] != '\0') {
    this->finish_reason_ = finish_reason;
    ESP_LOGV(TAG, "SSE finish_reason: %s", finish_reason);
  }
#endif

  JsonObject delta = first_choice["delta"].as<JsonObject>();
  if (delta.isNull()) {
    ESP_LOGV(TAG, "SSE: no delta in choice");
    return;
  }
  // Accumulate delta.content into response_text_ (the text to speak).
  const char *content = delta["content"];
  if (content != nullptr && content[0] != '\0') {
    this->response_text_.append(content);
#ifdef USE_TEXT_SENSOR
    if (this->text_response_sensor_ != nullptr) {
      this->text_response_sensor_->publish_state(this->response_text_);
    }
#endif
  }
  // Accumulate delta.reasoning into reasoning_text_ (internal thinking).
  // Not published to the text sensor — the user should not see thinking.
  // Used as a fallback at stream end only if no content was received.
  const char *reasoning = delta["reasoning"];
  if (reasoning != nullptr && reasoning[0] != '\0') {
    this->reasoning_text_.append(reasoning);
  }
#ifdef USE_OPENAI_CONVERSATIONS_MCP
  // Accumulate delta.tool_calls (fragmented across SSE chunks). Each element
  // is keyed by "index": the first chunk for an index carries id +
  // function.name, subsequent chunks append to function.arguments (partial
  // JSON string — do NOT parse until stream end).
  JsonArray tool_calls = delta["tool_calls"].as<JsonArray>();
  if (!tool_calls.isNull()) {
    for (JsonObject tc : tool_calls) {
      int index = tc["index"] | 0;
      if (index < 0 || index >= (int) MAX_PARALLEL_TOOLS) {
        continue;
      }
      // Find existing entry with this index, or create a new one.
      AccumulatedToolCall *acc = nullptr;
      for (size_t i = 0; i < this->accumulated_tool_call_count_; i++) {
        if (this->accumulated_tool_calls_[i].index == index) {
          acc = &this->accumulated_tool_calls_[i];
          break;
        }
      }
      if (acc == nullptr && this->accumulated_tool_call_count_ < MAX_PARALLEL_TOOLS) {
        acc = &this->accumulated_tool_calls_[this->accumulated_tool_call_count_++];
        acc->index = index;
      }
      if (acc == nullptr) {
        continue;
      }
      // Capture id (first chunk only).
      const char *id = tc["id"];
      if (id != nullptr && id[0] != '\0') {
        acc->id = id;
      }
      // Capture function.name + arguments.
      JsonObject function = tc["function"].as<JsonObject>();
      if (!function.isNull()) {
        const char *name = function["name"];
        if (name != nullptr && name[0] != '\0') {
          acc->name = name;
        }
        const char *args = function["arguments"];
        if (args != nullptr) {
          acc->arguments.append(args);
        }
      }
    }
  }
#else
  // Without MCP support, tool_calls can't be executed. Log and move on.
  const char *finish_reason = first_choice["finish_reason"];
  if (finish_reason != nullptr && strcmp(finish_reason, "tool_calls") == 0) {
    ESP_LOGW(TAG, "Server returned finish_reason=tool_calls; no MCP servers configured");
  }
#endif
}

void OpenAIConversations::process_stt_response_(const uint8_t *data, size_t len) {
  // Append to the accumulating STT response text; the full body is parsed once
  // the stream ends (handled in loop READING_STT).
  this->stt_response_text_.append(reinterpret_cast<const char *>(data), len);
}

// --- Speaker output --------------------------------------------------------

// --- Main loop state machine ------------------------------------------------

#ifdef USE_OPENAI_CONVERSATIONS_MCP
// --- Tool execution helpers -------------------------------------------------

void OpenAIConversations::retain_b64_audio_(size_t offset, size_t len) {
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

void OpenAIConversations::append_assistant_tool_calls_message_() {
  std::string msg = "{\"role\":\"assistant\",\"tool_calls\":[";
  bool first = true;
  for (size_t i = 0; i < this->accumulated_tool_call_count_; i++) {
    const auto &tc = this->accumulated_tool_calls_[i];
    if (tc.index < 0) {
      continue;
    }
    if (!first) {
      msg += ",";
    }
    first = false;
    msg += "{\"id\":\"" + escape_json_string_(tc.id) + "\"";
    msg += ",\"type\":\"function\"";
    msg += ",\"function\":{\"name\":\"" + escape_json_string_(tc.name) + "\"";
    msg += ",\"arguments\":\"" + escape_json_string_(tc.arguments) + "\"}}";
  }
  msg += "]}";
  this->turn_messages_.push_back(std::move(msg));
}

void OpenAIConversations::reset_accumulated_tool_calls_() {
  for (size_t i = 0; i < this->accumulated_tool_call_count_; i++) {
    this->accumulated_tool_calls_[i].index = -1;
    this->accumulated_tool_calls_[i].id.clear();
    this->accumulated_tool_calls_[i].name.clear();
    this->accumulated_tool_calls_[i].arguments.clear();
  }
  this->accumulated_tool_call_count_ = 0;
}

void OpenAIConversations::append_tool_result_message_(const std::string &tool_call_id,
                                                        const std::string &result) {
  std::string msg = "{\"role\":\"tool\",\"tool_call_id\":\"" + escape_json_string_(tool_call_id);
  msg += "\",\"content\":\"" + escape_json_string_(result) + "\"}";
  this->turn_messages_.push_back(std::move(msg));
}

void OpenAIConversations::build_chat_request_body_from_history_() {
  std::string escaped_prompt = escape_json_string_(this->system_prompt_);
  std::string escaped_model = escape_json_string_(this->chat_model_);

  // Suffix: closes messages array + tool_choice + tools.
  std::string suffix = "],\"tool_choice\":\"auto\"";
#ifdef USE_OPENAI_CONVERSATIONS_MCP
  if (!this->cached_tools_json_.empty()) {
    suffix += ",\"tools\":" + this->cached_tools_json_;
  } else
#endif
#ifdef USE_OPENAI_CONVERSATIONS_TOOLS
  if (this->has_tools_) {
    suffix += ",\"tools\":" + std::string(TOOLS_JSON);
  }
#endif
  suffix += "}";

  if (this->multimodal_ && this->retained_b64_audio_ != nullptr) {
    // Multimodal: user message contains the retained base64 audio.
    std::string prefix =
        "{\"model\":\"" + escaped_model + "\",\"stream\":true,\"messages\":["
        "{\"role\":\"system\",\"content\":\"" + escaped_prompt + "\"},"
        "{\"role\":\"user\",\"content\":["
        "{\"type\":\"text\",\"text\":\"\"},"
        "{\"type\":\"input_audio\",\"input_audio\":{\"data\":\"";
    std::string user_suffix = "\",\"format\":\"wav\"}}]}";

    // Turn messages (assistant + tool messages from previous rounds).
    std::string turn_msgs;
    for (const auto &msg : this->turn_messages_) {
      turn_msgs += "," + msg;
    }

    size_t b64_len = this->retained_b64_len_;
    size_t total = prefix.size() + b64_len + user_suffix.size() + turn_msgs.size() + suffix.size();

    ExternalRAMAllocator<uint8_t> ext(ExternalRAMAllocator<uint8_t>::ALLOC_EXTERNAL);
    if (this->request_body_ != nullptr) {
      ext.deallocate(this->request_body_, this->request_body_capacity_);
    }
    this->request_body_capacity_ = total + 1;
    this->request_body_ = ext.allocate(this->request_body_capacity_);
    if (this->request_body_ == nullptr) {
      this->fail_("alloc_failed", "Could not allocate chat history body");
      return;
    }

    size_t off = 0;
    memcpy(this->request_body_ + off, prefix.data(), prefix.size());
    off += prefix.size();
    memcpy(this->request_body_ + off, this->retained_b64_audio_, b64_len);
    off += b64_len;
    memcpy(this->request_body_ + off, user_suffix.data(), user_suffix.size());
    off += user_suffix.size();
    memcpy(this->request_body_ + off, turn_msgs.data(), turn_msgs.size());
    off += turn_msgs.size();
    memcpy(this->request_body_ + off, suffix.data(), suffix.size());
    off += suffix.size();

    this->request_body_size_ = off;
    this->request_body_sent_ = 0;
    ESP_LOGV(TAG, "Built chat history body (multimodal): %u bytes (b64=%u msgs=%u)",
             (unsigned) this->request_body_size_, (unsigned) b64_len,
             (unsigned) this->turn_messages_.size());
  } else {
    // Text mode: user message is text.
    std::string escaped_text = escape_json_string_(this->user_text_);
    std::string body =
        "{\"model\":\"" + escaped_model + "\",\"stream\":true,\"messages\":["
        "{\"role\":\"system\",\"content\":\"" + escaped_prompt + "\"},"
        "{\"role\":\"user\",\"content\":\"" + escaped_text + "\"}";
    for (const auto &msg : this->turn_messages_) {
      body += "," + msg;
    }
    body += suffix;

    ExternalRAMAllocator<uint8_t> ext(ExternalRAMAllocator<uint8_t>::ALLOC_EXTERNAL);
    if (this->request_body_ != nullptr) {
      ext.deallocate(this->request_body_, this->request_body_capacity_);
    }
    this->request_body_capacity_ = body.size() + 1;
    this->request_body_ = ext.allocate(this->request_body_capacity_);
    if (this->request_body_ == nullptr) {
      this->fail_("alloc_failed", "Could not allocate chat history body");
      return;
    }
    memcpy(this->request_body_, body.data(), body.size());
    this->request_body_size_ = body.size();
    this->request_body_sent_ = 0;
    ESP_LOGV(TAG, "Built chat history body (text): %u bytes (msgs=%u)",
             (unsigned) this->request_body_size_, (unsigned) this->turn_messages_.size());
  }
}

std::string OpenAIConversations::extract_mcp_json_() {
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

void OpenAIConversations::process_mcp_response_() {
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

void OpenAIConversations::build_cached_tools_json_() {
  // Convert raw MCP tools/list responses into an OpenAI-format tools JSON array.
  // MCP shape:      {"name":"X","description":"Y","inputSchema":{...}}
  // OpenAI shape:   {"type":"function","function":{"name":"X","description":"Y","parameters":{...}}}
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

      // Build the OpenAI-shape tool object using ArduinoJson, then serialize.
#ifdef USE_PSRAM
      PsRamJsonAllocator out_psram_alloc;
      JsonDocument out_doc(&out_psram_alloc);
#else
      JsonDocument out_doc;
#endif
      JsonObject func_obj = out_doc.to<JsonObject>();
      func_obj["type"] = "function";
      JsonObject function = func_obj["function"].to<JsonObject>();
      function["name"] = name;
      if (description != nullptr) {
        function["description"] = description;
      }
      // Move inputSchema -> parameters (copy the subtree).
      JsonObject input_schema = tool["inputSchema"].as<JsonObject>();
      if (!input_schema.isNull()) {
        function["parameters"] = input_schema;
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
#endif  // USE_OPENAI_CONVERSATIONS_MCP

void OpenAIConversations::loop() {
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
      this->speech_ended_ = false;
      if (this->request_target_ == RequestTarget::CHAT) {
        this->response_text_.clear();
        this->reasoning_text_.clear();
#ifdef USE_OPENAI_CONVERSATIONS_MCP
        this->reset_accumulated_tool_calls_();
        this->finish_reason_.clear();
#endif
      } else if (this->request_target_ == RequestTarget::STT) {
        this->stt_response_text_.clear();
#ifdef USE_OPENAI_CONVERSATIONS_MCP
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
#ifdef USE_OPENAI_CONVERSATIONS_MCP
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
      this->speech_ended_ = false;
      this->response_text_.clear();
      this->reasoning_text_.clear();
#ifdef USE_OPENAI_CONVERSATIONS_MCP
      this->reset_accumulated_tool_calls_();
      this->finish_reason_.clear();
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
      // DONE: stream ended (either [DONE] or EOF).
#ifdef USE_OPENAI_CONVERSATIONS_MCP
      // Check if the LLM requested tool execution. If so, do NOT TTS or use
      // reasoning — route to the tool execution state instead.
      if (this->finish_reason_ == "tool_calls" || this->accumulated_tool_call_count_ > 0) {
        if (this->mcp_servers_.empty()) {
          this->fail_("no_mcp_servers", "LLM requested tool calls but no MCP servers configured");
          break;
        }
        this->tool_round_++;
        if (this->tool_round_ > MAX_TOOL_ROUNDS) {
          this->fail_("max_tool_rounds", "Exceeded maximum tool call rounds");
          break;
        }
        ESP_LOGD(TAG, "Tool calls requested (round %u, %u tools)",
                 (unsigned) this->tool_round_, (unsigned) this->accumulated_tool_call_count_);
        this->on_tool_start_cb_.call();
        // Retain base64 audio (multimodal) so round 2+ can re-include it.
        if (this->multimodal_ && this->retained_b64_audio_ == nullptr) {
          this->retain_b64_audio_(this->round1_b64_offset_, this->round1_b64_len_);
        }
        // Build the assistant message with tool_calls and add to history.
        this->append_assistant_tool_calls_message_();
        // Clear response/reasoning — they are not spoken when tools are called.
        this->response_text_.clear();
        this->reasoning_text_.clear();
        // Routes were already built during FETCHING_TOOLS; go straight to execution.
        this->current_tool_index_ = 0;
        this->mcp_phase_ = McpPhase::EXECUTING;
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
      ESP_LOGD(TAG, "Chat response (%u chars): %s", (unsigned) this->response_text_.size(),
               this->response_text_.c_str());
      this->on_tts_start_cb_.call(this->response_text_);
      // Build the TTS request and send it.
      this->request_target_ = RequestTarget::TTS;
      this->build_tts_request_body_();
      if (this->request_body_ == nullptr) {
        break;  // fail_ already called
      }
      this->set_state_(State::SENDING_REQUEST);
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

#ifdef USE_OPENAI_CONVERSATIONS_MCP
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
      this->reset_accumulated_tool_calls_();
      this->current_tool_index_ = 0;
      this->build_chat_request_body_from_history_();
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
      this->stop_http_task_();
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

}  // namespace esphome::openai_conversations

#endif  // USE_OPENAI_CONVERSATIONS



