#include "openai_assistant.h"
#include "esphome/core/defines.h"

#ifdef USE_OPENAI_ASSISTANT

#include "esphome/components/json/json_util.h"
#include "esphome/core/alloc_helpers.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <ArduinoJson.h>
#include <esp_err.h>
#include <algorithm>
#include <cstring>
#include <cinttypes>
#include <string_view>

namespace esphome::openai_assistant {

static const char *const TAG = "openai_assistant";
static const size_t SAMPLE_RATE_HZ = 16000;
static const size_t OUTPUT_SAMPLE_RATE_HZ = 24000;
static const size_t RING_BUFFER_SAMPLES = 512 * SAMPLE_RATE_HZ / 1000;
static const size_t RING_BUFFER_SIZE = RING_BUFFER_SAMPLES * sizeof(int16_t);
static const size_t SEND_BUFFER_SAMPLES = 32 * SAMPLE_RATE_HZ / 1000;
static const size_t SEND_BUFFER_SIZE = SEND_BUFFER_SAMPLES * sizeof(int16_t);
static const size_t SPEAKER_BUFFER_SIZE = 4096;
static const uint32_t CONNECT_TIMEOUT_MS = 15000;
static const uint32_t RESPONSE_INACTIVITY_TIMEOUT_MS = 15000;
static const uint32_t RESPONSE_TEXT_PUBLISH_INTERVAL_MS = 250;
static const uint32_t NO_SPEECH_TIMEOUT_MS = 5000;
static const uint32_t RESPONSE_CREATE_AFTER_COMMIT_TIMEOUT_MS = 3000;
static const size_t MAX_JSON_MESSAGE_SIZE = 8192;

static std::string extract_json_type_(const char *data, size_t len) {
  const std::string_view view(data, len);
  size_t type_pos = view.find("\"type\"");
  if (type_pos == std::string_view::npos) {
    return "";
  }
  size_t colon_pos = view.find(':', type_pos + 6);
  if (colon_pos == std::string_view::npos) {
    return "";
  }
  size_t quote_start = view.find('"', colon_pos + 1);
  if (quote_start == std::string_view::npos) {
    return "";
  }
  size_t quote_end = view.find('"', quote_start + 1);
  if (quote_end == std::string_view::npos) {
    return "";
  }
  return std::string(view.substr(quote_start + 1, quote_end - quote_start - 1));
}

static const char *state_to_string(State state) {
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
  }
  return "UNKNOWN";
}

OpenAIAssistant::OpenAIAssistant() = default;

OpenAIAssistant::~OpenAIAssistant() { this->disconnect_(); }

void OpenAIAssistant::setup() {
  this->mic_source_->add_data_callback([this](const std::vector<uint8_t> &data) {
    if (this->ring_buffer_ != nullptr) {
      this->mic_bytes_received_ += data.size();
      this->ring_buffer_->write(data.data(), data.size());
    }
  });

#ifdef USE_SPEAKER
  if (this->speaker_ != nullptr) {
    this->speaker_->set_audio_stream_info(audio::AudioStreamInfo(16, 1, OUTPUT_SAMPLE_RATE_HZ));
  }
#endif
}

float OpenAIAssistant::get_setup_priority() const { return setup_priority::AFTER_CONNECTION; }

void OpenAIAssistant::dump_config() {
  ESP_LOGCONFIG(TAG, "OpenAI Assistant:");
  ESP_LOGCONFIG(TAG, "  Endpoint: %s", this->endpoint_.c_str());
  ESP_LOGCONFIG(TAG, "  Model: %s", this->model_.c_str());
  ESP_LOGCONFIG(TAG, "  Voice: %s", this->voice_.c_str());
  ESP_LOGCONFIG(TAG, "  Wake Word: %s", YESNO(this->use_wake_word_));
}

bool OpenAIAssistant::allocate_buffers_() {
  if (this->ring_buffer_ == nullptr) {
    this->ring_buffer_ = ring_buffer::RingBuffer::create(RING_BUFFER_SIZE);
    if (this->ring_buffer_ == nullptr) {
      ESP_LOGE(TAG, "Could not allocate microphone ring buffer");
      return false;
    }
  }

#ifdef USE_SPEAKER
  if ((this->speaker_ != nullptr) && (this->speaker_buffer_ == nullptr)) {
    RAMAllocator<uint8_t> allocator;
    this->speaker_buffer_ = allocator.allocate(SPEAKER_BUFFER_SIZE);
    if (this->speaker_buffer_ == nullptr) {
      ESP_LOGE(TAG, "Could not allocate speaker buffer");
      return false;
    }
  }
#endif
  return true;
}

