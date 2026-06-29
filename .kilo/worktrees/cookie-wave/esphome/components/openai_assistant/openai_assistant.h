#ifndef OPENAI_ASSISTANT_H
#define OPENAI_ASSISTANT_H

//#include "esphome/components/common/common.h"
#include "esphome/components/media_player/media_player.h"
#include "esphome/components/microphone/microphone.h"
#include "esphome/components/micro_wake_word/micro_wake_word.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/components/network/network_component.h"
#include "esphome/core/automation.h"
#include "openai_client.h"

#include <string>
#include <memory>

namespace esphome {
namespace openai_assistant {

struct Timer {
  std::string id;
  std::string name;
  uint32_t total_seconds;
  uint32_t seconds_left;
  bool is_active;

  /// Buffer size for to_str() - sufficient for typical timer names
  static constexpr size_t TO_STR_BUFFER_SIZE = 128;
  /// Format to buffer, returns pointer to buffer (may truncate long names)
  const char *to_str(std::span<char, TO_STR_BUFFER_SIZE> buffer) const {
    snprintf(buffer.data(), buffer.size(),
             "Timer(id=%s, name=%s, total_seconds=%" PRIu32 ", seconds_left=%" PRIu32 ", is_active=%s)",
             this->id.c_str(), this->name.c_str(), this->total_seconds, this->seconds_left, YESNO(this->is_active));
    return buffer.data();
  }
  // Remove before 2026.8.0
  ESPDEPRECATED("Use to_str() instead. Removed in 2026.8.0", "2026.2.0")
  std::string to_string() const {  // NOLINT
    char buffer[TO_STR_BUFFER_SIZE];
    return this->to_str(buffer);
  }
};

enum class State {
    IDLE,
    LISTENING,
    SPEAKING,
    ERROR
};

class OpenAIClient;

class OpenAIAssistant final : public Component {
public:
    OpenAIAssistant(std::shared_ptr<media_player::MediaPlayer> media_player,
                      std::shared_ptr<microphone::MicrophoneSource> microphone_source,
                      std::shared_ptr<micro_wake_word::MicroWakeWord> micro_wake_word,
                      std::shared_ptr<text_sensor::TextSensor> text_request,
                      std::shared_ptr<text_sensor::TextSensor> text_response);

    void setup() override;
    void loop() override;

    void set_api_key(const std::string& api_key);
    void set_model(const std::string& model);
    void set_instructions(const std::string& instructions);
    void set_system_prompt(const std::string& prompt);
    void set_endpoint(const std::string& endpoint);
    void set_vad_threshold(float threshold) {} // Stubbed, Realtime API handles VAD
    void set_noise_suppression_level(uint8_t level) {}
    void set_auto_gain(float gain) {}
    void set_volume_multiplier(float volume) {}
    void set_conversation_timeout(uint32_t timeout) {}
    void set_use_wake_word(bool use_wake_word) {}
    void set_has_timers(bool has_timers) {}
    void handle_start_continuous();
    void on_wake_word_triggered(std::string wake_word);

    const std::vector<Timer> &get_timers() const { return this->timers_; }

    void set_microphone_source(std::shared_ptr<microphone::MicrophoneSource> mic) { this->microphone_source_ = mic; }
    void set_microphone_source2(std::shared_ptr<microphone::MicrophoneSource> mic) {} // Stub for 2nd mic
    void set_media_player(std::shared_ptr<media_player::MediaPlayer> player) { this->media_player_ = player; }
    void set_speaker(std::shared_ptr<media_player::MediaPlayer> spkr) { this->media_player_ = spkr; } // Map speaker to media_player
    void set_micro_wake_word(std::shared_ptr<micro_wake_word::MicroWakeWord> mww) { this->micro_wake_word_ = mww; }

