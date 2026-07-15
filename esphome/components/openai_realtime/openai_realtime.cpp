#include "openai_realtime.h"
#include "esphome/core/defines.h"

#ifdef USE_OPENAI_REALTIME

#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/components/json/json_util.h"

#include <algorithm>
#include <cinttypes>
#include <climits>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#ifdef USE_OPENAI_REALTIME_TOOLS
#include "openai_realtime_tools.h"
#endif

namespace esphome::openai_realtime {

static const char *const TAG = "openai_realtime";

static const size_t SAMPLE_RATE_HZ = 16000;
static const size_t RING_BUFFER_SAMPLES = 512 * SAMPLE_RATE_HZ / 1000;  // 512 ms
static const size_t RING_BUFFER_SIZE = RING_BUFFER_SAMPLES * sizeof(int16_t);
static const size_t SEND_BUFFER_SAMPLES = 32 * SAMPLE_RATE_HZ / 1000;  // 32 ms
static const size_t SEND_BUFFER_SIZE = SEND_BUFFER_SAMPLES * sizeof(int16_t);
static const size_t MAX_CHUNKS_PER_LOOP = 4;
static const size_t AUDIO_APPEND_JSON_BUFFER_SIZE = 2048;

static const size_t MAX_JSON_FRAME_SIZE = 262144;  // 256 KB — handles large audio delta frames
static const size_t MAX_PENDING_JSON_MESSAGES = 64;

static const uint32_t CONNECTION_TIMEOUT_MS = 25000;
static const uint32_t NO_SPEECH_TIMEOUT_MS = 5000;
static const uint32_t RESPONSE_TIMEOUT_MS = 30000;  // inactivity timer; resets on each response event

static const uint32_t AUDIO_STATS_LOG_INTERVAL_MS = 1000;

static const char BASE64_ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t base64_encode_to(const uint8_t *data, size_t len, char *out, size_t out_len) {
  size_t needed = 4 * ((len + 2) / 3);
  if (out_len < needed + 1) {
    return 0;
  }
  size_t i = 0, j = 0;
  while (i < len) {
    uint32_t a = i < len ? data[i++] : 0;
    uint32_t b = i < len ? data[i++] : 0;
    uint32_t c = i < len ? data[i++] : 0;
    uint32_t triple = (a << 16) | (b << 8) | c;
    out[j++] = BASE64_ALPHABET[(triple >> 18) & 0x3F];
    out[j++] = BASE64_ALPHABET[(triple >> 12) & 0x3F];
    out[j++] = BASE64_ALPHABET[(triple >> 6) & 0x3F];
    out[j++] = BASE64_ALPHABET[triple & 0x3F];
  }
  for (size_t pad = 0; pad < (3 - (len % 3)) % 3; pad++) {
    out[j - 1 - pad] = '=';
  }
  out[j] = '\0';
  return j;
}

OpenAIRealtime::OpenAIRealtime() = default;

OpenAIRealtime::~OpenAIRealtime() {
  this->disconnect_();
  if (this->rx_message_buffer_ != nullptr) {
    free(this->rx_message_buffer_);
    this->rx_message_buffer_ = nullptr;
  }
  // Free all queued audio deltas
  for (auto &delta : this->audio_delta_queue_) {
    if (delta.data != nullptr) {
      free(delta.data);
    }
  }
  this->audio_delta_queue_.clear();
}

void OpenAIRealtime::setup() {
  this->mic_source_->add_data_callback([this](const std::vector<uint8_t> &data) {
    if (this->ring_buffer_ != nullptr) {
      this->ring_buffer_->write(data.data(), data.size());
      this->mic_bytes_received_ += data.size();
    }
  });

#ifdef USE_OPENAI_REALTIME_TOOLS
  if (this->has_tools_) {
    this->tools_json_ = esphome::openai_realtime::TOOLS_JSON;
  }
#endif

  // Allocate the websocket receive buffer in PSRAM. This buffer assembles
  // fragmented JSON frames (up to MAX_JSON_FRAME_SIZE) and is reused for every
  // frame — a single persistent allocation, no per-frame churn. PSRAM is
  // required because audio delta frames can be 100 KB+.
  ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOC_EXTERNAL);
  this->rx_message_buffer_ = allocator.allocate(MAX_JSON_FRAME_SIZE);
  if (this->rx_message_buffer_ == nullptr) {
    ESP_LOGE(TAG, "Could not allocate rx_message buffer in PSRAM (%d bytes)", MAX_JSON_FRAME_SIZE);
    this->mark_failed();
  }
}

float OpenAIRealtime::get_setup_priority() const { return setup_priority::AFTER_CONNECTION; }

void OpenAIRealtime::dump_config() {
  ESP_LOGCONFIG(TAG, "OpenAI Assistant:");
  ESP_LOGCONFIG(TAG, "  Endpoint: %s", this->endpoint_.c_str());
  ESP_LOGCONFIG(TAG, "  Model: %s", this->model_.c_str());
  ESP_LOGCONFIG(TAG, "  Voice: %s", this->voice_.c_str());
  ESP_LOGCONFIG(TAG, "  Language: %s", this->language_.c_str());
  ESP_LOGCONFIG(TAG, "  Noise suppression level: %d", this->noise_suppression_level_);
  ESP_LOGCONFIG(TAG, "  Auto gain: %d dBFS", this->auto_gain_);
  ESP_LOGCONFIG(TAG, "  Volume multiplier: %.2f", this->volume_multiplier_);
  ESP_LOGCONFIG(TAG, "  Tools: %s", this->tools_json_.empty() ? "none" : "configured");
}

bool OpenAIRealtime::allocate_buffers_() {
  if (this->ring_buffer_ == nullptr) {
    this->ring_buffer_ = ring_buffer::RingBuffer::create(
        RING_BUFFER_SIZE, ring_buffer::RingBuffer::MemoryPreference::EXTERNAL_FIRST);
    if (this->ring_buffer_ == nullptr) {
      ESP_LOGE(TAG, "Could not allocate ring buffer");
      return false;
    }
  }

  this->audio_delta_mutex_ = xSemaphoreCreateMutex();
  if (this->audio_delta_mutex_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create audio delta mutex");
    return false;
  }
  if (!this->audio_buffer_.init()) {
    ESP_LOGE(TAG, "Failed to allocate PSRAM audio buffer");
    return false;
  }

  return true;
}

