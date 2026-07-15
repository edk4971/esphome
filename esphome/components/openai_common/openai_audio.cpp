#include "openai_audio.h"

#ifdef USE_OPENAI_COMMON

#include "esphome/core/alloc_helpers.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstring>

namespace esphome::openai_common {

static const char *const TAG = "openai_audio";

PsramAudioBuffer::~PsramAudioBuffer() { this->deinit(); }

bool PsramAudioBuffer::init(size_t buffer_size) {
  this->buffer_size_ = buffer_size;

  ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOC_EXTERNAL);
  this->buffer_ = allocator.allocate(buffer_size);
  if (this->buffer_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate %u bytes in PSRAM for audio ring buffer", (unsigned) buffer_size);
    return false;
  }

  this->data_ready_ = xSemaphoreCreateBinary();
  this->space_available_ = xSemaphoreCreateBinary();
  if (this->data_ready_ == nullptr || this->space_available_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create semaphores for audio ring buffer");
    this->deinit();
    return false;
  }

  this->reset();
  ESP_LOGD(TAG, "Audio ring buffer initialized: %u bytes in PSRAM", (unsigned) buffer_size);
  return true;
}

void PsramAudioBuffer::deinit() {
  this->stop_feeder();
  if (this->buffer_ != nullptr) {
    ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOC_EXTERNAL);
    allocator.deallocate(this->buffer_, this->buffer_size_);
    this->buffer_ = nullptr;
  }
  if (this->data_ready_ != nullptr) {
    vSemaphoreDelete(this->data_ready_);
    this->data_ready_ = nullptr;
  }
  if (this->space_available_ != nullptr) {
    vSemaphoreDelete(this->space_available_);
    this->space_available_ = nullptr;
  }
  this->buffer_size_ = 0;
}

void PsramAudioBuffer::reset() {
  this->write_offset_ = 0;
  this->read_offset_ = 0;
  this->producer_done_ = false;
  this->stream_started_ = false;
  this->stream_done_ = false;
  this->should_exit_ = false;
}

// --- Producer API ----------------------------------------------------------

void PsramAudioBuffer::write(const uint8_t *data, size_t len) {
  size_t off = 0;
  while (off < len) {
    if (this->should_exit_) {
      return;
    }
    size_t w = this->write_offset_.load(std::memory_order_acquire);
    size_t r = this->read_offset_.load(std::memory_order_acquire);
    size_t available_space = (r - w - 1 + this->buffer_size_) % this->buffer_size_;
    if (available_space == 0) {
      xSemaphoreTake(this->space_available_, pdMS_TO_TICKS(100));
      continue;
    }
    size_t to_write = (len - off < available_space) ? (len - off) : available_space;
    size_t first_part = (w + to_write > this->buffer_size_) ? (this->buffer_size_ - w) : to_write;
    memcpy(this->buffer_ + w, data + off, first_part);
    if (to_write > first_part) {
      memcpy(this->buffer_, data + off + first_part, to_write - first_part);
    }
    this->write_offset_.store((w + to_write) % this->buffer_size_, std::memory_order_release);
    off += to_write;
    xSemaphoreGive(this->data_ready_);
  }
}

// --- Feeder task -----------------------------------------------------------

void PsramAudioBuffer::start_feeder(speaker::Speaker *speaker, uint32_t sample_rate,
                                    uint8_t bits, uint8_t channels) {
  this->speaker_ = speaker;
  this->sample_rate_ = sample_rate;
  this->bits_ = bits;
  this->channels_ = channels;
  this->should_exit_ = false;
  if (!this->feeder_task_.create(PsramAudioBuffer::feeder_task_fn_, "oai_spk_feed",
                                  FEEDER_STACK_SIZE, this, FEEDER_TASK_PRIORITY, false)) {
    ESP_LOGE(TAG, "Failed to create speaker feeder task");
  }
}

void PsramAudioBuffer::stop_feeder() {
  this->should_exit_ = true;
  // Unblock both the feeder (waiting on data_ready_) and the producer
  // (waiting on space_available_) so they observe should_exit_ promptly.
  if (this->data_ready_ != nullptr) {
    xSemaphoreGive(this->data_ready_);
  }
  if (this->space_available_ != nullptr) {
    xSemaphoreGive(this->space_available_);
  }
  if (this->feeder_task_.is_created()) {
    for (int i = 0; i < 50; i++) {
      if (eTaskGetState(this->feeder_task_.get_handle()) == eSuspended) {
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    this->feeder_task_.destroy();
  }
}

void PsramAudioBuffer::feeder_task_fn_(void *arg) {
  PsramAudioBuffer *self = (PsramAudioBuffer *) arg;
  bool speaker_started = false;
  uint8_t read_buf[FEEDER_CHUNK];

  while (!self->should_exit_) {
    size_t w = self->write_offset_.load(std::memory_order_acquire);
    size_t r = self->read_offset_.load(std::memory_order_acquire);
    size_t available = (w - r + self->buffer_size_) % self->buffer_size_;

    if (available == 0) {
      if (self->producer_done_) {
        break;
      }
      xSemaphoreTake(self->data_ready_, pdMS_TO_TICKS(100));
      continue;
    }

    size_t to_read = (available < FEEDER_CHUNK) ? available : FEEDER_CHUNK;
    size_t first_part = (r + to_read > self->buffer_size_) ? (self->buffer_size_ - r) : to_read;
    memcpy(read_buf, self->buffer_ + r, first_part);
    if (to_read > first_part) {
      memcpy(read_buf + first_part, self->buffer_, to_read - first_part);
    }
    self->read_offset_.store((r + to_read) % self->buffer_size_, std::memory_order_release);

    xSemaphoreGive(self->space_available_);

    if (speaker_started && self->speaker_ != nullptr &&
        self->speaker_->is_stopped()) {
      self->speaker_->start();
    }
    if (!speaker_started) {
      speaker_started = true;
      if (self->speaker_ != nullptr) {
        self->speaker_->set_audio_stream_info(
            audio::AudioStreamInfo(self->bits_, self->channels_, self->sample_rate_));
        self->speaker_->start();
      }
      self->stream_started_ = true;
    }

    if (self->speaker_ != nullptr) {
      size_t off = 0;
      while (off < to_read && !self->should_exit_) {
        size_t written = self->speaker_->play(read_buf + off, to_read - off, 0);
        if (written == 0) {
          if (self->speaker_->is_stopped()) {
            break;
          }
          vTaskDelay(pdMS_TO_TICKS(1));
          continue;
        }
        off += written;
        vTaskDelay(pdMS_TO_TICKS(1));
      }
    }
  }

  if (speaker_started && self->speaker_ != nullptr && !self->should_exit_) {
    self->speaker_->finish();
  }
  if (!self->should_exit_) {
    self->stream_done_ = true;
  }
  ESP_LOGD(TAG, "Speaker feeder task finished");
  vTaskSuspend(nullptr);
}

}  // namespace esphome::openai_common

#endif  // USE_OPENAI_COMMON