void OpenAIAssistant::clear_buffers_() {
  if (this->ring_buffer_ != nullptr) {
    this->ring_buffer_->reset();
  }
#ifdef USE_SPEAKER
  this->speaker_buffer_size_ = 0;
#endif
}

void OpenAIAssistant::deallocate_buffers_() {
  this->ring_buffer_.reset();
  this->mic_bytes_received_ = 0;
  this->audio_bytes_sent_ = 0;
  this->audio_chunks_sent_ = 0;
  this->response_audio_bytes_received_ = 0;
  this->response_audio_chunks_received_ = 0;
  this->websocket_send_failures_ = 0;
  this->last_audio_log_time_ = 0;
#ifdef USE_SPEAKER
  if (this->speaker_buffer_ != nullptr) {
    RAMAllocator<uint8_t> allocator;
    allocator.deallocate(this->speaker_buffer_, SPEAKER_BUFFER_SIZE);
    this->speaker_buffer_ = nullptr;
    this->speaker_buffer_size_ = 0;
  }
#endif
}

bool OpenAIAssistant::connect_() {
  if (this->websocket_ != nullptr) {
    ESP_LOGD(TAG, "Realtime websocket client already exists");
    return true;
  }

  this->endpoint_with_model_ = this->endpoint_;
  this->endpoint_with_model_ += (this->endpoint_.find('?') == std::string::npos) ? "?model=" : "&model=";
  this->endpoint_with_model_ += this->model_;

  this->headers_.clear();
  if (!this->api_key_.empty()) {
    this->headers_ += "Authorization: Bearer ";
    this->headers_ += this->api_key_;
    this->headers_ += "\r\n";
  }
  this->headers_ += "OpenAI-Beta: realtime=v1\r\n";

  ESP_LOGI(TAG, "Opening realtime websocket: %s", this->endpoint_with_model_.c_str());
  ESP_LOGD(TAG, "Realtime websocket auth header present: %s", YESNO(!this->api_key_.empty()));

  esp_websocket_client_config_t config = {};
  config.uri = this->endpoint_with_model_.c_str();
  config.headers = this->headers_.c_str();
  config.network_timeout_ms = 10000;
  config.reconnect_timeout_ms = 10000;

  this->websocket_ = esp_websocket_client_init(&config);
  if (this->websocket_ == nullptr) {
    ESP_LOGE(TAG, "Could not initialize websocket client");
    return false;
  }
  esp_websocket_register_events(this->websocket_, WEBSOCKET_EVENT_ANY, OpenAIAssistant::websocket_event_handler_, this);
  esp_err_t err = esp_websocket_client_start(this->websocket_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Could not start websocket client: %s", esp_err_to_name(err));
    this->disconnect_();
    return false;
  }
  ESP_LOGI(TAG, "Realtime websocket client start requested");
  return true;
}

void OpenAIAssistant::disconnect_() {
  if (this->websocket_ == nullptr) {
    return;
  }
  if (esp_websocket_client_is_connected(this->websocket_)) {
    esp_websocket_client_stop(this->websocket_);
  }
  esp_websocket_client_destroy(this->websocket_);
  this->websocket_ = nullptr;
  this->connected_ = false;
  this->session_configured_ = false;
}

void OpenAIAssistant::set_state_(State state) {
  if (this->state_ == state) {
    return;
  }
  ESP_LOGD(TAG, "State changed from %s to %s", state_to_string(this->state_), state_to_string(state));
  this->state_ = state;
}

void OpenAIAssistant::request_start(bool continuous, bool silence_detection) {
  ESP_LOGD(TAG, "Requesting start: continuous=%s, silence_detection=%s, wake_word='%s'", YESNO(continuous),
           YESNO(silence_detection), this->wake_word_.c_str());
  if (this->state_ != State::IDLE) {
    ESP_LOGW(TAG, "Ignoring start request because assistant is not idle");
    return;
  }
  this->continuous_ = continuous;
  this->silence_detection_ = silence_detection;
  this->response_requested_ = false;
  this->input_committed_ = false;
  this->transcription_completed_ = false;
  this->input_committed_time_ = 0;
  if (!this->allocate_buffers_()) {
    this->error_trigger_.trigger("buffer-allocation-failed", "Could not allocate audio buffers");
    return;
  }
  this->clear_buffers_();
  if (!this->wake_word_.empty()) {
    this->wake_word_detected_trigger_.trigger();
  }
  this->set_state_(State::CONNECTING);
  this->connection_start_time_ = millis();
  this->last_response_event_time_ = 0;
  this->last_response_publish_time_ = 0;
  this->listening_start_time_ = 0;
  this->speech_started_ = false;
  this->no_speech_timeout_ = false;
  if (!this->connect_()) {
    this->error_trigger_.trigger("connection-failed", "Could not connect to realtime endpoint");
    this->set_state_(State::IDLE);
    this->continuous_ = false;
    this->idle_trigger_.trigger();
  }
}