void OpenAIRealtime::clear_buffers_() {
  if (this->ring_buffer_ != nullptr) {
    this->ring_buffer_->reset();
  }
  this->audio_buffer_.reset();
}

void OpenAIRealtime::deallocate_buffers_() {
  this->stop_audio_pipeline_();
  this->ring_buffer_.reset();
  this->audio_buffer_.deinit();
  if (this->audio_delta_mutex_ != nullptr) {
    vSemaphoreDelete(this->audio_delta_mutex_);
    this->audio_delta_mutex_ = nullptr;
  }
  // Free all queued audio deltas
  for (auto &delta : this->audio_delta_queue_) {
    if (delta.data != nullptr) {
      free(delta.data);
    }
  }
  this->audio_delta_queue_.clear();
}

void OpenAIRealtime::connect_() {
  if (this->websocket_ != nullptr) {
    return;
  }

  this->endpoint_with_model_ = this->endpoint_ + "?model=" + this->model_;

  this->headers_.clear();
  if (!this->api_key_.empty()) {
    this->headers_ += "Authorization: Bearer " + this->api_key_ + "\r\n";
  }
  this->headers_ += "OpenAI-Beta: realtime=v1\r\n";

  esp_websocket_client_config_t config = {};
  config.uri = this->endpoint_with_model_.c_str();
  config.headers = this->headers_.c_str();
  config.buffer_size = 4096;
  config.ping_interval_sec = 10;
  config.task_name = "openai_ws";
  config.task_stack = 8192;
  config.task_prio = 5;

  this->websocket_ = esp_websocket_client_init(&config);
  if (this->websocket_ == nullptr) {
    ESP_LOGE(TAG, "Could not initialize websocket client");
    this->on_error_cb_.call("websocket-init-failed", "Could not initialize websocket client");
    this->set_state_(State::IDLE);
    return;
  }

  esp_websocket_register_events(this->websocket_, WEBSOCKET_EVENT_ANY, OpenAIRealtime::websocket_event_handler_,
                                this);

  this->pending_client_connected_ = false;
  this->pending_client_disconnected_ = false;
  this->pending_websocket_error_ = false;
  this->session_configured_ = false;
  this->connected_ = false;

  esp_err_t err = esp_websocket_client_start(this->websocket_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Could not start websocket client: %s", esp_err_to_name(err));
    this->on_error_cb_.call("websocket-start-failed", "Could not start websocket client");
    this->disconnect_();
    this->set_state_(State::IDLE);
    return;
  }

  this->start_connection_timeout_();
}

void OpenAIRealtime::disconnect_() {
  this->cancel_connection_timeout_();
  this->cancel_no_speech_timeout_();
  this->cancel_response_timeout_();

  if (this->websocket_ != nullptr) {
    // Close with a bounded timeout before destroying. esp_websocket_client_destroy()
    // internally calls stop() with portMAX_DELAY, which blocks the main loop indefinitely
    // (tripping the task watchdog → reboot) when the transport is in an error state
    // (e.g. a 404 handshake failure). A finite-timeout close first lets the client
    // shut down its transport cleanly; destroy() then tears down the task without
    // re-attempting a blocking close on a dead transport.
    esp_websocket_client_close(this->websocket_, pdMS_TO_TICKS(1000));
    esp_websocket_client_destroy(this->websocket_);
    this->websocket_ = nullptr;
  }

  this->connected_ = false;
  this->session_configured_ = false;
}

static const char *openai_realtime_state_to_string(State state) {
  switch (state) {
    case State::IDLE:
      return "IDLE";
    case State::CONNECTING:
      return "CONNECTING";
    case State::START_MICROPHONE:
      return "START_MICROPHONE";
    case State::STARTING_MICROPHONE:
      return "STARTING_MICROPHONE";
    case State::STREAMING_MICROPHONE:
      return "STREAMING_MICROPHONE";
    case State::STOP_MICROPHONE:
      return "STOP_MICROPHONE";
    case State::STOPPING_MICROPHONE:
      return "STOPPING_MICROPHONE";
    case State::AWAITING_RESPONSE:
      return "AWAITING_RESPONSE";
    case State::STREAMING_RESPONSE:
      return "STREAMING_RESPONSE";
    default:
      return "UNKNOWN";
  }
}

void OpenAIRealtime::set_state_(State state) {
  State old_state = this->state_;
  this->state_ = state;
  ESP_LOGD(TAG, "State changed from %s to %s", openai_realtime_state_to_string(old_state),
           openai_realtime_state_to_string(state));
}

void OpenAIRealtime::request_start(bool silence_detection) {
  ESP_LOGD(TAG, "request_start called (silence_detection=%d)", silence_detection);
  if (this->state_ != State::IDLE) {
    ESP_LOGW(TAG, "Cannot start while not idle");
    return;
  }

  this->silence_detection_ = silence_detection;
  this->request_text_.clear();
  this->response_text_.clear();
  this->speech_started_ = false;
  this->tts_streaming_ = false;
  this->tool_start_fired_ = false;
  this->in_flight_tool_calls_.clear();

  if (!this->wake_word_.empty()) {
    this->on_wake_word_detected_cb_.call();
  }

  this->set_state_(State::CONNECTING);
  this->connect_();
}

void OpenAIRealtime::request_stop() {
  ESP_LOGD(TAG, "request_stop called");
  switch (this->state_) {
    case State::IDLE:
      break;
    case State::CONNECTING:
      this->disconnect_();
      this->set_state_(State::IDLE);
      break;
    case State::START_MICROPHONE:
    case State::STARTING_MICROPHONE:
    case State::STREAMING_MICROPHONE:
      this->set_state_(State::STOP_MICROPHONE);
      break;
    case State::STOP_MICROPHONE:
    case State::STOPPING_MICROPHONE:
    case State::AWAITING_RESPONSE:
    case State::STREAMING_RESPONSE:
    case State::DRAINING_AUDIO:
      this->audio_buffer_.request_exit();
      this->stop_audio_pipeline_();
      this->finish_response_now_();
      break;
  }
}