    Trigger<> *get_listening_trigger() { return this->listening_trigger_; }
    Trigger<> *get_start_trigger() { return this->start_trigger_; }
    Trigger<> *get_wake_word_detected_trigger() { return this->wake_word_detected_trigger_; }
    Trigger<std::string> *get_stt_end_trigger() { return this->stt_end_trigger_; }
    Trigger<std::string> *get_tts_start_trigger() { return this->tts_start_trigger_; }
    Trigger<std::string> *get_tts_end_trigger() { return this->tts_end_trigger_; }
    Trigger<> *get_end_trigger() { return this->end_trigger_; }
    Trigger<std::string, std::string> *get_error_trigger() { return this->error_trigger_; }
    Trigger<> *get_client_connected_trigger() { return this->client_connected_trigger_; }
    Trigger<> *get_client_disconnected_trigger() { return this->client_disconnected_trigger_; }
    Trigger<> *get_intent_start_trigger() { return this->intent_start_trigger_; }
    Trigger<std::string> *get_intent_progress_trigger() { return this->intent_progress_trigger_; }
    Trigger<> *get_intent_end_trigger() { return this->intent_end_trigger_; }
    Trigger<> *get_stt_vad_start_trigger() { return this->stt_vad_start_trigger_; }
    Trigger<> *get_stt_vad_end_trigger() { return this->stt_vad_end_trigger_; }
    Trigger<> *get_tts_stream_start_trigger() { return this->tts_stream_start_trigger_; }
    Trigger<> *get_tts_stream_end_trigger() { return this->tts_stream_end_trigger_; }
    Trigger<> *get_idle_trigger() { return this->idle_trigger_; }
    
    // Timer triggers (stubbed types to allow compilation)
    Trigger<Timer> *get_timer_started_trigger() { return this->timer_started_trigger_; }
    Trigger<Timer> *get_timer_updated_trigger() { return this->timer_updated_trigger_; }
    Trigger<Timer> *get_timer_cancelled_trigger() { return this->timer_cancelled_trigger_; }
    Trigger<Timer> *get_timer_finished_trigger() { return this->timer_finished_trigger_; }
    Trigger<const std::vector<Timer> &> *get_timer_tick_trigger() { return this->timer_tick_trigger_; }

    State get_state() const { return this->state_; }

private:
    void handle_wake_word_detected();
    void handle_audio_delta(const std::vector<uint8_t>& data);
    void handle_text_delta(const std::string& content);
    void handle_error(const std::string& error_msg);
    void transition_to(State new_state);

    bool wake_word_flag_{false};

    std::shared_ptr<media_player::MediaPlayer> media_player_;
    std::shared_ptr<microphone::MicrophoneSource> microphone_source_;
    std::shared_ptr<micro_wake_word::MicroWakeWord> micro_wake_word_;
    std::shared_ptr<text_sensor::TextSensor> text_request_;
    std::shared_ptr<text_sensor::TextSensor> text_response_;

    std::unique_ptr<OpenAIClient> client_;
    State state_;

    std::string api_key_;
    std::string model_;
    std::string instructions_;
    std::string system_prompt_;
    std::string endpoint_;

    Trigger<> *listening_trigger_{new Trigger<>()};
    Trigger<> *start_trigger_{new Trigger<>()};
    Trigger<> *wake_word_detected_trigger_{new Trigger<>()};
    Trigger<std::string> *stt_end_trigger_{new Trigger<std::string>()};
    Trigger<std::string> *tts_start_trigger_{new Trigger<std::string>()};
    Trigger<std::string> *tts_end_trigger_{new Trigger<std::string>()};
    Trigger<> *end_trigger_{new Trigger<>()};
    Trigger<std::string, std::string> *error_trigger_{new Trigger<std::string, std::string>()};
    Trigger<> *client_connected_trigger_{new Trigger<>()};
    Trigger<> *client_disconnected_trigger_{new Trigger<>()};
    Trigger<> *intent_start_trigger_{new Trigger<>()};
    Trigger<std::string> *intent_progress_trigger_{new Trigger<std::string>()};
    Trigger<> *intent_end_trigger_{new Trigger<>()};
    Trigger<> *stt_vad_start_trigger_{new Trigger<>()};
    Trigger<> *stt_vad_end_trigger_{new Trigger<>()};
    Trigger<> *tts_stream_start_trigger_{new Trigger<>()};
    Trigger<> *tts_stream_end_trigger_{new Trigger<>()};
    Trigger<> *idle_trigger_{new Trigger<>()};
    
    Trigger<Timer> *timer_started_trigger_{new Trigger<Timer>()};
    Trigger<Timer> *timer_updated_trigger_{new Trigger<Timer>()};
    Trigger<Timer> *timer_cancelled_trigger_{new Trigger<Timer>()};
    Trigger<Timer> *timer_finished_trigger_{new Trigger<Timer>()};
    Trigger<const std::vector<Timer> &> *timer_tick_trigger_{new Trigger<const std::vector<Timer> &>()};
protected:
    // Ensure you have a flag to track this state
    bool continuous_mode_ = false;
    std::shared_ptr<speaker::Speaker> speaker_;
    std::vector<Timer> timers_;
};

} // namespace openai_assistant
} // namespace esphome

#endif // OPENAI_ASSISTANT_H