void OpenAIAssistant::request_stop() {
  ESP_LOGD(TAG, "Requesting stop");
  this->continuous_ = false;
  switch (this->state_) {
    case State::IDLE:
      break;
    case State::START_MICROPHONE:
    case State::STARTING_MICROPHONE:
    case State::STREAMING_MICROPHONE:
      this->signal_stop_();
      this->set_state_(State::STOP_MICROPHONE);
      break;
    case State::AWAITING_RESPONSE:
    case State::STREAMING_RESPONSE:
      this->signal_stop_();
      break;
    default:
      this->set_state_(State::STOP_MICROPHONE);
      break;
  }
}

void OpenAIAssistant::loop() {
  this->process_pending_websocket_events_();

  switch (this->state_) {
    case State::IDLE:
      if (!this->continuous_) {
        this->deallocate_buffers_();
      }
      break;
    case State::CONNECTING:
      if (this->connected_ && this->session_configured_) {
        this->set_state_(State::START_MICROPHONE);
      } else if (millis() - this->connection_start_time_ > CONNECT_TIMEOUT_MS) {
        ESP_LOGW(TAG, "Realtime websocket did not connect within %ums", CONNECT_TIMEOUT_MS);
        this->error_trigger_.trigger("connection-timeout", "Realtime websocket connection timed out");
        this->disconnect_();
        this->continuous_ = false;
        this->set_state_(State::IDLE);
        this->idle_trigger_.trigger();
      }
      break;
    case State::START_MICROPHONE:
      ESP_LOGD(TAG, "Starting microphone");
      this->mic_source_->start();
      this->set_state_(State::STARTING_MICROPHONE);
      break;
    case State::STARTING_MICROPHONE:
      if (this->mic_source_->is_running()) {
        this->start_trigger_.trigger();
        this->listening_trigger_.trigger();
        this->listening_start_time_ = millis();
        this->speech_started_ = false;
        this->set_state_(State::STREAMING_MICROPHONE);
      }
      break;
    case State::STREAMING_MICROPHONE: {
      if (this->ring_buffer_ == nullptr || !this->connected_) {
        break;
      }
      uint8_t buffer[SEND_BUFFER_SIZE];
      uint8_t chunks_sent_this_loop = 0;
      while (this->ring_buffer_->available() >= SEND_BUFFER_SIZE && chunks_sent_this_loop < 4) {
        size_t read = this->ring_buffer_->read(buffer, SEND_BUFFER_SIZE);
        if (read == 0) {
          break;
        }
        this->send_audio_append_(buffer, read);
        chunks_sent_this_loop++;
      }
      const uint32_t now = millis();
      if (now - this->last_audio_log_time_ > 5000) {
        this->last_audio_log_time_ = now;
        ESP_LOGD(TAG,
                 "Audio stream stats: mic_rx=%" PRIu32 " bytes, sent=%" PRIu32 " bytes in %" PRIu32
                 " chunks, send_failures=%" PRIu32 ", ring_available=%u",
                 this->mic_bytes_received_, this->audio_bytes_sent_, this->audio_chunks_sent_,
                 this->websocket_send_failures_, static_cast<unsigned>(this->ring_buffer_->available()));
      }
      if (this->silence_detection_ && !this->speech_started_ && this->listening_start_time_ != 0 &&
          now - this->listening_start_time_ > NO_SPEECH_TIMEOUT_MS) {
        ESP_LOGW(TAG, "No speech detected within %ums; returning to idle", NO_SPEECH_TIMEOUT_MS);
        this->no_speech_timeout_ = true;
        this->continuous_ = false;
        this->set_state_(State::STOP_MICROPHONE);
      }
      break;
    }
    case State::STOP_MICROPHONE:
      if (this->mic_source_->is_running()) {
        this->mic_source_->stop();
        this->set_state_(State::STOPPING_MICROPHONE);
      } else if (this->no_speech_timeout_) {
        this->no_speech_timeout_ = false;
        this->set_state_(State::IDLE);
        this->idle_trigger_.trigger();
        this->disconnect_();
      } else {
        this->set_state_(State::AWAITING_RESPONSE);
      }
      break;
    case State::STOPPING_MICROPHONE:
      if (this->mic_source_->is_stopped()) {
        if (this->no_speech_timeout_) {
          this->no_speech_timeout_ = false;
          this->set_state_(State::IDLE);
          this->idle_trigger_.trigger();
          this->disconnect_();
        } else {
          this->set_state_(State::AWAITING_RESPONSE);
        }
      }
      break;
    case State::AWAITING_RESPONSE:
      if (!this->connected_) {
        this->set_state_(State::IDLE);
      } else if (this->input_committed_ && !this->response_requested_ && this->input_committed_time_ != 0 &&
                 millis() - this->input_committed_time_ > RESPONSE_CREATE_AFTER_COMMIT_TIMEOUT_MS) {
        ESP_LOGW(TAG, "Input transcription did not complete within %ums; requesting response anyway",
                 RESPONSE_CREATE_AFTER_COMMIT_TIMEOUT_MS);
        this->send_response_create_();
      } else if (this->last_response_event_time_ != 0 &&
                 millis() - this->last_response_event_time_ > RESPONSE_INACTIVITY_TIMEOUT_MS) {
        ESP_LOGW(TAG, "Realtime response inactive for %ums; finishing response", RESPONSE_INACTIVITY_TIMEOUT_MS);
        this->finish_response_();
      }
      break;
    case State::STREAMING_RESPONSE:
#ifdef USE_SPEAKER
      if (this->state_ == State::STREAMING_RESPONSE && this->speaker_ != nullptr && this->speaker_buffer_size_ > 0) {
        size_t written = this->speaker_->play(this->speaker_buffer_, this->speaker_buffer_size_);
        if (written > 0) {
          memmove(this->speaker_buffer_, this->speaker_buffer_ + written, this->speaker_buffer_size_ - written);
          this->speaker_buffer_size_ -= written;
        }
      }
#endif
      if (this->last_response_event_time_ != 0 &&
          millis() - this->last_response_event_time_ > RESPONSE_INACTIVITY_TIMEOUT_MS) {
        ESP_LOGW(TAG, "Realtime response inactive for %ums; finishing response", RESPONSE_INACTIVITY_TIMEOUT_MS);
        this->finish_response_();
      }
      break;
  }
}