void OpenAIRealtime::finish_response_() {
  if (this->state_ == State::IDLE) {
    return;
  }

  // Tell the audio producer that no more deltas are coming. The producer
  // task will finish decoding remaining deltas, write to the ring buffer,
  // and the feeder will drain before signaling stream_done.
  this->audio_buffer_.set_producer_done();

  // If there's still audio in the decode queue or the feeder is still
  // playing, don't tear down yet — transition to DRAINING_AUDIO and let
  // loop() finish the response once the feeder signals stream_done.
  if (this->audio_producer_task_.is_created() || this->audio_buffer_.is_feeder_active()) {
    ESP_LOGD(TAG, "Deferring finish_response_ — audio pipeline still active");
    this->cancel_response_timeout_();
    this->pending_finish_ = true;
    this->set_state_(State::DRAINING_AUDIO);
    return;
  }

  this->finish_response_now_();
}

void OpenAIRealtime::finish_response_now_() {
  this->stop_audio_pipeline_();

  this->publish_response_text_(this->response_text_, true);
  this->on_end_cb_.call();

  this->disconnect_();
  this->deallocate_buffers_();

  // Defer on_idle_cb_ until the speaker has fully stopped. The speaker's
  // finish() is async (signals the speaker task to drain+stop), and the i2s
  // bus is shared between speaker and mic. If we fire on_idle_cb_ now, MWW
  // restarts and starts the mic on the same i2s bus the speaker is still
  // releasing → "Parent bus is busy" → speaker error cascade.
#ifdef USE_SPEAKER
  if (this->speaker_ != nullptr && !this->speaker_->is_stopped()) {
    this->pending_idle_ = true;
  } else {
    this->on_idle_cb_.call();
  }
#else
  this->on_idle_cb_.call();
#endif
  this->pending_finish_ = false;
  this->set_state_(State::IDLE);
}

void OpenAIRealtime::send_session_update_() {
  // Build the session.update JSON manually as a string instead of using
  // json::build_json, because the tools array can be 15KB+ and ArduinoJson's
  // SerializationBuffer has a 5120-byte cap that truncates the output.
  // The tools_json_ string is already valid JSON, so we inject it directly.

  auto json_escape = [](std::string &out, const std::string &in) {
    out += '"';
    for (char c : in) {
      switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
          if ((unsigned char) c < 0x20) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", (unsigned int) (unsigned char) c);
            out += buf;
          } else {
            out += c;
          }
          break;
      }
    }
    out += '"';
  };

  std::string msg;
  msg.reserve(512 + this->system_prompt_.size() + this->tools_json_.size());
  msg += "{\"type\":\"session.update\",\"session\":{";
  msg += "\"instructions\":";
  json_escape(msg, this->system_prompt_);
  if (!this->voice_.empty()) {
    msg += ",\"voice\":";
    json_escape(msg, this->voice_);
  }
  msg += ",\"modalities\":[\"text\",\"audio\"]";
  msg += ",\"input_audio_format\":\"pcm16\"";
  msg += ",\"output_audio_format\":\"pcm16\"";
  if (!this->language_.empty()) {
    msg += ",\"input_audio_transcription\":{\"language\":";
    json_escape(msg, this->language_);
    msg += "}";
  }
  msg += ",\"turn_detection\":{\"type\":\"server_vad\",\"create_response\":true}";
  if (!this->tools_json_.empty()) {
    msg += ",\"tools\":";
    msg += this->tools_json_;
    msg += ",\"tool_choice\":\"auto\"";
  }
  msg += "}}";

  ESP_LOGD(TAG, "Sending session.update: %u bytes", (unsigned) msg.size());
  ESP_LOGD(TAG, "session.update content: %s", msg.c_str());
  if (!this->tools_json_.empty()) {
    ESP_LOGI(TAG, "Tools JSON (%u bytes): %s", (unsigned) this->tools_json_.size(), this->tools_json_.c_str());
  } else {
    ESP_LOGI(TAG, "No tools configured");
  }
  this->send_text_(msg.c_str(), msg.size());
}

void OpenAIRealtime::send_audio_append_(const uint8_t *data, size_t len) {
  if (this->websocket_ == nullptr || !this->connected_) {
    return;
  }

  char buffer[AUDIO_APPEND_JSON_BUFFER_SIZE];
  size_t prefix_len = snprintf(buffer, sizeof(buffer), "{\"type\":\"input_audio_buffer.append\",\"audio\":\"");
  size_t encoded_len = base64_encode_to(data, len, buffer + prefix_len, sizeof(buffer) - prefix_len - 2);
  if (encoded_len == 0) {
    ESP_LOGW(TAG, "Audio delta too large for send buffer");
    return;
  }
  buffer[prefix_len + encoded_len] = '"';
  buffer[prefix_len + encoded_len + 1] = '}';
  buffer[prefix_len + encoded_len + 2] = '\0';

  if (!this->send_text_(buffer, prefix_len + encoded_len + 2)) {
    ESP_LOGW(TAG, "Failed to send audio append");
  } else {
    this->audio_bytes_sent_ += len;
    this->audio_chunks_sent_++;
  }
}

bool OpenAIRealtime::send_text_(const char *text, size_t len) {
  if (this->websocket_ == nullptr || !this->connected_) {
    return false;
  }

  int ret = esp_websocket_client_send_text(this->websocket_, text, len, pdMS_TO_TICKS(1000));
  if (ret < 0) {
    ESP_LOGW(TAG, "Websocket send failed: %d", ret);
    return false;
  }
  return true;
}

