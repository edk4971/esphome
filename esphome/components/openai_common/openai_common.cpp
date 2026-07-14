#include "openai_common.h"

#ifdef USE_OPENAI_COMMON

#include "esphome/core/log.h"

#ifdef USE_MICRO_WAKE_WORD
#include "esphome/components/micro_wake_word/micro_wake_word.h"
#endif

namespace esphome::openai_common {

static const char *const TAG = "openai_common";

float OpenAIBase::get_setup_priority() const { return setup_priority::AFTER_CONNECTION; }

void OpenAIBase::dump_config() {
  ESP_LOGCONFIG(TAG, "OpenAI Voice Assistant (base):");
  ESP_LOGCONFIG(TAG, "  Endpoint: %s", this->endpoint_base_.c_str());
  ESP_LOGCONFIG(TAG, "  Chat model: %s", this->chat_model_.c_str());
  ESP_LOGCONFIG(TAG, "  Volume multiplier: %.2f", this->volume_multiplier_);
  ESP_LOGCONFIG(TAG, "  Silence threshold: %.4f", this->silence_threshold_);
  ESP_LOGCONFIG(TAG, "  Silence duration: %u ms", (unsigned) this->silence_duration_ms_);
  ESP_LOGCONFIG(TAG, "  Max recording: %u ms", (unsigned) this->max_recording_ms_);
}

void OpenAIBase::stop_mic_on_turn_start_() {
#ifdef USE_MICRO_WAKE_WORD
  if (this->micro_wake_word_ != nullptr) {
    this->micro_wake_word_->stop();
  }
#endif
}

void OpenAIBase::restart_mic_after_turn_end_() {
#ifdef USE_MICRO_WAKE_WORD
  if (this->micro_wake_word_ != nullptr) {
    this->micro_wake_word_->start();
  }
#endif
}

void OpenAIBase::feed_speaker_(const uint8_t *data, size_t len) {
  if (this->speaker_ == nullptr || this->speaker_buffer_ == nullptr) {
    return;
  }
  size_t offset = 0;
  while (offset < len) {
    size_t free_space = SPEAKER_BUFFER_SIZE - this->speaker_buffer_index_;
    size_t to_copy = (len - offset < free_space) ? (len - offset) : free_space;
    memcpy(this->speaker_buffer_ + this->speaker_buffer_index_, data + offset, to_copy);
    this->speaker_buffer_index_ += to_copy;
    offset += to_copy;

    if (this->volume_multiplier_ != 1.0f && to_copy > 0) {
      size_t start_sample_index = (this->speaker_buffer_index_ - to_copy) / sizeof(int16_t);
      size_t num_samples = to_copy / sizeof(int16_t);
      int16_t *samples = reinterpret_cast<int16_t *>(this->speaker_buffer_);
      for (size_t i = 0; i < num_samples; i++) {
        float scaled = (float) samples[start_sample_index + i] * this->volume_multiplier_;
        if (scaled > 32767.0f) {
          scaled = 32767.0f;
        } else if (scaled < -32768.0f) {
          scaled = -32768.0f;
        }
        samples[start_sample_index + i] = (int16_t) scaled;
      }
    }

    if (this->speaker_buffer_index_ >= SPEAKER_BUFFER_SIZE) {
      this->flush_speaker_buffer_();
    }
  }
}

void OpenAIBase::flush_speaker_buffer_() {
  if (this->speaker_ == nullptr || this->speaker_buffer_ == nullptr || this->speaker_buffer_index_ == 0) {
    return;
  }
  if (this->speaker_->is_stopped()) {
    this->speaker_buffer_index_ = 0;
    return;
  }
  size_t written = this->speaker_->play(this->speaker_buffer_, this->speaker_buffer_index_);
  if (written > 0 && written < this->speaker_buffer_index_) {
    memmove(this->speaker_buffer_, this->speaker_buffer_ + written, this->speaker_buffer_index_ - written);
  }
  this->speaker_buffer_index_ -= (written <= this->speaker_buffer_index_) ? written : this->speaker_buffer_index_;
}

void OpenAIBase::fail_(const std::string &code, const std::string &message) {
  ESP_LOGE(TAG, "Error: %s — %s", code.c_str(), message.c_str());
  this->on_error_cb_.call(code, message);
  this->teardown_to_idle_();
}

void OpenAIBase::teardown_to_idle_() {
  // Base implementation: reset per-turn indices for the shared members owned by
  // OpenAIBase. The actual allocation/deallocation lifecycle of speaker_buffer_
  // is managed by the derived class (which allocates it once in setup and frees
  // it on destruction), so we only reset the index here — we never free it.
  this->speaker_buffer_index_ = 0;
  this->tts_header_skipped_ = false;
}

}  // namespace esphome::openai_common

#endif  // USE_OPENAI_COMMON