void OpenAIAssistant::send_session_update_() {
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  root["type"] = "session.update";
  JsonObject session = root["session"].to<JsonObject>();
  session["instructions"] = this->system_prompt_;
  session["voice"] = this->voice_;
  JsonArray modalities = session["modalities"].to<JsonArray>();
  modalities.add("text");
  modalities.add("audio");
  session["input_audio_format"] = "pcm16";
  session["output_audio_format"] = "pcm16";
  JsonObject turn_detection = session["turn_detection"].to<JsonObject>();
  if (this->silence_detection_) {
    turn_detection["type"] = "server_vad";
    // Let server VAD commit the input buffer, but create the response ourselves so we can explicitly request audio.
    turn_detection["create_response"] = false;
  } else {
    turn_detection["type"] = "none";
  }
  std::string msg;
  serializeJson(doc, msg);
  this->send_text_(msg);
}

void OpenAIAssistant::send_audio_append_(const uint8_t *data, size_t len) {
  std::string encoded = base64_encode(data, len);
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  root["type"] = "input_audio_buffer.append";
  root["audio"] = encoded;
  std::string msg;
  serializeJson(doc, msg);
  if (this->send_text_(msg)) {
    this->audio_bytes_sent_ += len;
    this->audio_chunks_sent_++;
  }
}

bool OpenAIAssistant::send_text_(const std::string &text) {
  if (this->websocket_ == nullptr || !this->connected_) {
    this->websocket_send_failures_++;
    return false;
  }
  const int sent = esp_websocket_client_send_text(this->websocket_, text.c_str(), text.size(), 50);
  if (sent < 0) {
    this->websocket_send_failures_++;
    ESP_LOGW(TAG, "Realtime websocket send failed for %u byte text frame", static_cast<unsigned>(text.size()));
    return false;
  }
  return true;
}