void OpenAIRealtime::queue_json_message_(const uint8_t *data, size_t len) {
  if (len == 0) {
    return;
  }

  std::lock_guard<Mutex> lock(this->pending_json_messages_lock_);
  if (this->pending_json_messages_.size() >= MAX_PENDING_JSON_MESSAGES) {
    ESP_LOGW(TAG, "Pending JSON message queue full; dropping message");
    return;
  }
  // Allocate in PSRAM so large audio delta frames don't exhaust internal RAM.
  ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOC_EXTERNAL);
  uint8_t *buf = allocator.allocate(len);
  if (buf == nullptr) {
    ESP_LOGW(TAG, "Could not allocate %u bytes in PSRAM for pending message; dropping", (unsigned) len);
    return;
  }
  memcpy(buf, data, len);
  this->pending_json_messages_.push_back({buf, len});
  App.wake_loop_threadsafe();
}

void OpenAIRealtime::process_pending_flags_() {
  bool client_connected = false;
  bool client_disconnected = false;
  bool websocket_error = false;
  {
    std::lock_guard<Mutex> lock(this->pending_flags_lock_);
    client_connected = this->pending_client_connected_;
    this->pending_client_connected_ = false;
    client_disconnected = this->pending_client_disconnected_;
    this->pending_client_disconnected_ = false;
    websocket_error = this->pending_websocket_error_;
    this->pending_websocket_error_ = false;
  }

    if (client_connected) {
    this->connected_ = true;
    ESP_LOGD(TAG, "Websocket connected");
    this->on_client_connected_cb_.call();
    this->send_session_update_();
  }

  if (client_disconnected) {
    ESP_LOGD(TAG, "Websocket disconnected");
    if (this->connected_) {
      this->connected_ = false;
      this->on_client_disconnected_cb_.call();
    }
    if (this->state_ != State::IDLE) {
      this->finish_response_();
    }
  }

  if (websocket_error) {
    ESP_LOGE(TAG, "Websocket error");
    this->on_error_cb_.call("websocket-error", "Websocket error");
    if (this->state_ != State::IDLE) {
      this->finish_response_();
    }
  }
}

void OpenAIRealtime::process_pending_json_messages_() {
  // Process at most a few messages per loop iteration to avoid blocking the main
  // loop for too long during bursts of transcript/audio deltas. This gives the
  // heap time to coalesce freed PSRAM blocks between parses, reducing transient
  // allocation failures in the JSON deserializer.
  static constexpr size_t MAX_MESSAGES_PER_LOOP = 4;

  for (size_t i = 0; i < MAX_MESSAGES_PER_LOOP; i++) {
    PendingMessage msg{};
    {
      std::lock_guard<Mutex> lock(this->pending_json_messages_lock_);
      if (this->pending_json_messages_.empty()) {
        break;
      }
      msg = this->pending_json_messages_.front();
      this->pending_json_messages_.erase(this->pending_json_messages_.begin());
    }



    this->handle_json_message_(msg.data, msg.len);
    free(msg.data);
  }

  // If more messages remain, wake the loop again to process them next iteration.
  bool has_more = false;
  {
    std::lock_guard<Mutex> lock(this->pending_json_messages_lock_);
    has_more = !this->pending_json_messages_.empty();
  }
  if (has_more) {
    App.wake_loop_threadsafe();
  }
}

