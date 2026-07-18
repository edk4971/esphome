#pragma once

#include "esphome/core/defines.h"

#ifdef USE_OPENAI_COMMON

#include "esphome/core/log.h"
#include "esphome/core/static_task.h"

#include "esphome/components/audio/audio.h"
#include "esphome/components/speaker/speaker.h"

#include <atomic>
#include <cstring>

#include <freertos/semphr.h>

namespace esphome::openai_common {

/// 2 MB PSRAM SPSC ring buffer with a dedicated feeder task for continuous,
/// crackle-free speaker playback.
///
/// **Producer** (component-specific task) calls ``write()`` to push decoded
/// PCM into the ring buffer.  ``write()`` blocks when the buffer is full
/// (natural backpressure — the producer pauses until the feeder drains).
///
/// **Consumer** (the feeder task, owned by this class) reads PCM from the
/// ring buffer and calls ``speaker_->play()`` continuously with non-blocking
/// calls and 1 ms yields so the main loop stays responsive.
///
/// The ring buffer decouples bursty audio generation (TTS HTTP, base64
/// decode) from the steady playback rate of the I2S speaker.  The speaker
/// starts once and never stops until all audio is drained — no crackle, no
/// underruns.
///
/// Usage:
/// ```cpp
/// PsramAudioBuffer audio_buf;
/// audio_buf.init();                          // allocate 2 MB in PSRAM
/// audio_buf.start_feeder(speaker, 22050);    // start feeder task
/// // producer task: audio_buf.write(pcm, len);
/// audio_buf.set_producer_done();             // no more data
/// // feeder drains, then sets stream_done flag
/// audio_buf.stop_feeder();                   // cleanup
/// audio_buf.deinit();                        // free PSRAM
/// ```
class PsramAudioBuffer {
 public:
  PsramAudioBuffer() = default;
  ~PsramAudioBuffer();

  /// Allocate the ring buffer in PSRAM + create semaphores.
  /// @param buffer_size  Ring buffer size in bytes (default 2 MB).
  /// @return false on allocation failure.
  bool init(size_t buffer_size = DEFAULT_BUFFER_SIZE);

  /// Free the ring buffer + semaphores.  Safe to call multiple times.
  void deinit();

  /// Reset offsets and flags for reuse across turns (does not free memory).
  void reset();

  // --- Producer API (called from the component's producer task) ---

  /// Write PCM data to the ring buffer.  Blocks if the buffer is full
  /// (backpressure).  Checks ``should_exit_`` between iterations to avoid
  /// deadlock during stop.
  void write(const uint8_t *data, size_t len);

  /// Signal that no more audio data is coming.  The feeder will drain the
  /// remaining buffer, finish the speaker, and set the ``stream_done_`` flag.
  void set_producer_done() {
    this->producer_done_ = true;
    if (this->data_ready_ != nullptr) {
      xSemaphoreGive(this->data_ready_);
    }
  }

  /// Request the feeder and any blocked producer to exit.  Set from the
  /// stop/teardown path.
  void request_exit() { this->should_exit_ = true; }

  // --- Feeder control (called from the main loop) ---

  /// Start the speaker feeder task for continuous playback.
  /// @param speaker      The speaker to feed.
  /// @param sample_rate  Output sample rate (e.g. 22050 for piper, 24000 for
  ///                      Realtime API).
  /// @param bits         Bits per sample (default 16).
  /// @param channels     Number of channels (default 1 = mono).
  void start_feeder(speaker::Speaker *speaker, uint32_t sample_rate,
                    uint8_t bits = 16, uint8_t channels = 1);

  /// Stop the feeder task.  Unblocks the feeder if it is waiting on data.
  void stop_feeder();

  // --- Status flags (poll from main loop) ---

  /// True once the feeder has started playing audio (speaker started).
  bool is_stream_started() const { return this->stream_started_.load(std::memory_order_relaxed); }
  /// Clear the stream-started flag (call after processing it in loop).
  void clear_stream_started() { this->stream_started_ = false; }

  /// True when the feeder has drained all audio and finished the speaker.
  bool is_stream_done() const { return this->stream_done_.load(std::memory_order_relaxed); }
  /// Clear the stream-done flag (call after processing it in loop).
  void clear_stream_done() { this->stream_done_ = false; }

  /// True if the feeder task is currently running.
  bool is_feeder_active() const { return this->feeder_task_.is_created(); }

  /// Default ring buffer size: 2 MB (in PSRAM, allocated once at init).
  static constexpr size_t DEFAULT_BUFFER_SIZE = 2 * 1024 * 1024;

 protected:
  /// Feeder task entry point.  arg is the PsramAudioBuffer * this pointer.
  static void feeder_task_fn_(void *arg);

  // --- Ring buffer (PSRAM, SPSC, lock-free) ---
  uint8_t *buffer_{nullptr};
  size_t buffer_size_{0};
  std::atomic<size_t> write_offset_{0};
  std::atomic<size_t> read_offset_{0};

  // --- Synchronisation ---
  SemaphoreHandle_t data_ready_{nullptr};        // producer → feeder
  SemaphoreHandle_t space_available_{nullptr};    // feeder → producer

  // --- State flags ---
  std::atomic<bool> producer_done_{false};
  std::atomic<bool> stream_started_{false};
  std::atomic<bool> stream_done_{false};
  volatile bool should_exit_{false};

  // --- Feeder task ---
  StaticTask feeder_task_;
  static constexpr uint32_t FEEDER_STACK_SIZE = 4096;
  static constexpr UBaseType_t FEEDER_TASK_PRIORITY = 3;
  static constexpr size_t FEEDER_CHUNK = 2048;

  // --- Speaker info (set by start_feeder) ---
  speaker::Speaker *speaker_{nullptr};
  uint32_t sample_rate_{24000};
  uint8_t bits_{16};
  uint8_t channels_{1};
};

}  // namespace esphome::openai_common

#endif  // USE_OPENAI_COMMON