void OpenAIAssistant::send_response_create_() {
  if (this->response_requested_) {
    return;
  }
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  root["type"] = "response.create";
  JsonObject response = root["response"].to<JsonObject>();
  JsonArray output_modalities = response["output_modalities"].to<JsonArray>();
  output_modalities.add("audio");
  JsonObject audio = response["audio"].to<JsonObject>();
  JsonObject output = audio["output"].to<JsonObject>();
  JsonObject format = output["format"].to<JsonObject>();
  format["type"] = "audio/pcm";
  format["rate"] = OUTPUT_SAMPLE_RATE_HZ;
  output["voice"] = this->voice_;

  std::string msg;
  serializeJson(doc, msg);
  ESP_LOGD(TAG, "Requesting realtime response with audio output");
  if (this->send_text_(msg)) {
    this->response_requested_ = true;
  }
}

void OpenAIAssistant::signal_stop_() {
  if (!this->connected_) {
    return;
  }
  ESP_LOGD(TAG, "Committing input audio buffer and requesting response");
  this->send_text_("{\"type\":\"input_audio_buffer.commit\"}");
  this->send_response_create_();
}

void OpenAIAssistant::finish_response_() {
  if (this->tts_streaming_) {
    this->tts_streaming_ = false;
    this->tts_stream_end_trigger_.trigger();
#ifdef USE_SPEAKER
    if (this->speaker_ != nullptr) {
      this->speaker_->finish();
    }
#endif
  }
  this->response_text_active_ = false;
  this->last_response_event_time_ = 0;
  this->last_response_publish_time_ = 0;
  if (!this->response_text_.empty()) {
    this->publish_response_text_(this->response_text_.c_str());
  }
  ESP_LOGI(TAG, "Response finished: response_audio=%" PRIu32 " bytes in %" PRIu32 " chunks", this->response_audio_bytes_received_,
           this->response_audio_chunks_received_);
  if (this->response_audio_bytes_received_ == 0) {
    ESP_LOGW(TAG, "Realtime response contained transcript text but no audio deltas; check endpoint voice/audio output config");
  }
  this->end_trigger_.trigger();
  if (this->continuous_) {
    this->idle_trigger_.trigger();
    this->set_state_(State::START_MICROPHONE);
  } else {
    this->set_state_(State::IDLE);
    this->disconnect_();
    this->idle_trigger_.trigger();
  }
}

void OpenAIAssistant::queue_json_message_(const uint8_t *data, size_t len) {
  {
    LockGuard lock(this->pending_json_messages_lock_);
    if (this->pending_json_messages_.size() >= 64) {
      ESP_LOGW(TAG, "Dropping realtime JSON message because pending queue is full");
      return;
    }
    this->pending_json_messages_.emplace_back(reinterpret_cast<const char *>(data), len);
  }
  App.wake_loop_threadsafe();
}

void OpenAIAssistant::process_pending_websocket_events_() {
  bool client_connected = false;
  bool client_disconnected = false;
  bool websocket_error = false;
  bool session_update = false;
  {
    LockGuard lock(this->pending_websocket_events_lock_);
    client_connected = this->pending_client_connected_;
    client_disconnected = this->pending_client_disconnected_;
    websocket_error = this->pending_websocket_error_;
    session_update = this->pending_session_update_;
    this->pending_client_connected_ = false;
    this->pending_client_disconnected_ = false;
    this->pending_websocket_error_ = false;
    this->pending_session_update_ = false;
  }

  if (client_connected) {
    this->client_connected_trigger_.trigger();
  }
  if (session_update && this->connected_) {
    this->send_session_update_();
  }
  if (client_disconnected) {
    this->client_disconnected_trigger_.trigger();
    if (this->state_ != State::IDLE) {
      this->set_state_(State::STOP_MICROPHONE);
    }
  }
  if (websocket_error) {
    this->error_trigger_.trigger("websocket-error", "Realtime websocket error");
  }

  std::vector<std::string> messages;
  {
    LockGuard lock(this->pending_json_messages_lock_);
    messages.swap(this->pending_json_messages_);
  }
  for (const auto &message : messages) {
    this->handle_json_message_(reinterpret_cast<const uint8_t *>(message.data()), message.size());
  }
}

void OpenAIAssistant::websocket_event_handler_(void *handler_args, esp_event_base_t base, int32_t event_id,
                                               void *event_data) {
  auto *assistant = static_cast<OpenAIAssistant *>(handler_args);
  assistant->handle_websocket_event_(static_cast<esp_websocket_event_id_t>(event_id),
                                     static_cast<esp_websocket_event_data_t *>(event_data));
}