void OpenAIRealtime::handle_json_message_(const uint8_t *data, size_t len) {
  // Fast-path: extract the "type" field with a string search to decide whether
  // we need full JSON parsing. Audio delta messages can be 100KB+ (base64 PCM),
  // and ArduinoJson's deserializer fails to allocate even with PSRAM available.
  // For those, we extract the "delta" field directly without parsing.
  const std::string_view view(reinterpret_cast<const char *>(data), len);

  auto find_string_value = [](std::string_view haystack, const char *key) -> std::string_view {
    std::string_view key_sv(key);
    size_t pos = haystack.find(key_sv);
    if (pos == std::string_view::npos)
      return {};
    pos += key_sv.size();
    // Skip whitespace and colon
    while (pos < haystack.size() && (haystack[pos] == ' ' || haystack[pos] == ':' || haystack[pos] == '\t'))
      pos++;
    if (pos >= haystack.size() || haystack[pos] != '"')
      return {};
    size_t start = pos + 1;
    size_t end = haystack.find('"', start);
    if (end == std::string_view::npos)
      return {};
    return haystack.substr(start, end - start);
  };

  // Detect event type by searching for specific type strings anywhere in the
  // full message. This handles servers that put the "type" field after large
  // data fields (e.g., "delta" before "type" in audio delta frames). These
  // strings are unique enough that false positives are essentially impossible.
  bool is_audio_delta = view.find("\"response.output_audio.delta\"") != std::string_view::npos ||
                         view.find("\"response.audio.delta\"") != std::string_view::npos;
  bool is_response_done = view.find("\"response.done\"") != std::string_view::npos;
  bool is_audio_done = view.find("\"response.output_audio.done\"") != std::string_view::npos ||
                       view.find("\"response.audio.done\"") != std::string_view::npos ||
                       (len > 8192 && view.find("\"output_audio\"") != std::string_view::npos);
  bool is_mcp_event = view.find("\"mcp_list_tools") != std::string_view::npos ||
                      view.find("\"response.mcp_call") != std::string_view::npos;

  // Audio delta fast-path: extract base64 delta without JSON parsing.
  if (is_audio_delta) {
    std::string_view delta_sv = find_string_value(view, "\"delta\"");
    ESP_LOGD(TAG, "Received event: response.output_audio.delta (fast-path, %u bytes b64)",
             (unsigned) delta_sv.size());
    if (!delta_sv.empty()) {
      this->handle_audio_delta_(delta_sv.data(), delta_sv.size());
    }
    if (this->state_ == State::AWAITING_RESPONSE) {
      this->set_state_(State::STREAMING_RESPONSE);
    }
    // handle_audio_delta_() starts the audio pipeline (which fires
    // on_tts_stream_start_cb_) on the first delta.
    this->last_response_event_time_ = millis();
    this->start_response_timeout_();
    return;
  }

  // Fast-path for large response.done frames. These can be 100KB+ because they
  // contain the full output array with embedded audio. We don't need the content
  // — we already processed the streamed deltas. Just finish the response.
  if (is_response_done) {
    ESP_LOGD(TAG, "Received event: response.done (fast-path, %u bytes)", (unsigned) len);
    this->finish_response_();
    return;
  }

  // Fast-path for large audio.done frames. Contains the complete audio buffer
  // which we don't need — we already processed the streamed deltas. Just flush
  // the speaker and reset the timeout.
  if (is_audio_done) {
    ESP_LOGD(TAG, "Received event: response.output_audio.done (fast-path, %u bytes)", (unsigned) len);
    this->audio_buffer_.set_producer_done();
    this->last_response_event_time_ = millis();
    this->start_response_timeout_();
    return;
  }

  // MCP lifecycle events — just log them. These can be large (mcp_list_tools.completed
  // contains all tool definitions) so use the fast-path to avoid JSON parsing.
  if (is_mcp_event) {
    // Extract the actual type string for logging
    std::string_view mcp_type;
    size_t mcp_pos = view.find("\"mcp_list_tools");
    if (mcp_pos == std::string_view::npos) {
      mcp_pos = view.find("\"response.mcp_call");
    }
    if (mcp_pos != std::string_view::npos) {
      mcp_type = find_string_value(view.substr(mcp_pos), "\"type\"");
    }
    ESP_LOGI(TAG, "MCP event: '%.*s' (%u bytes)", (int) mcp_type.size(), mcp_type.data(), (unsigned) len);
    // MCP calls reset the response timeout — the server is still working.
    if (view.find("\"response.mcp_call") != std::string_view::npos) {
      this->last_response_event_time_ = millis();
      this->start_response_timeout_();
    }
    return;
  }

  // Skip JSON parsing for large messages we don't need to parse. The fast-paths
  // above handle audio deltas, response.done, audio.done, and MCP events.
  // Remaining large messages are typically conversation.item.added with embedded
  // input audio — we don't need their content.
  if (len > 8192) {
    // Try to extract the type from the first 512 bytes for logging.
    size_t search_len = std::min(len, (size_t) 512);
    std::string_view prefix(reinterpret_cast<const char *>(data), search_len);
    std::string_view log_type = find_string_value(prefix, "\"type\"");
    ESP_LOGD(TAG, "Skipping large message: %u bytes, type='%.*s'",
             (unsigned) len, (int) log_type.size(), log_type.data());
    return;
  }

  // For all other message types, use full JSON parsing (these are small).
  json::parse_json(data, len, [this](JsonObject root) -> bool {
    const char *type = root["type"] | "";
    ESP_LOGD(TAG, "Received event: %s", type);

    if (strcmp(type, "session.created") == 0 || strcmp(type, "session.updated") == 0) {
      this->session_configured_ = true;
      this->cancel_connection_timeout_();
      ESP_LOGD(TAG, "Session configured");
    } else if (strcmp(type, "input_audio_buffer.speech_started") == 0) {
      this->speech_started_ = true;
      this->cancel_no_speech_timeout_();
      this->on_stt_vad_start_cb_.call();
    } else if (strcmp(type, "input_audio_buffer.speech_stopped") == 0) {
      this->on_stt_vad_end_cb_.call();
      if (this->state_ == State::STREAMING_MICROPHONE) {
        this->set_state_(State::STOP_MICROPHONE);
      }
    } else if (strcmp(type, "input_audio_buffer.committed") == 0) {
      // Audio committed by server; nothing to do in auto mode.
    } else if (strcmp(type, "conversation.item.input_audio_transcription.delta") == 0) {
      const char *delta = root["delta"] | "";
      this->request_text_ += delta;
      this->publish_request_text_(this->request_text_);
    } else if (strcmp(type, "conversation.item.input_audio_transcription.completed") == 0) {
      const char *transcript = root["transcript"] | "";
      this->request_text_ = transcript;
      this->publish_request_text_(this->request_text_);
      this->on_stt_end_cb_.call(this->request_text_);
    } else if (strcmp(type, "conversation.item.input_audio_transcription.failed") == 0) {
      ESP_LOGW(TAG, "Input audio transcription failed");
    } else if (strcmp(type, "response.output_audio_transcript.delta") == 0 ||
               strcmp(type, "response.audio_transcript.delta") == 0) {
      const char *delta = root["delta"] | "";
      this->response_text_ += delta;
      this->publish_response_text_(this->response_text_);
      if (this->state_ == State::AWAITING_RESPONSE) {
        this->set_state_(State::STREAMING_RESPONSE);
      }
      // on_tts_stream_start_cb_ is fired by start_audio_pipeline_() when
      // the first audio delta arrives. Text transcript deltas typically
      // arrive around the same time as audio deltas.
      this->last_response_event_time_ = millis();
      this->start_response_timeout_();
    } else if (strcmp(type, "response.output_audio_transcript.done") == 0 ||
               strcmp(type, "response.audio_transcript.done") == 0) {
      const char *transcript = root["transcript"] | "";
      this->response_text_ = transcript;
      this->publish_response_text_(this->response_text_, true);
      this->on_tts_end_cb_.call(this->response_text_);
      this->last_response_event_time_ = millis();
      this->start_response_timeout_();
    } else if (strcmp(type, "response.output_audio.done") == 0 ||
               strcmp(type, "response.audio.done") == 0) {
      this->audio_buffer_.set_producer_done();
      this->last_response_event_time_ = millis();
      this->start_response_timeout_();
    } else if (strcmp(type, "response.output_item.done") == 0) {
      this->last_response_event_time_ = millis();
      this->start_response_timeout_();
    } else if (strcmp(type, "response.done") == 0) {
      this->log_response_status_(root);
      this->finish_response_();
    } else if (strcmp(type, "error") == 0) {
      const char *error_type = root["error"]["type"] | "error";
      const char *code = root["error"]["code"] | "";
      const char *message = root["error"]["message"] | "";
      ESP_LOGE(TAG, "Error: %s - %s (%s)", error_type, message, code);
      this->on_error_cb_.call(code, message);
      if (this->state_ != State::IDLE) {
        this->finish_response_();
      }
    } else if (strncmp(type, "response.function_call_arguments.", 33) == 0 ||
                strncmp(type, "response.mcp_call_arguments.", 28) == 0 ||
                strncmp(type, "response.mcp_call.", 18) == 0 ||
                strncmp(type, "mcp_list_tools.", 15) == 0) {
      ESP_LOGI(TAG, "Tool/MCP event: %s", type);
      // Fire on_tool_start on the first function/MCP call event of this turn.
      if (!this->tool_start_fired_) {
        this->tool_start_fired_ = true;
        this->on_tool_start_cb_.call();
      }
      // MCP calls reset the response timeout — the server is still working.
      if (strncmp(type, "response.mcp_call", 17) == 0) {
        this->last_response_event_time_ = millis();
        this->start_response_timeout_();
      }
    } else {
      ESP_LOGV(TAG, "Unhandled event: %s", type);
    }

    return true;
  });
}

