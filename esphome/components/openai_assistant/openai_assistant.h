#pragma once

#include "esphome/core/defines.h"

#ifdef USE_OPENAI_ASSISTANT

#include "esphome/core/automation.h"
#include "esphome/core/component.h"

#include "esphome/components/microphone/microphone_source.h"
#include "esphome/components/ring_buffer/ring_buffer.h"
#ifdef USE_SPEAKER
#include "esphome/components/speaker/speaker.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif

#include <esp_websocket_client.h>

#include <memory>
#include <string>
#include <vector>

namespace esphome::openai_assistant {

enum class State : uint8_t {
  IDLE,
  CONNECTING,
  START_MICROPHONE,
  STARTING_MICROPHONE,
  STREAMING_MICROPHONE,
  STOP_MICROPHONE,
  STOPPING_MICROPHONE,
  AWAITING_RESPONSE,
  STREAMING_RESPONSE,
};

class OpenAIAssistant : public Component {
 public:
  OpenAIAssistant();
  ~OpenAIAssistant() override;

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override;

  void set_microphone_source(microphone::MicrophoneSource *mic_source) { this->mic_source_ = mic_source; }
  void set_api_key(const std::string &api_key) { this->api_key_ = api_key; }
  void set_model(const std::string &model) { this->model_ = model; }
  void set_endpoint(const std::string &endpoint) { this->endpoint_ = endpoint; }
  void set_system_prompt(const std::string &system_prompt) { this->system_prompt_ = system_prompt; }
  void set_use_wake_word(bool use_wake_word) { this->use_wake_word_ = use_wake_word; }
#ifdef USE_SPEAKER
  void set_speaker(speaker::Speaker *speaker) { this->speaker_ = speaker; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_text_request_sensor(text_sensor::TextSensor *sensor) { this->text_request_sensor_ = sensor; }
  void set_text_response_sensor(text_sensor::TextSensor *sensor) { this->text_response_sensor_ = sensor; }
#endif

  void request_start(bool continuous, bool silence_detection);
  void request_stop();
  bool is_running() const { return this->state_ != State::IDLE; }
  bool is_connected() const { return this->connected_; }
  bool is_continuous() const { return this->continuous_; }

  Trigger<> *get_listening_trigger() { return &this->listening_trigger_; }
  Trigger<> *get_start_trigger() { return &this->start_trigger_; }
  Trigger<> *get_stt_vad_start_trigger() { return &this->stt_vad_start_trigger_; }
  Trigger<> *get_stt_vad_end_trigger() { return &this->stt_vad_end_trigger_; }
  Trigger<std::string> *get_stt_end_trigger() { return &this->stt_end_trigger_; }
  Trigger<std::string> *get_tts_start_trigger() { return &this->tts_start_trigger_; }
  Trigger<std::string> *get_tts_end_trigger() { return &this->tts_end_trigger_; }
  Trigger<> *get_tts_stream_start_trigger() { return &this->tts_stream_start_trigger_; }
  Trigger<> *get_tts_stream_end_trigger() { return &this->tts_stream_end_trigger_; }
  Trigger<> *get_end_trigger() { return &this->end_trigger_; }
  Trigger<std::string, std::string> *get_error_trigger() { return &this->error_trigger_; }
  Trigger<> *get_idle_trigger() { return &this->idle_trigger_; }
  Trigger<> *get_client_connected_trigger() { return &this->client_connected_trigger_; }
  Trigger<> *get_client_disconnected_trigger() { return &this->client_disconnected_trigger_; }

 protected:
  bool allocate_buffers_();
  void clear_buffers_();
  void deallocate_buffers_();
  bool connect_();
  void disconnect_();
  void set_state_(State state);
  void send_session_update_();
  void send_audio_append_(const uint8_t *data, size_t len);
  void send_text_(const std::string &text);
  void signal_stop_();
  void handle_websocket_event_(esp_websocket_event_id_t event_id, esp_websocket_event_data_t *event_data);
  void handle_json_message_(const uint8_t *data, size_t len);
  void handle_audio_delta_(const char *delta, size_t len);
  void publish_request_text_(const char *text);
  void publish_response_text_(const char *text);

  static void websocket_event_handler_(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);

  microphone::MicrophoneSource *mic_source_{nullptr};
  std::unique_ptr<ring_buffer::RingBuffer> ring_buffer_;

#ifdef USE_SPEAKER
  speaker::Speaker *speaker_{nullptr};
  uint8_t *speaker_buffer_{nullptr};
  size_t speaker_buffer_size_{0};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *text_request_sensor_{nullptr};
  text_sensor::TextSensor *text_response_sensor_{nullptr};
#endif

  esp_websocket_client_handle_t websocket_{nullptr};
  std::string endpoint_;
  std::string endpoint_with_model_;
  std::string headers_;
  std::string api_key_;
  std::string model_;
  std::string system_prompt_;
  std::string rx_message_;
  std::string request_text_;
  std::string response_text_;

  bool connected_{false};
  bool session_configured_{false};
  bool continuous_{false};
  bool silence_detection_{true};
  bool use_wake_word_{false};
  bool tts_streaming_{false};
  bool response_text_active_{false};
  State state_{State::IDLE};

  Trigger<> listening_trigger_;
  Trigger<> start_trigger_;
  Trigger<> stt_vad_start_trigger_;
  Trigger<> stt_vad_end_trigger_;
  Trigger<std::string> stt_end_trigger_;
  Trigger<std::string> tts_start_trigger_;
  Trigger<std::string> tts_end_trigger_;
  Trigger<> tts_stream_start_trigger_;
  Trigger<> tts_stream_end_trigger_;
  Trigger<> end_trigger_;
  Trigger<std::string, std::string> error_trigger_;
  Trigger<> idle_trigger_;
  Trigger<> client_connected_trigger_;
  Trigger<> client_disconnected_trigger_;
};

template<typename... Ts> class StartAction : public Action<Ts...>, public Parented<OpenAIAssistant> {
 public:
  void play(const Ts &...x) override { this->parent_->request_start(false, this->silence_detection_); }
  void set_silence_detection(bool silence_detection) { this->silence_detection_ = silence_detection; }

 protected:
  bool silence_detection_{true};
};

template<typename... Ts> class StartContinuousAction : public Action<Ts...>, public Parented<OpenAIAssistant> {
 public:
  void play(const Ts &...x) override { this->parent_->request_start(true, true); }
};

template<typename... Ts> class StopAction : public Action<Ts...>, public Parented<OpenAIAssistant> {
 public:
  void play(const Ts &...x) override { this->parent_->request_stop(); }
};

template<typename... Ts> class IsRunningCondition : public Condition<Ts...>, public Parented<OpenAIAssistant> {
 public:
  bool check(const Ts &...x) override { return this->parent_->is_running() || this->parent_->is_continuous(); }
};

template<typename... Ts> class ConnectedCondition : public Condition<Ts...>, public Parented<OpenAIAssistant> {
 public:
  bool check(const Ts &...x) override { return this->parent_->is_connected(); }
};

}  // namespace esphome::openai_assistant

#endif  // USE_OPENAI_ASSISTANT
