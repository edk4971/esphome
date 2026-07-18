#pragma once

#include "esphome/core/defines.h"

#ifdef USE_OPENAI_COMMON

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/static_task.h"

#include "esphome/components/audio/audio.h"
#include "esphome/components/microphone/microphone_source.h"
#include "esphome/components/speaker/speaker.h"
#ifdef USE_MICRO_WAKE_WORD
#include "esphome/components/micro_wake_word/micro_wake_word.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace esphome::openai_common {

/// Shared infrastructure common to all three OpenAI voice assistant
/// components (responses, conversations, realtime). Holds mic/speaker/MWW
/// wiring, text sensors, all lazy callbacks, shared config fields, the 16 KiB
/// speaker buffer with volume application, MWW lifecycle helpers, teardown and
/// failure signalling. Protocol-specific cleanup is provided by overriding
/// ``teardown_to_idle_()``.
class OpenAIBase : public Component {
 public:
  OpenAIBase() = default;
  ~OpenAIBase() = default;

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

  // --- Shared config field setters ---
  void set_endpoint_base(const std::string &v) { this->endpoint_base_ = v; }
  void set_api_key(const std::string &v) { this->api_key_ = v; }
  void set_chat_model(const std::string &v) { this->chat_model_ = v; }
  void set_system_prompt(const std::string &v) { this->system_prompt_ = v; }
  void set_silence_threshold(float v) { this->silence_threshold_ = v; }
  void set_silence_duration_ms(uint32_t v) { this->silence_duration_ms_ = v; }
  void set_max_recording_ms(uint32_t v) { this->max_recording_ms_ = v; }
  void set_volume_multiplier(float v) { this->volume_multiplier_ = v; }
  void set_wake_word(const std::string &v) { this->wake_word_ = v; }

  // --- Public API (used by actions/conditions) ---
  bool is_running() const { return this->is_active_(); }

  // --- MWW lifecycle helpers ---
  void stop_mic_on_turn_start_();
  void restart_mic_after_turn_end_();

  // --- Speaker buffer (16 KiB) with volume application ---
  void feed_speaker_(const uint8_t *data, size_t len);
  void flush_speaker_buffer_();

  // --- Failure signalling ---
  /// Fires on_error and routes to teardown. Virtual so subclasses that defer
  /// teardown through a state machine (e.g. waiting for the speaker/mic to stop
  /// before cleaning up) can override instead of tearing down immediately.
  virtual void fail_(const std::string &code, const std::string &message);

  // --- Callback registration (templatized to accept forwarder structs) ---
  template<typename F> void add_on_start_callback(F &&cb) { this->on_start_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_listening_callback(F &&cb) { this->on_listening_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_wake_word_detected_callback(F &&cb) {
    this->on_wake_word_detected_cb_.add(std::forward<F>(cb));
  }
  template<typename F> void add_on_stt_end_callback(F &&cb) { this->on_stt_end_cb_.add(std::forward<F>(cb)); }
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
  /// Subclass reports whether a turn is active (e.g. state_ != IDLE).
  virtual bool is_active_() const = 0;
  /// Subclass cleans up protocol-specific resources (HTTP task, WebSocket,
  /// MCP) then calls base implementation.
  virtual void teardown_to_idle_();

  // Shared wiring.
  microphone::MicrophoneSource *mic_source_{nullptr};
  speaker::Speaker *speaker_{nullptr};
#ifdef USE_MICRO_WAKE_WORD
  micro_wake_word::MicroWakeWord *micro_wake_word_{nullptr};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *text_request_sensor_{nullptr};
  text_sensor::TextSensor *text_response_sensor_{nullptr};
#endif

  // Shared config fields.
  std::string endpoint_base_;
  std::string api_key_;
  std::string chat_model_;
  std::string system_prompt_;
  float silence_threshold_{0.002f};
  uint32_t silence_duration_ms_{700};
  uint32_t max_recording_ms_{30000};
  float volume_multiplier_{1.0f};
  std::string wake_word_;

  // Authorization header value derived from api_key_ ("Bearer <key>").
  std::string auth_header_value_;

  // 16 KiB speaker buffer for non-streaming playback (volume applied here).
  static constexpr size_t SPEAKER_BUFFER_SIZE = 16384;
  uint8_t *speaker_buffer_{nullptr};
  size_t speaker_buffer_index_{0};
  bool tts_header_skipped_{false};

  // --- Callback managers (lazy: 4 bytes each when empty) ---
  LazyCallbackManager<void()> on_start_cb_;
  LazyCallbackManager<void()> on_listening_cb_;
  LazyCallbackManager<void()> on_wake_word_detected_cb_;
  LazyCallbackManager<void(std::string)> on_stt_end_cb_;
  LazyCallbackManager<void(std::string)> on_tts_start_cb_;
  LazyCallbackManager<void(std::string)> on_tts_end_cb_;
  LazyCallbackManager<void()> on_tts_stream_start_cb_;
  LazyCallbackManager<void()> on_tts_stream_end_cb_;
  LazyCallbackManager<void()> on_end_cb_;
  LazyCallbackManager<void()> on_idle_cb_;
  LazyCallbackManager<void(std::string, std::string)> on_error_cb_;
};

}  // namespace esphome::openai_common

#endif  // USE_OPENAI_COMMON