void OpenAIAssistant::handle_websocket_event_(esp_websocket_event_id_t event_id,
                                              esp_websocket_event_data_t *event_data) {
  ESP_LOGV(TAG, "Realtime websocket event: id=%d op=%d payload_len=%d data_len=%d offset=%d", static_cast<int>(event_id),
           event_data == nullptr ? -1 : event_data->op_code, event_data == nullptr ? -1 : event_data->payload_len,
           event_data == nullptr ? -1 : event_data->data_len, event_data == nullptr ? -1 : event_data->payload_offset);
  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      ESP_LOGD(TAG, "Realtime websocket connected");
      this->connected_ = true;
      {
        LockGuard lock(this->pending_websocket_events_lock_);
        this->pending_client_connected_ = true;
        this->pending_session_update_ = true;
      }
      App.wake_loop_threadsafe();
      break;
    case WEBSOCKET_EVENT_DISCONNECTED:
      ESP_LOGD(TAG, "Realtime websocket disconnected");
      this->connected_ = false;
      this->session_configured_ = false;
      {
        LockGuard lock(this->pending_websocket_events_lock_);
        this->pending_client_disconnected_ = true;
      }
      App.wake_loop_threadsafe();
      break;
    case WEBSOCKET_EVENT_DATA:
      if (event_data->op_code == 0x1) {
        if (event_data->payload_offset == 0 && event_data->payload_len == event_data->data_len) {
          if (event_data->payload_len > MAX_JSON_MESSAGE_SIZE) {
            std::string type = extract_json_type_(event_data->data_ptr, event_data->data_len);
            ESP_LOGW(TAG, "Dropping oversized realtime JSON message: %d bytes, type='%s'", event_data->payload_len,
                     type.c_str());
            if (type == "response.audio.delta" || type == "response.output_audio.delta") {
              ESP_LOGW(TAG, "Dropped oversized realtime audio delta; configure endpoint to send smaller audio chunks");
            }
          } else {
            this->queue_json_message_(reinterpret_cast<const uint8_t *>(event_data->data_ptr), event_data->data_len);
          }
        } else {
          if (event_data->payload_offset == 0) {
            this->rx_message_.clear();
            this->rx_drop_oversized_payload_ = event_data->payload_len > MAX_JSON_MESSAGE_SIZE;
            this->rx_oversized_payload_len_ = this->rx_drop_oversized_payload_ ? event_data->payload_len : 0;
            this->rx_oversized_type_.clear();
            if (!this->rx_drop_oversized_payload_) {
              this->rx_message_.reserve(event_data->payload_len);
            } else {
              this->rx_oversized_type_ = extract_json_type_(event_data->data_ptr, event_data->data_len);
            }
          }
          if (this->rx_drop_oversized_payload_) {
            if (event_data->payload_offset + event_data->data_len >= event_data->payload_len) {
              ESP_LOGW(TAG, "Dropping oversized fragmented realtime JSON message: %u bytes, type='%s'",
                       static_cast<unsigned>(this->rx_oversized_payload_len_), this->rx_oversized_type_.c_str());
              if (this->rx_oversized_type_ == "response.audio.delta" ||
                  this->rx_oversized_type_ == "response.output_audio.delta") {
                ESP_LOGW(TAG, "Dropped oversized realtime audio delta; configure endpoint to send smaller audio chunks");
              }
              this->rx_drop_oversized_payload_ = false;
              this->rx_oversized_payload_len_ = 0;
              this->rx_oversized_type_.clear();
            }
            break;
          }
          if (this->rx_message_.size() < static_cast<size_t>(event_data->payload_offset)) {
            this->rx_message_.resize(event_data->payload_offset);
          }
          this->rx_message_.append(event_data->data_ptr, event_data->data_len);
          if (event_data->payload_offset + event_data->data_len >= event_data->payload_len) {
            this->queue_json_message_(reinterpret_cast<const uint8_t *>(this->rx_message_.data()), this->rx_message_.size());
            this->rx_message_.clear();
          }
        }
      }
      break;
    case WEBSOCKET_EVENT_ERROR:
      ESP_LOGW(TAG, "Realtime websocket error");
      {
        LockGuard lock(this->pending_websocket_events_lock_);
        this->pending_websocket_error_ = true;
      }
      App.wake_loop_threadsafe();
      break;
    default:
      break;
  }
}

