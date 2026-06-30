#pragma once

#include "esphome/core/defines.h"

#ifdef USE_OPENAI_ASSISTANT

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include "esphome/components/microphone/microphone_source.h"
#include "esphome/components/ring_buffer/ring_buffer.h"
#ifdef USE_MEDIA_PLAYER
#include "esphome/components/media_player/media_player.h"
#endif
#ifdef USE_MICRO_WAKE_WORD
#include "esphome/components/micro_wake_word/micro_wake_word.h"
#endif
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

struct Timer {
  std::string id;
  std::string name;
  uint32_t total_seconds{0};
  uint32_t seconds_left{0};
  bool is_active{false};
};

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
  ~OpenAIAssistant();

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
  void set_wake_word(const std::string &wake_word) { this->wake_word_ = wake_word; }
  void set_noise_suppression_level(uint8_t noise_suppression_level) {
    this->noise_suppression_level_ = noise_suppression_level;
  }
  void set_auto_gain(uint8_t auto_gain) { this->auto_gain_ = auto_gain; }
  void set_volume_multiplier(float volume_multiplier) { this->volume_multiplier_ = volume_multiplier; }
#ifdef USE_MICRO_WAKE_WORD
  void set_micro_wake_word(micro_wake_word::MicroWakeWord *mww) { this->micro_wake_word_ = mww; }
#endif
#ifdef USE_MEDIA_PLAYER
  void set_media_player(media_player::MediaPlayer *media_player) { this->media_player_ = media_player; }
#endif
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
  Trigger<> *get_wake_word_detected_trigger() { return &this->wake_word_detected_trigger_; }
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
  Trigger<Timer> *get_timer_started_trigger() { return &this->timer_started_trigger_; }
  Trigger<Timer> *get_timer_updated_trigger() { return &this->timer_updated_trigger_; }
  Trigger<Timer> *get_timer_cancelled_trigger() { return &this->timer_cancelled_trigger_; }
  Trigger<Timer> *get_timer_finished_trigger() { return &this->timer_finished_trigger_; }
  Trigger<const std::vector<Timer> &> *get_timer_tick_trigger() { return &this->timer_tick_trigger_; }
  const std::vector<Timer> &get_timers() const { return this->timers_; }

 protected:
  bool allocate_buffers_();
  void clear_buffers_();
  void deallocate_buffers_();
  bool connect_();
  void disconnect_();
  void set_state_(State state);
  void send_session_update_();
  void send_audio_append_(const uint8_t *data, size_t len);
  bool send_text_(const std::string &text);
  void signal_stop_();
  void finish_response_();
  void queue_json_message_(const uint8_t *data, size_t len);
  void process_pending_websocket_events_();
  void handle_websocket_event_(esp_websocket_event_id_t event_id, esp_websocket_event_data_t *event_data);
  void handle_json_message_(const uint8_t *data, size_t len);
  void handle_audio_delta_(const char *delta, size_t len);
  void publish_request_text_(const char *text);
  void publish_response_text_(const char *text);

  static void websocket_event_handler_(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);

  microphone::MicrophoneSource *mic_source_{nullptr};
  std::unique_ptr<ring_buffer::RingBuffer> ring_buffer_;

#ifdef USE_MICRO_WAKE_WORD
  micro_wake_word::MicroWakeWord *micro_wake_word_{nullptr};
#endif
#ifdef USE_MEDIA_PLAYER
  media_player::MediaPlayer *media_player_{nullptr};
#endif
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
  std::string wake_word_;
  std::string rx_message_;
  std::string request_text_;
  std::string response_text_;
  std::vector<std::string> pending_json_messages_;
  Mutex pending_json_messages_lock_;
  Mutex pending_websocket_events_lock_;

  bool connected_{false};
  bool session_configured_{false};
  bool continuous_{false};
  bool silence_detection_{true};
  bool use_wake_word_{false};
  uint8_t noise_suppression_level_{0};
  uint8_t auto_gain_{0};
  float volume_multiplier_{1.0f};
  bool tts_streaming_{false};
  bool response_text_active_{false};
  State state_{State::IDLE};
  uint32_t connection_start_time_{0};
  uint32_t last_audio_log_time_{0};
  uint32_t mic_bytes_received_{0};
  uint32_t audio_bytes_sent_{0};
  uint32_t audio_chunks_sent_{0};
  uint32_t websocket_send_failures_{0};
  bool pending_client_connected_{false};
  bool pending_client_disconnected_{false};
  bool pending_websocket_error_{false};
  bool pending_session_update_{false};

  std::vector<Timer> timers_;

  Trigger<> listening_trigger_;
  Trigger<> start_trigger_;
  Trigger<> wake_word_detected_trigger_;
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
  Trigger<Timer> timer_started_trigger_;
  Trigger<Timer> timer_finished_trigger_;
  Trigger<Timer> timer_updated_trigger_;
  Trigger<Timer> timer_cancelled_trigger_;
  Trigger<const std::vector<Timer> &> timer_tick_trigger_;
};

template<typename... Ts> class StartAction : public Action<Ts...>, public Parented<OpenAIAssistant> {
  TEMPLATABLE_VALUE(std::string, wake_word)

 public:
  void play(const Ts &...x) override {
    this->parent_->set_wake_word(this->wake_word_.value(x...));
    this->parent_->request_start(false, this->silence_detection_);
  }
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