void OpenAIRealtime::handle_audio_delta_(const char *delta, size_t len) {
  if (this->speaker_ == nullptr || len == 0) {
    return;
  }

  // Start the audio pipeline on the first delta of a response.
  if (!this->tts_streaming_) {
    this->start_audio_pipeline_();
  }

  // Push the delta onto the PSRAM-backed queue. The audio producer task
  // will pop and decode it. The queue is capped to prevent unbounded growth.
  if (this->audio_delta_mutex_ != nullptr) {
    xSemaphoreTake(this->audio_delta_mutex_, portMAX_DELAY);
    if (this->audio_delta_queue_.size() >= MAX_AUDIO_DELTA_QUEUE) {
      ESP_LOGW(TAG, "Audio delta queue full (%u); dropping oldest delta", (unsigned) MAX_AUDIO_DELTA_QUEUE);
      free(this->audio_delta_queue_.front().data);
      this->audio_delta_queue_.erase(this->audio_delta_queue_.begin());
    }
    ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOC_EXTERNAL);
    uint8_t *buf = allocator.allocate(len);
    if (buf != nullptr) {
      memcpy(buf, delta, len);
      this->audio_delta_queue_.push_back({buf, len});
    } else {
      ESP_LOGW(TAG, "Could not allocate %u bytes in PSRAM for audio delta; dropping", (unsigned) len);
    }
    xSemaphoreGive(this->audio_delta_mutex_);
  }

  this->response_audio_chunks_received_++;
  this->last_response_event_time_ = millis();
  this->start_response_timeout_();
}

void OpenAIRealtime::start_audio_pipeline_() {
  this->audio_buffer_.reset();
  // Realtime API outputs 24kHz mono 16-bit PCM
  this->audio_buffer_.start_feeder(this->speaker_, 24000, 16, 1);
  this->audio_producer_should_exit_ = false;
  if (!this->audio_producer_task_.create(OpenAIRealtime::audio_producer_task_fn_,
                                          "oai_aud_prod", AUDIO_PRODUCER_STACK_SIZE,
                                          this, AUDIO_TASK_PRIORITY, false)) {
    ESP_LOGE(TAG, "Failed to create audio producer task");
  }
  this->tts_streaming_ = true;
  this->on_tts_stream_start_cb_.call();
}