void OpenAIAssistant::handle_json_message_(const uint8_t *data, size_t len) {
  JsonDocument doc = json::parse_json(data, len);
  JsonObject root = doc.as<JsonObject>();
  if (root.isNull()) {
    ESP_LOGW(TAG, "Could not parse realtime JSON message of length %u", static_cast<unsigned>(len));
    return;
  }
  const char *type = root["type"] | "";
  ESP_LOGV(TAG, "Realtime event type: %s", type);
  if (strncmp(type, "response.", 9) == 0) {
    this->last_response_event_time_ = millis();
  }

  if (strcmp(type, "session.created") == 0 || strcmp(type, "session.updated") == 0) {
    this->session_configured_ = true;
  } else if (strcmp(type, "input_audio_buffer.speech_started") == 0) {
    this->speech_started_ = true;
    this->stt_vad_start_trigger_.trigger();
  } else if (strcmp(type, "input_audio_buffer.speech_stopped") == 0) {
    this->stt_vad_end_trigger_.trigger();
    if (this->silence_detection_) {
      this->set_state_(State::STOP_MICROPHONE);
    }
  } else if (strcmp(type, "input_audio_buffer.committed") == 0) {
    this->input_committed_ = true;
    this->input_committed_time_ = millis();
  } else if (strcmp(type, "conversation.item.input_audio_transcription.delta") == 0) {
    this->request_text_ += root["delta"] | "";
    this->publish_request_text_(this->request_text_.c_str());
  } else if (strcmp(type, "conversation.item.input_audio_transcription.completed") == 0) {
    const char *transcript = root["transcript"] | "";
    this->request_text_ = transcript;
    this->publish_request_text_(transcript);
    this->stt_end_trigger_.trigger(transcript);
    this->transcription_completed_ = true;
    if (this->input_committed_ && !this->response_requested_) {
      this->send_response_create_();
    }
  } else if (strcmp(type, "conversation.item.input_audio_transcription.failed") == 0) {
    ESP_LOGW(TAG, "Input audio transcription failed; waiting briefly before requesting response");
  } else if (strcmp(type, "response.audio_transcript.delta") == 0 ||
             strcmp(type, "response.output_audio_transcript.delta") == 0) {
    const char *delta = root["delta"] | "";
    if (!this->response_text_active_) {
      this->response_text_active_ = true;
      this->response_text_.clear();
      this->set_state_(State::STREAMING_RESPONSE);
      this->tts_start_trigger_.trigger(delta);
    }
    this->response_text_ += delta;
    if (millis() - this->last_response_publish_time_ > RESPONSE_TEXT_PUBLISH_INTERVAL_MS) {
      this->last_response_publish_time_ = millis();
      this->publish_response_text_(this->response_text_.c_str());
    }
  } else if (strcmp(type, "response.audio_transcript.done") == 0 ||
             strcmp(type, "response.output_audio_transcript.done") == 0) {
    const char *transcript = root["transcript"] | "";
    this->response_text_ = transcript;
    this->publish_response_text_(transcript);
    this->tts_end_trigger_.trigger(transcript);
  } else if (strcmp(type, "response.audio.delta") == 0 || strcmp(type, "response.output_audio.delta") == 0) {
    const char *delta = root["delta"] | "";
    this->handle_audio_delta_(delta, strlen(delta));
  } else if (strcmp(type, "response.audio.done") == 0 || strcmp(type, "response.output_audio.done") == 0) {
    if (this->tts_streaming_) {
      this->tts_streaming_ = false;
      this->tts_stream_end_trigger_.trigger();
#ifdef USE_SPEAKER
      if (this->speaker_ != nullptr) {
        this->speaker_->finish();
      }
#endif
    }
  } else if (strcmp(type, "response.output_item.done") == 0) {
    this->finish_response_();
  } else if (strcmp(type, "response.done") == 0) {
    this->log_response_status_(root);
    this->finish_response_();
  } else if (strcmp(type, "error") == 0) {
    JsonObject error = root["error"];
    const char *code = error["code"] | "error";
    const char *message = error["message"] | "Realtime API error";
    const char *param = error["param"] | "";
    ESP_LOGW(TAG, "Realtime API error: code='%s' message='%s' param='%s'", code, message, param);
    this->error_trigger_.trigger(code, message);
  }
}

