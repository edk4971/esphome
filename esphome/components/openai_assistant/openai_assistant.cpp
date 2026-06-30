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
#include <cstring>
#include <cinttypes>

namespace esphome::openai_assistant {

static const char *const TAG = "openai_assistant";
static const size_t SAMPLE_RATE_HZ = 16000;
static const size_t RING_BUFFER_SAMPLES = 512 * SAMPLE_RATE_HZ / 1000;
static const size_t RING_BUFFER_SIZE = RING_BUFFER_SAMPLES * sizeof(int16_t);
static const size_t SEND_BUFFER_SAMPLES = 32 * SAMPLE_RATE_HZ / 1000;
static const size_t SEND_BUFFER_SIZE = SEND_BUFFER_SAMPLES * sizeof(int16_t);
static const size_t SPEAKER_BUFFER_SIZE = 4096;
static const uint32_t CONNECT_TIMEOUT_MS = 15000;

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
    this->speaker_->set_audio_stream_info(audio::AudioStreamInfo(16, 1, SAMPLE_RATE_HZ));
  }
#endif
}

float OpenAIAssistant::get_setup_priority() const { return setup_priority::AFTER_CONNECTION; }

void OpenAIAssistant::dump_config() {
  ESP_LOGCONFIG(TAG, "OpenAI Assistant:");
  ESP_LOGCONFIG(TAG, "  Endpoint: %s", this->endpoint_.c_str());
  ESP_LOGCONFIG(TAG, "  Model: %s", this->model_.c_str());
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
      break;
    }
    case State::STOP_MICROPHONE:
      if (this->mic_source_->is_running()) {
        this->mic_source_->stop();
        this->set_state_(State::STOPPING_MICROPHONE);
      } else {
        this->set_state_(State::AWAITING_RESPONSE);
      }
      break;
    case State::STOPPING_MICROPHONE:
      if (this->mic_source_->is_stopped()) {
        this->set_state_(State::AWAITING_RESPONSE);
      }
      break;
    case State::AWAITING_RESPONSE:
      if (!this->connected_) {
        this->set_state_(State::IDLE);
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
      break;
  }
}

void OpenAIAssistant::send_session_update_() {
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  root["type"] = "session.update";
  JsonObject session = root["session"].to<JsonObject>();
  session["instructions"] = this->system_prompt_;
  JsonArray modalities = session["modalities"].to<JsonArray>();
  modalities.add("text");
  modalities.add("audio");
  session["input_audio_format"] = "pcm16";
  session["output_audio_format"] = "pcm16";
  JsonObject turn_detection = session["turn_detection"].to<JsonObject>();
  if (this->silence_detection_) {
    turn_detection["type"] = "server_vad";
    turn_detection["create_response"] = true;
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

void OpenAIAssistant::signal_stop_() {
  if (!this->connected_) {
    return;
  }
  ESP_LOGD(TAG, "Committing input audio buffer and requesting response");
  this->send_text_("{\"type\":\"input_audio_buffer.commit\"}");
  this->send_text_("{\"type\":\"response.create\",\"response\":{\"modalities\":[\"text\",\"audio\"]}}");
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
  this->end_trigger_.trigger();
  if (this->continuous_) {
    this->idle_trigger_.trigger();
    this->set_state_(State::START_MICROPHONE);
  } else {
    this->set_state_(State::IDLE);
    this->idle_trigger_.trigger();
  }
}

void OpenAIAssistant::queue_json_message_(const uint8_t *data, size_t len) {
  {
    LockGuard lock(this->pending_json_messages_lock_);
    if (this->pending_json_messages_.size() >= 8) {
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
          this->queue_json_message_(reinterpret_cast<const uint8_t *>(event_data->data_ptr), event_data->data_len);
        } else {
          if (event_data->payload_offset == 0) {
            this->rx_message_.clear();
            this->rx_message_.reserve(event_data->payload_len);
          }
          this->rx_message_.append(event_data->data_ptr, event_data->data_len);
          if (this->rx_message_.size() >= event_data->payload_len) {
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

  if (strcmp(type, "session.created") == 0 || strcmp(type, "session.updated") == 0) {
    this->session_configured_ = true;
  } else if (strcmp(type, "input_audio_buffer.speech_started") == 0) {
    this->stt_vad_start_trigger_.trigger();
  } else if (strcmp(type, "input_audio_buffer.speech_stopped") == 0) {
    this->stt_vad_end_trigger_.trigger();
    if (this->silence_detection_) {
      this->set_state_(State::STOP_MICROPHONE);
    }
  } else if (strcmp(type, "conversation.item.input_audio_transcription.delta") == 0) {
    this->request_text_ += root["delta"] | "";
    this->publish_request_text_(this->request_text_.c_str());
  } else if (strcmp(type, "conversation.item.input_audio_transcription.completed") == 0) {
    const char *transcript = root["transcript"] | "";
    this->request_text_ = transcript;
    this->publish_request_text_(transcript);
    this->stt_end_trigger_.trigger(transcript);
  } else if (strcmp(type, "response.audio_transcript.delta") == 0 ||
             strcmp(type, "response.output_audio_transcript.delta") == 0) {
    const char *delta = root["delta"] | "";
    if (!this->response_text_active_) {
      this->response_text_active_ = true;
      this->response_text_.clear();
      this->tts_start_trigger_.trigger(delta);
    }
    this->response_text_ += delta;
    this->publish_response_text_(this->response_text_.c_str());
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

void OpenAIAssistant::handle_audio_delta_(const char *delta, size_t len) {
#ifdef USE_SPEAKER
  if (this->speaker_ == nullptr || this->speaker_buffer_ == nullptr) {
    return;
  }
  if (!this->tts_streaming_) {
    this->tts_streaming_ = true;
    this->set_state_(State::STREAMING_RESPONSE);
    this->speaker_->start();
    this->tts_stream_start_trigger_.trigger();
  }
  size_t available = SPEAKER_BUFFER_SIZE - this->speaker_buffer_size_;
  const size_t maximum_decoded_size = (len / 4) * 3;
  if (maximum_decoded_size > available) {
    ESP_LOGW(TAG, "Speaker buffer full, dropping realtime audio chunk");
    return;
  }
  size_t decoded = base64_decode(reinterpret_cast<const uint8_t *>(delta), len,
                                 this->speaker_buffer_ + this->speaker_buffer_size_, available);
  this->speaker_buffer_size_ += decoded;
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