void OpenAIRealtime::stop_audio_pipeline_() {
  this->audio_producer_should_exit_ = true;
  this->audio_buffer_.request_exit();
  this->audio_buffer_.set_producer_done();
  if (this->audio_producer_task_.is_created()) {
    for (int i = 0; i < 50; i++) {
      if (eTaskGetState(this->audio_producer_task_.get_handle()) == eSuspended) {
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    this->audio_producer_task_.destroy();
  }
  this->audio_buffer_.stop_feeder();
  if (this->tts_streaming_) {
    this->tts_streaming_ = false;
    this->on_tts_stream_end_cb_.call();
  }
}

void OpenAIRealtime::audio_producer_task_fn_(void *arg) {
  OpenAIRealtime *self = (OpenAIRealtime *) arg;
  // Decode buffer: 3 bytes PCM per 4 base64 chars. Use 4KB chunks.
  uint8_t decode_buf[4096];

  while (!self->audio_producer_should_exit_) {
    // Pop a delta from the queue (mutex-protected)
    AudioDelta delta;
    bool got_delta = false;
    if (self->audio_delta_mutex_ != nullptr) {
      xSemaphoreTake(self->audio_delta_mutex_, portMAX_DELAY);
      if (!self->audio_delta_queue_.empty()) {
        delta = self->audio_delta_queue_.front();
        self->audio_delta_queue_.erase(self->audio_delta_queue_.begin());
        got_delta = true;
      }
      xSemaphoreGive(self->audio_delta_mutex_);
    }

    if (!got_delta) {
      // No deltas available. If response is done, signal producer done.
      // Otherwise wait briefly and retry.
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    // Decode the base64 delta in chunks and write PCM to the ring buffer.
    size_t offset = 0;
    while (offset < delta.len && !self->audio_producer_should_exit_) {
      size_t remaining = delta.len - offset;
      size_t b64_chunk = std::min(remaining, (size_t) 4096);
      b64_chunk -= (b64_chunk % 4);  // keep base64 alignment

      if (b64_chunk == 0) {
        break;
      }

      size_t decoded = base64_decode(
          delta.data + offset, b64_chunk, decode_buf, sizeof(decode_buf));

      if (decoded > 0) {
        // Apply volume multiplier to decoded int16 samples
        if (self->volume_multiplier_ != 1.0f) {
          size_t num_samples = decoded / sizeof(int16_t);
          int16_t *samples = reinterpret_cast<int16_t *>(decode_buf);
          for (size_t i = 0; i < num_samples; i++) {
            float scaled = (float) samples[i] * self->volume_multiplier_;
            if (scaled > 32767.0f) scaled = 32767.0f;
            else if (scaled < -32768.0f) scaled = -32768.0f;
            samples[i] = (int16_t) scaled;
          }
        }
        self->audio_buffer_.write(decode_buf, decoded);
      }

      offset += b64_chunk;
    }

    // Free the delta's PSRAM allocation
    free(delta.data);
    self->response_audio_bytes_received_ += delta.len;
  }

  // Signal the feeder that no more audio is coming
  self->audio_buffer_.set_producer_done();
  ESP_LOGD(TAG, "Audio producer task finished");
  vTaskSuspend(nullptr);
}

void OpenAIRealtime::log_response_status_(JsonObject root) {
  const char *status = root["status"] | "";
  JsonObject status_details = root["status_details"];
  JsonArray output_modalities = root["output_modalities"];
  const char *detail_type = status_details["type"] | "";
  const char *detail_reason = status_details["reason"] | "";
  ESP_LOGD(TAG,
           "Response done: status=%s detail_type=%s detail_reason=%s output_modalities_count=%u",
           status, detail_type, detail_reason, output_modalities.size());
}

void OpenAIRealtime::publish_request_text_(const std::string &text) {
#ifdef USE_TEXT_SENSOR
  if (this->text_request_sensor_ != nullptr) {
    this->text_request_sensor_->publish_state(text);
  }
#endif
}

void OpenAIRealtime::publish_response_text_(const std::string &text, bool force) {
#ifdef USE_TEXT_SENSOR
  if (this->text_response_sensor_ != nullptr) {
    uint32_t now = millis();
    if (force || now - this->last_response_publish_time_ >= 250) {
      this->text_response_sensor_->publish_state(text);
      this->last_response_publish_time_ = now;
    }
  }
#endif
}

void OpenAIRealtime::start_no_speech_timeout_() {
  this->set_timeout("no-speech", NO_SPEECH_TIMEOUT_MS, [this]() {
    ESP_LOGW(TAG, "No speech detected");
    this->on_error_cb_.call("no-speech", "No speech detected");
    this->finish_response_();
  });
}

void OpenAIRealtime::start_response_timeout_() {
  this->set_timeout("response", RESPONSE_TIMEOUT_MS, [this]() {
    ESP_LOGW(TAG, "Response timeout");
    this->on_error_cb_.call("response-timeout", "Response timeout");
    this->finish_response_();
  });
}

void OpenAIRealtime::start_connection_timeout_() {
  this->set_timeout("connection", CONNECTION_TIMEOUT_MS, [this]() {
    ESP_LOGE(TAG, "Connection timeout");
    this->on_error_cb_.call("connection-timeout", "Connection timeout");
    this->disconnect_();
    this->deallocate_buffers_();
    this->on_idle_cb_.call();
    this->set_state_(State::IDLE);
  });
}

void OpenAIRealtime::cancel_no_speech_timeout_() { this->cancel_timeout("no-speech"); }
void OpenAIRealtime::cancel_response_timeout_() { this->cancel_timeout("response"); }
void OpenAIRealtime::cancel_connection_timeout_() { this->cancel_timeout("connection"); }

void OpenAIRealtime::loop() {
  if (this->state_ != State::IDLE) {
    this->process_pending_flags_();
    this->process_pending_json_messages_();
  }

  switch (this->state_) {
    case State::IDLE:
      this->deallocate_buffers_();
      // Fire deferred on_idle_cb_ once the speaker has fully released the i2s bus.
#ifdef USE_SPEAKER
      if (this->pending_idle_ && (this->speaker_ == nullptr || this->speaker_->is_stopped())) {
        this->pending_idle_ = false;
        this->on_idle_cb_.call();
      }
#else
      if (this->pending_idle_) {
        this->pending_idle_ = false;
        this->on_idle_cb_.call();
      }
#endif
      break;

    case State::CONNECTING: {
      if (this->connected_ && this->session_configured_) {
        this->on_start_cb_.call();
        this->set_state_(State::START_MICROPHONE);
      }
      break;
    }

    case State::START_MICROPHONE: {
      if (!this->allocate_buffers_()) {
        this->status_set_error();
        this->on_error_cb_.call("alloc-failed", "Failed to allocate buffers");
        this->finish_response_();
        break;
      }
      this->status_clear_error();
      this->clear_buffers_();
      this->mic_source_->start();
      this->set_state_(State::STARTING_MICROPHONE);
      break;
    }

    case State::STARTING_MICROPHONE: {
      if (this->mic_source_->is_running()) {
        this->set_state_(State::STREAMING_MICROPHONE);
        this->on_listening_cb_.call();
        this->start_no_speech_timeout_();
      }
      break;
    }

    case State::STREAMING_MICROPHONE: {
      uint8_t send_buffer[SEND_BUFFER_SIZE];
      for (size_t i = 0; i < MAX_CHUNKS_PER_LOOP; i++) {
        size_t read = this->ring_buffer_->read(send_buffer, SEND_BUFFER_SIZE);
        if (read == 0) {
          break;
        }
        this->send_audio_append_(send_buffer, read);
      }

      uint32_t now = millis();
      if (now - this->last_audio_stats_time_ >= AUDIO_STATS_LOG_INTERVAL_MS) {
        ESP_LOGD(TAG, "Audio stream stats: mic_rx=%" PRIu32 " sent=%" PRIu32 " chunks=%" PRIu32,
                 this->mic_bytes_received_, this->audio_bytes_sent_, this->audio_chunks_sent_);
        this->last_audio_stats_time_ = now;
      }
      break;
    }

    case State::STOP_MICROPHONE: {
      if (this->mic_source_->is_running()) {
        this->mic_source_->stop();
      }
      this->set_state_(State::STOPPING_MICROPHONE);
      break;
    }

    case State::STOPPING_MICROPHONE: {
      if (this->mic_source_->is_stopped()) {
        this->set_state_(State::AWAITING_RESPONSE);
        this->last_response_event_time_ = millis();
        this->start_response_timeout_();
      }
      break;
    }

    case State::AWAITING_RESPONSE:
      // Waiting for response events; timeouts handled by start_response_timeout_.
      break;

    case State::STREAMING_RESPONSE:
      // Don't check stream_done here — response.done (which calls
      // finish_response_()) is the authoritative end-of-turn signal.
      // finish_response_() transitions to DRAINING_AUDIO when the audio
      // pipeline is still active. Checking stream_done here would clear
      // the flag, making the DRAINING_AUDIO check dead and causing a deadlock.
      break;

    case State::DRAINING_AUDIO:
      // Poll the audio buffer for stream completion. The feeder task drains
      // the ring buffer, finishes the speaker, and sets stream_done.
      if (this->audio_buffer_.is_stream_done()) {
        this->audio_buffer_.clear_stream_done();
        this->stop_audio_pipeline_();
        this->finish_response_now_();
      }
      break;
  }
}

void OpenAIRealtime::handle_websocket_event_(esp_websocket_event_id_t event_id,
                                               esp_websocket_event_data_t *event_data) {
  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      ESP_LOGD(TAG, "Websocket connected");
      {
        std::lock_guard<Mutex> lock(this->pending_flags_lock_);
        this->pending_client_connected_ = true;
      }
      App.wake_loop_threadsafe();
      break;

    case WEBSOCKET_EVENT_DISCONNECTED:
      ESP_LOGD(TAG, "Websocket disconnected");
      {
        std::lock_guard<Mutex> lock(this->pending_flags_lock_);
        this->pending_client_disconnected_ = true;
      }
      App.wake_loop_threadsafe();
      break;

    case WEBSOCKET_EVENT_DATA: {
      // Extract the "type" field from the first fragment of each frame.
      // Used for early-skipping frames we don't need to buffer fully.
      std::string frame_type;
      if (event_data->payload_offset == 0) {
        const std::string_view view(event_data->data_ptr, event_data->data_len);
        size_t type_pos = view.find("\"type\"");
        if (type_pos != std::string_view::npos) {
          size_t colon = view.find(':', type_pos + 6);
          if (colon != std::string_view::npos) {
            size_t q1 = view.find('"', colon + 1);
            size_t q2 = (q1 != std::string_view::npos) ? view.find('"', q1 + 1) : std::string_view::npos;
            if (q1 != std::string_view::npos && q2 != std::string_view::npos) {
              frame_type = std::string(view.substr(q1 + 1, q2 - q1 - 1));
            }
          }
        }
      }

      // Early-skip response.audio.done / response.output_audio.done frames.
      // These contain the complete audio buffer (can be 400KB+) which we don't
      // need — we already processed the streamed deltas. Queue a minimal
      // synthetic event so the handler can flush the speaker, then skip the
      // rest of the frame without buffering it. This check must come BEFORE
      // the oversized-frame drop, otherwise large .done frames get dropped
      // without queueing the synthetic event.
      //
      // Some servers use flat type names (e.g. "output_audio" instead of
      // "response.output_audio.done"). We early-skip those too, but only when
      // oversized — small frames with flat names might be deltas we need.
      if (event_data->payload_offset == 0 &&
          (frame_type == "response.audio.done" || frame_type == "response.output_audio.done" ||
           (event_data->payload_len > MAX_JSON_FRAME_SIZE &&
            (frame_type == "output_audio" || frame_type == "audio")))) {
        static const char synthetic_done[] = "{\"type\":\"response.output_audio.done\"}";
        this->queue_json_message_(reinterpret_cast<const uint8_t *>(synthetic_done), sizeof(synthetic_done) - 1);
        this->rx_drop_oversized_payload_ = true;
        break;
      }

      // Early-skip oversized response.done frames. These can contain the full
      // output array with embedded audio. Queue a synthetic event so the handler
      // finishes the response, then skip the frame.
      if (event_data->payload_offset == 0 && frame_type == "response.done") {
        static const char synthetic_response_done[] = "{\"type\":\"response.done\"}";
        this->queue_json_message_(reinterpret_cast<const uint8_t *>(synthetic_response_done),
                                  sizeof(synthetic_response_done) - 1);
        this->rx_drop_oversized_payload_ = true;
        break;
      }

      // Drop oversized frames (e.g. unary .done events containing full audio).
      if (event_data->payload_len > MAX_JSON_FRAME_SIZE) {
        if (event_data->payload_offset == 0) {
          ESP_LOGW(TAG, "Oversized JSON frame (%" PRIu32 " bytes); dropping type='%s'",
                   event_data->payload_len, frame_type.c_str());
        }
        this->rx_drop_oversized_payload_ = true;
        this->rx_message_len_ = 0;
        break;
      }

      if (this->rx_drop_oversized_payload_) {
        if (event_data->payload_offset + event_data->data_len >= event_data->payload_len) {
          this->rx_drop_oversized_payload_ = false;
        }
        break;
      }
      if (event_data->payload_offset == 0) {
        this->rx_message_len_ = 0;
      }
      // Append fragment to the PSRAM-backed rx buffer
      size_t copy_len = std::min((size_t) event_data->data_len,
                                  MAX_JSON_FRAME_SIZE - this->rx_message_len_);
      if (copy_len > 0 && this->rx_message_buffer_ != nullptr) {
        memcpy(this->rx_message_buffer_ + this->rx_message_len_,
               event_data->data_ptr, copy_len);
        this->rx_message_len_ += copy_len;
      }
      if (event_data->payload_offset + event_data->data_len >= event_data->payload_len) {
        this->queue_json_message_(this->rx_message_buffer_, this->rx_message_len_);
        this->rx_message_len_ = 0;
      }
      break;
    }

    case WEBSOCKET_EVENT_ERROR:
      ESP_LOGE(TAG, "Websocket event error");
      {
        std::lock_guard<Mutex> lock(this->pending_flags_lock_);
        this->pending_websocket_error_ = true;
      }
      App.wake_loop_threadsafe();
      break;

    default:
      break;
  }
}

void OpenAIRealtime::websocket_event_handler_(void *handler_args, esp_event_base_t base, int32_t event_id,
                                               void *event_data) {
  auto *this_ = static_cast<OpenAIRealtime *>(handler_args);
  this_->handle_websocket_event_(static_cast<esp_websocket_event_id_t>(event_id),
                                 static_cast<esp_websocket_event_data_t *>(event_data));
}

}  // namespace esphome::openai_realtime

#endif  // USE_OPENAI_REALTIME