void OpenAIAssistant::log_response_status_(JsonObject root) {
  JsonObject response = root["response"];
  if (response.isNull()) {
    ESP_LOGW(TAG, "response.done did not include a response object");
    return;
  }

  const char *status = response["status"] | "";
  JsonObject status_details = response["status_details"];
  const char *detail_type = status_details["type"] | "";
  const char *reason = status_details["reason"] | "";
  JsonObject error = status_details["error"];
  const char *error_type = error["type"] | "";
  const char *error_code = error["code"] | "";

  JsonArray output_modalities = response["output_modalities"].as<JsonArray>();
  std::string modalities;
  for (const char *modality : output_modalities) {
    if (!modalities.empty()) {
      modalities += ",";
    }
    modalities += modality;
  }

  ESP_LOGI(TAG, "Realtime response done: status='%s' detail_type='%s' reason='%s' error_type='%s' error_code='%s' "
                "output_modalities='%s' output_items=%u",
           status, detail_type, reason, error_type, error_code, modalities.c_str(),
           static_cast<unsigned>(response["output"].size()));

  JsonArray output = response["output"].as<JsonArray>();
  for (JsonObject item : output) {
    JsonArray content = item["content"].as<JsonArray>();
    for (JsonObject part : content) {
      const char *part_type = part["type"] | "";
      const char *transcript = part["transcript"] | "";
      if (transcript[0] != '\0') {
        ESP_LOGI(TAG, "Realtime response output part: type='%s' transcript='%s'", part_type, transcript);
        this->response_text_ = transcript;
        this->publish_response_text_(this->response_text_.c_str());
      }
    }
  }
}

void OpenAIAssistant::handle_audio_delta_(const char *delta, size_t len) {
#ifdef USE_SPEAKER
  if (this->speaker_ == nullptr || this->speaker_buffer_ == nullptr) {
    ESP_LOGW(TAG, "Received realtime audio delta but no speaker is configured");
    return;
  }
  if (!this->tts_streaming_) {
    this->tts_streaming_ = true;
    this->set_state_(State::STREAMING_RESPONSE);
    this->speaker_->start();
    this->tts_stream_start_trigger_.trigger();
  }

  size_t offset = 0;
  while (offset < len) {
    if (this->speaker_buffer_size_ == SPEAKER_BUFFER_SIZE) {
      size_t written = this->speaker_->play(this->speaker_buffer_, this->speaker_buffer_size_);
      if (written == 0) {
        ESP_LOGW(TAG, "Speaker buffer full, dropping remainder of realtime audio chunk");
        return;
      }
      memmove(this->speaker_buffer_, this->speaker_buffer_ + written, this->speaker_buffer_size_ - written);
      this->speaker_buffer_size_ -= written;
    }

    const size_t available = SPEAKER_BUFFER_SIZE - this->speaker_buffer_size_;
    size_t chars_to_decode = std::min(len - offset, (available / 3) * 4);
    chars_to_decode -= chars_to_decode % 4;
    if (chars_to_decode == 0) {
      size_t written = this->speaker_->play(this->speaker_buffer_, this->speaker_buffer_size_);
      if (written == 0) {
        ESP_LOGW(TAG, "Speaker could not accept realtime audio; dropping remainder of chunk");
        return;
      }
      memmove(this->speaker_buffer_, this->speaker_buffer_ + written, this->speaker_buffer_size_ - written);
      this->speaker_buffer_size_ -= written;
      continue;
    }

    size_t decoded = base64_decode(reinterpret_cast<const uint8_t *>(delta + offset), chars_to_decode,
                                   this->speaker_buffer_ + this->speaker_buffer_size_, available);
    auto *samples = reinterpret_cast<int16_t *>(this->speaker_buffer_ + this->speaker_buffer_size_);
    for (size_t i = 0; i < decoded / sizeof(int16_t); i++) {
      samples[i] = clamp<int32_t>(static_cast<int32_t>(samples[i] * this->volume_multiplier_), INT16_MIN, INT16_MAX);
    }
    this->speaker_buffer_size_ += decoded;
    this->response_audio_bytes_received_ += decoded;
    offset += chars_to_decode;
  }
  this->response_audio_chunks_received_++;
  ESP_LOGD(TAG, "Received realtime audio: %" PRIu32 " bytes in %" PRIu32 " chunks", this->response_audio_bytes_received_,
           this->response_audio_chunks_received_);
#endif
}

void OpenAIAssistant::publish_request_text_(const char *text) {
#ifdef USE_TEXT_SENSOR
  if (this->text_request_sensor_ != nullptr) {
    this->text_request_sensor_->publish_state(text);
  }
#endif
}

void OpenAIAssistant::publish_response_text_(const char *text) {
#ifdef USE_TEXT_SENSOR
  if (this->text_response_sensor_ != nullptr) {
    this->text_response_sensor_->publish_state(text);
  }
#endif
}

}  // namespace esphome::openai_assistant

#endif  // USE_OPENAI_ASSISTANT
