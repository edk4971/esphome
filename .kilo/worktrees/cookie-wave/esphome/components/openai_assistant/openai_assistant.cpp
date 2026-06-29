#include "openai_assistant.h"
#include "openai_client.h"
#include <iostream>
#include "esphome/components/network/network_component.h"

namespace esphome {
namespace openai_assistant {

OpenAIAssistant::OpenAIAssistant(std::shared_ptr<media_player::MediaPlayer> media_player,
                                  std::shared_ptr<microphone::MicrophoneSource> microphone_source,
                                  std::shared_ptr<micro_wake_word::MicroWakeWord> micro_wake_word,
                                  std::shared_ptr<text_sensor::TextSensor> text_request,
                                  std::shared_ptr<text_sensor::TextSensor> text_response)
    : media_player_(media_player),
      microphone_source_(microphone_source),
      micro_wake_word_(micro_wake_word),
      text_request_(text_request),
      text_response_(text_response),
      state_(State::IDLE),
      client_(nullptr) {}

void OpenAIAssistant::setup() {
    this->client_ = std::make_unique<OpenAIClient>();
    
    this->client_->on_audio_delta = [this](const std::vector<uint8_t>& data) {
        this->handle_audio_delta(data);
    };
    
    this->client_->on_text_delta = [this](const std::string& content) {
        this->handle_text_delta(content);
    };
    
    this->client_->on_error = [this](const std::string& error_msg) {
        this->handle_error(error_msg);
    };
    
    this->micro_wake_word_->get_wake_word_detected_trigger()->add_action(
        [this](std::string wake_word) {
            this->wake_word_flag_ = true;
        }
    );
}

void OpenAIAssistant::loop() {
    switch (this->state_) {
        case State::IDLE:
            if (this->wake_word_flag_) {
                this->wake_word_flag_ = false; // Reset flag
                this->handle_wake_word_detected();
            }
            break;
        case State::LISTENING:
            this->client_->receive_frame();
            break;
        case State::SPEAKING:
            this->client_->receive_frame();
            break;
        case State::ERROR:
            break;
    }
}

void OpenAIAssistant::set_api_key(const std::string& api_key) {
    this->api_key_ = api_key;
}

void OpenAIAssistant::set_model(const std::string& model) {
    this->model_ = model;
}

void OpenAIAssistant::set_instructions(const std::string& instructions) {
    this->instructions_ = instructions;
}

void OpenAIAssistant::set_system_prompt(const std::string& prompt) {
    this->system_prompt_ = prompt;
}

void OpenAIAssistant::set_endpoint(const std::string& endpoint) {
    this->endpoint_ = endpoint;
}

void OpenAIAssistant::handle_wake_word_detected() {
    // Reset continuous mode when a physical wake word is used
    this->continuous_mode_ = false; 
    
    this->wake_word_detected_trigger_->trigger(); // Fire trigger
    this->transition_to(State::LISTENING);
    
    if (this->client_->connect(this->api_key_, this->model_, this->endpoint_)) {
        if (!this->instructions_.empty()) {
            this->client_->send_text_event("session.update", "{\"instructions\":\"" + this->instructions_ + "\"}");
        }
        if (!this->system_prompt_.empty()) {
            this->client_->send_text_event("session.update", "{\"system_prompt\":\"" + this->system_prompt_ + "\"}");
        }
    } else {
        this->handle_error("Failed to connect to OpenAI");
    }
}

void OpenAIAssistant::handle_audio_delta(const std::vector<uint8_t>& data) {
    if (this->state_ == State::SPEAKING && this->speaker_ != nullptr) {
        this->speaker_->play(data.data(), data.size());
    }
}

void OpenAIAssistant::handle_text_delta(const std::string& content) {
    if (content.find("transcription") != std::string::npos) {
        this->text_request_->publish_state(content);
    } else if (content.find("response") != std::string::npos || content.find("text") != std::string::npos) {
        if (this->state_ != State::SPEAKING) {
            this->transition_to(State::SPEAKING);
        }
        
        this->text_response_->publish_state(content);
        
        if (content.find("done") != std::string::npos) {
            this->text_response_->publish_state("");
            
            // Check if we should return to LISTENING (continuous) or go to IDLE
            if (this->continuous_mode_) {
                ESP_LOGD("openai_assistant", "Continuous mode: Returning to LISTENING");
                this->transition_to(State::LISTENING);
            } else {
                this->continuous_mode_ = false;
                this->transition_to(State::IDLE);
            }
        }
    }
}

void OpenAIAssistant::handle_error(const std::string& error_msg) {
    this->state_ = State::ERROR;
    // Log error or notify user
}

void OpenAIAssistant::transition_to(State new_state) {
    this->state_ = new_state;
    
    // Fire the corresponding ESPHome automation triggers so the UI updates
    if (new_state == State::IDLE) {
        this->idle_trigger_->trigger();
        this->end_trigger_->trigger();
    } else if (new_state == State::LISTENING) {
        this->listening_trigger_->trigger();
    } else if (new_state == State::SPEAKING) {
        // In a real implementation, you'd extract the response text to pass here
        this->tts_start_trigger_->trigger("Speaking response..."); 
    }
}

void OpenAIAssistant::handle_start_continuous() {
    // 1. Enable the continuous mode flag so the state machine 
    // knows to loop back after speaking.
    this->continuous_mode_ = true;
    
    // 2. Trigger the standard start logic to begin the session.
    this->handle_wake_word_detected();
    
    ESP_LOGD("openai_assistant", "Continuous mode enabled.");
}

void OpenAIAssistant::on_timer_event(const api::OpenAIAssistantTimerEventResponse &msg) {
  // Find existing timer or add a new one
  auto it = this->timers_.begin();
  for (; it != this->timers_.end(); ++it) {
    if (it->id == msg.timer_id)
      break;
  }
  if (it == this->timers_.end()) {
    this->timers_.push_back({});
    it = this->timers_.end() - 1;
  }
  it->id = msg.timer_id;
  it->name = msg.name;
  it->total_seconds = msg.total_seconds;
  it->seconds_left = msg.seconds_left;
  it->is_active = msg.is_active;

  char timer_buf[Timer::TO_STR_BUFFER_SIZE];
  ESP_LOGD(TAG,
           "Timer Event\n"
           "  Type: %" PRId32 "\n"
           "  %s",
           msg.event_type, it->to_str(timer_buf));

  switch (msg.event_type) {
    case api::enums::OPENAI_ASSISTANT_TIMER_STARTED:
      this->timer_started_trigger_.trigger(*it);
      break;
    case api::enums::OPENAI_ASSISTANT_TIMER_UPDATED:
      this->timer_updated_trigger_.trigger(*it);
      break;
    case api::enums::OPENAI_ASSISTANT_TIMER_CANCELLED:
      this->timer_cancelled_trigger_.trigger(*it);
      this->timers_.erase(it);
      break;
    case api::enums::OPENAI_ASSISTANT_TIMER_FINISHED:
      this->timer_finished_trigger_.trigger(*it);
      this->timers_.erase(it);
      break;
  }

  if (this->timers_.empty()) {
    this->cancel_interval("timer-event");
    this->timer_tick_running_ = false;
  } else if (!this->timer_tick_running_) {
    this->set_interval("timer-event", 1000, [this]() { this->timer_tick_(); });
    this->timer_tick_running_ = true;
  }
}

void OpenAIAssistant::timer_tick_() {
  for (auto &timer : this->timers_) {
    if (timer.is_active && timer.seconds_left > 0) {
      timer.seconds_left--;
    }
  }
  this->timer_tick_trigger_.trigger(this->timers_);
}

void OpenAIAssistant::on_wake_word_triggered(std::string wake_word) {
    // 1. Handle the wake word string if needed (e.g., logging)
    ESP_LOGD("openai_assistant", "Wake word detected: %s", wake_word.c_str());

    // 2. Call your existing connection logic
    this->handle_wake_word_detected();
}

} // namespace openai_assistant
} // namespace esphome
