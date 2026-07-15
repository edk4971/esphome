#pragma once

#include "esphome/core/defines.h"

#ifdef USE_OPENAI_ASSISTANT

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/static_task.h"

#include "esphome/components/microphone/microphone_source.h"
#include "esphome/components/ring_buffer/ring_buffer.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/components/openai_common/openai_audio.h"
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_MICRO_WAKE_WORD
#include "esphome/components/micro_wake_word/micro_wake_word.h"
#endif

#include <esp_websocket_client.h>

#include <map>
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
  DRAINING_AUDIO,  // response.done received; draining remaining audio before teardown
};

struct InFlightToolCall {
  std::string call_id;
  std::string name;
  std::string arguments;
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
  void set_speaker(speaker::Speaker *speaker) { this->speaker_ = speaker; }
#ifdef USE_MICRO_WAKE_WORD
  void set_micro_wake_word(micro_wake_word::MicroWakeWord *mww) { this->micro_wake_word_ = mww; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_text_request_sensor(text_sensor::TextSensor *sensor) { this->text_request_sensor_ = sensor; }
  void set_text_response_sensor(text_sensor::TextSensor *sensor) { this->text_response_sensor_ = sensor; }
#endif

  void set_api_key(const std::string &api_key) { this->api_key_ = api_key; }
  void set_model(const std::string &model) { this->model_ = model; }
  void set_endpoint(const std::string &endpoint) { this->endpoint_ = endpoint; }
  void set_system_prompt(const std::string &system_prompt) { this->system_prompt_ = system_prompt; }
  void set_voice(const std::string &voice) { this->voice_ = voice; }
  void set_language(const std::string &language) { this->language_ = language; }
  void set_noise_suppression_level(uint8_t level) { this->noise_suppression_level_ = level; }
  void set_auto_gain(uint8_t gain) { this->auto_gain_ = gain; }
  void set_volume_multiplier(float multiplier) { this->volume_multiplier_ = multiplier; }
  void set_tools(const std::string &tools_json) { this->tools_json_ = tools_json; }
  void set_tools(const char *tools_json) { this->tools_json_ = tools_json; }
  void set_has_tools(bool has_tools) { this->has_tools_ = has_tools; }

  void set_wake_word(const std::string &wake_word) { this->wake_word_ = wake_word; }

  void request_start(bool silence_detection);
  void request_stop();

  /// No-op stubs for API compatibility with openai_responses/conversations.
  /// Realtime tools are server-side and models are preloaded by the endpoint.
  void prefetch_tools() {}
  void prewarm_models() {}

  bool is_running() const { return this->state_ != State::IDLE; }
  bool is_connected() const { return this->connected_; }

  // --- Callback registration (template-based, LazyCallbackManager) ---
  // Matches the pattern used by openai_responses/conversations. Each callback
  // is 4 bytes when empty (vs. Trigger's vtable overhead).
  template<typename F> void add_on_listening_callback(F &&cb) { this->on_listening_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_start_callback(F &&cb) { this->on_start_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_wake_word_detected_callback(F &&cb) {
    this->on_wake_word_detected_cb_.add(std::forward<F>(cb));
  }
  template<typename F> void add_on_stt_vad_start_callback(F &&cb) { this->on_stt_vad_start_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_stt_vad_end_callback(F &&cb) { this->on_stt_vad_end_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_stt_end_callback(F &&cb) { this->on_stt_end_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_tool_start_callback(F &&cb) { this->on_tool_start_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_tts_start_callback(F &&cb) { this->on_tts_start_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_tts_end_callback(F &&cb) { this->on_tts_end_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_tts_stream_start_callback(F &&cb) { this->on_tts_stream_start_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_tts_stream_end_callback(F &&cb) { this->on_tts_stream_end_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_end_callback(F &&cb) { this->on_end_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_error_callback(F &&cb) { this->on_error_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_idle_callback(F &&cb) { this->on_idle_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_client_connected_callback(F &&cb) { this->on_client_connected_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_client_disconnected_callback(F &&cb) {
    this->on_client_disconnected_cb_.add(std::forward<F>(cb));
  }

 protected:
  bool allocate_buffers_();
  void clear_buffers_();
  void deallocate_buffers_();

  void connect_();
  void disconnect_();
  void set_state_(State state);
  void finish_response_();
  void finish_response_now_();

  void send_session_update_();
  void send_audio_append_(const uint8_t *data, size_t len);
  bool send_text_(const char *text, size_t len);

  void queue_json_message_(const uint8_t *data, size_t len);
  void process_pending_json_messages_();
  void process_pending_flags_();
  void handle_websocket_event_(esp_websocket_event_id_t event_id, esp_websocket_event_data_t *event_data);
  void handle_json_message_(const uint8_t *data, size_t len);
  void handle_audio_delta_(const char *delta, size_t len);
  void start_audio_pipeline_();
  void stop_audio_pipeline_();
  static void audio_producer_task_fn_(void *arg);
  void log_response_status_(JsonObject root);

  void publish_request_text_(const std::string &text);
  void publish_response_text_(const std::string &text, bool force = false);

  void start_no_speech_timeout_();
  void start_response_timeout_();
  void start_connection_timeout_();
  void cancel_no_speech_timeout_();
  void cancel_response_timeout_();
  void cancel_connection_timeout_();

  static void websocket_event_handler_(void *handler_args, esp_event_base_t base, int32_t event_id,
                                       void *event_data);

  microphone::MicrophoneSource *mic_source_{nullptr};
  speaker::Speaker *speaker_{nullptr};
#ifdef USE_MICRO_WAKE_WORD
  micro_wake_word::MicroWakeWord *micro_wake_word_{nullptr};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *text_request_sensor_{nullptr};
  text_sensor::TextSensor *text_response_sensor_{nullptr};
#endif

  esp_websocket_client_handle_t websocket_{nullptr};

  std::string api_key_;
  std::string model_;
  std::string endpoint_;
  std::string endpoint_with_model_;
  std::string headers_;
  std::string system_prompt_;
  std::string voice_;
  std::string language_;
  std::string wake_word_;

  uint8_t noise_suppression_level_{0};
  uint8_t auto_gain_{0};
  float volume_multiplier_{1.0f};

  std::unique_ptr<ring_buffer::RingBuffer> ring_buffer_;

  // PSRAM ring buffer + feeder task for continuous, crackle-free speaker
  // playback. The audio producer task decodes base64 deltas and writes PCM
  // to the ring buffer; the feeder task (inside PsramAudioBuffer) reads PCM
  // and feeds the speaker continuously, decoupled from the main loop.
  openai_common::PsramAudioBuffer audio_buffer_;
  StaticTask audio_producer_task_;
  static constexpr uint32_t AUDIO_PRODUCER_STACK_SIZE = 4096;
  static constexpr UBaseType_t AUDIO_TASK_PRIORITY = 3;
  SemaphoreHandle_t audio_delta_mutex_{nullptr};
  volatile bool audio_producer_should_exit_{false};

  // Audio delta queue. Each delta is copied into PSRAM and decoded by the
  // audio producer task (not the main loop). The queue is protected by
  // audio_delta_mutex_ since handle_audio_delta_() (main loop) pushes and
  // audio_producer_task_fn_ (producer task) pops.
  struct AudioDelta {
    uint8_t *data{nullptr};   // PSRAM-allocated base64 data
    size_t len{0};            // total base64 length
  };
  std::vector<AudioDelta> audio_delta_queue_;
  static constexpr size_t MAX_AUDIO_DELTA_QUEUE = 64;  // safety cap

  // PSRAM-backed receive buffer for assembling fragmented websocket JSON frames.
  // Allocated once in setup() and reused for every frame — avoids per-frame heap
  // churn and keeps 256 KB of buffer in PSRAM instead of internal RAM.
  uint8_t *rx_message_buffer_{nullptr};
  size_t rx_message_len_{0};
  size_t rx_oversized_payload_len_{0};
  bool rx_drop_oversized_payload_{false};

  // Pending websocket messages, stored as PSRAM-backed raw buffers.
  // Each entry owns its bytes (allocated via ExternalRAMAllocator) so large audio
  // delta frames don't exhaust internal RAM while waiting for the main loop to
  // process them.
  struct PendingMessage {
    uint8_t *data{nullptr};
    size_t len{0};
  };
  std::vector<PendingMessage> pending_json_messages_;
  Mutex pending_json_messages_lock_;

  bool pending_client_connected_{false};
  bool pending_client_disconnected_{false};
  bool pending_websocket_error_{false};
  bool pending_idle_{false};  // deferred until speaker stops (i2s bus contention)
  bool pending_finish_{false};  // response.done received; finish after audio drains
  Mutex pending_flags_lock_;

  State state_{State::IDLE};
  bool connected_{false};
  bool session_configured_{false};
  bool silence_detection_{true};
  bool tts_streaming_{false};
  bool speech_started_{false};
  bool tool_start_fired_{false};

  std::string request_text_;
  std::string response_text_;
  std::string tools_json_;  // JSON-serialized tool definitions for session.update
  bool has_tools_{false};
  uint32_t last_response_publish_time_{0};

  uint32_t last_response_event_time_{0};
  uint32_t last_audio_stats_time_{0};
  uint32_t mic_bytes_received_{0};
  uint32_t audio_bytes_sent_{0};
  uint32_t audio_chunks_sent_{0};
  uint32_t response_audio_bytes_received_{0};
  uint32_t response_audio_chunks_received_{0};

  std::map<std::string, InFlightToolCall> in_flight_tool_calls_;

  // --- Callback managers (lazy: 4 bytes each when empty) ---
  LazyCallbackManager<void()> on_listening_cb_;
  LazyCallbackManager<void()> on_start_cb_;
  LazyCallbackManager<void()> on_wake_word_detected_cb_;
  LazyCallbackManager<void()> on_stt_vad_start_cb_;
  LazyCallbackManager<void()> on_stt_vad_end_cb_;
  LazyCallbackManager<void(std::string)> on_stt_end_cb_;
  LazyCallbackManager<void()> on_tool_start_cb_;
  LazyCallbackManager<void(std::string)> on_tts_start_cb_;
  LazyCallbackManager<void(std::string)> on_tts_end_cb_;
  LazyCallbackManager<void()> on_tts_stream_start_cb_;
  LazyCallbackManager<void()> on_tts_stream_end_cb_;
  LazyCallbackManager<void()> on_end_cb_;
  LazyCallbackManager<void(std::string, std::string)> on_error_cb_;
  LazyCallbackManager<void()> on_idle_cb_;
  LazyCallbackManager<void()> on_client_connected_cb_;
  LazyCallbackManager<void()> on_client_disconnected_cb_;
};

template<typename... Ts> class StartAction : public Action<Ts...>, public Parented<OpenAIAssistant> {
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

template<typename... Ts> class StopAction : public Action<Ts...>, public Parented<OpenAIAssistant> {
 public:
  void play(const Ts &...x) override { this->parent_->request_stop(); }
};

template<typename... Ts> class IsRunningCondition : public Condition<Ts...>, public Parented<OpenAIAssistant> {
 public:
  bool check(const Ts &...x) override { return this->parent_->is_running(); }
};

template<typename... Ts> class ConnectedCondition : public Condition<Ts...>, public Parented<OpenAIAssistant> {
 public:
  bool check(const Ts &...x) override { return this->parent_->is_connected(); }
};

}  // namespace esphome::openai_assistant

#endif  // USE_OPENAI_ASSISTANT
