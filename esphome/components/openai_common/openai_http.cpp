#include "openai_http.h"

#ifdef USE_OPENAI_COMMON

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <esp_crt_bundle.h>

#include <cmath>
#include <algorithm>
#include <cstring>

namespace esphome::openai_common {

static const char *const TAG = "openai_http";

// VAD is checked at most once per this interval to bound CPU in loop().
static constexpr uint32_t VAD_CHECK_INTERVAL_MS = 50;
// Speech must be sustained above the threshold for this long before we commit
// to RECORDING (filters out transient clicks).
static constexpr uint32_t SPEECH_ONSET_MS = 60;

// --- dump_config -------------------------------------------------------------

void OpenAIHTTPBase::dump_config() {
  this->OpenAIBase::dump_config();
  ESP_LOGCONFIG(TAG, "  Ring buffer: %u bytes", (unsigned) RING_BUFFER_SIZE);
  ESP_LOGCONFIG(TAG, "  SSE line buffer: %u bytes", (unsigned) SSE_LINE_MAX);
  ESP_LOGCONFIG(TAG, "  HTTP msg buffer: %u bytes", (unsigned) HTTP_MSG_BUFFER_SIZE);
}

// --- Buffer allocation --------------------------------------------------------

bool OpenAIHTTPBase::allocate_buffers_() {
  // All long-lived buffers live in PSRAM (the component requires psram). Each
  // is allocated once and freed in deallocate_buffers_() on teardown.
  ExternalRAMAllocator<uint8_t> ext(ExternalRAMAllocator<uint8_t>::ALLOC_EXTERNAL);

  if (this->ring_buffer_ == nullptr) {
    this->ring_buffer_ = ring_buffer::RingBuffer::create(RING_BUFFER_SIZE);
    if (this->ring_buffer_ == nullptr) {
      ESP_LOGE(TAG, "Failed to allocate ring buffer (%d bytes)", RING_BUFFER_SIZE);
      return false;
    }
  }

  // Recording buffer: worst case max_recording_ms of audio.
  this->recording_capacity_ = this->max_recording_ms_ * MIC_BYTES_PER_MS;
  if (this->recording_buffer_ == nullptr) {
    this->recording_buffer_ = ext.allocate(this->recording_capacity_);
    if (this->recording_buffer_ == nullptr) {
      ESP_LOGE(TAG, "Failed to allocate recording buffer (%d bytes)", (int) this->recording_capacity_);
      return false;
    }
  }
  this->recording_size_ = 0;

  if (this->speaker_buffer_ == nullptr) {
    this->speaker_buffer_ = ext.allocate(SPEAKER_BUFFER_SIZE);
    if (this->speaker_buffer_ == nullptr) {
      ESP_LOGE(TAG, "Failed to allocate speaker buffer (%d bytes)", SPEAKER_BUFFER_SIZE);
      return false;
    }
  }
  this->speaker_buffer_index_ = 0;

  if (this->sse_line_buffer_ == nullptr) {
    this->sse_line_buffer_ = reinterpret_cast<char *>(ext.allocate(SSE_LINE_MAX));
    if (this->sse_line_buffer_ == nullptr) {
      ESP_LOGE(TAG, "Failed to allocate SSE line buffer (%d bytes)", SSE_LINE_MAX);
      return false;
    }
  }
  this->sse_line_len_ = 0;

  return true;
}

void OpenAIHTTPBase::deallocate_buffers_() {
  ExternalRAMAllocator<uint8_t> ext(ExternalRAMAllocator<uint8_t>::ALLOC_EXTERNAL);

  this->ring_buffer_.reset();

  if (this->recording_buffer_ != nullptr) {
    ext.deallocate(this->recording_buffer_, this->recording_capacity_);
    this->recording_buffer_ = nullptr;
  }
  this->recording_capacity_ = 0;
  this->recording_size_ = 0;

  if (this->request_body_ != nullptr) {
    ext.deallocate(this->request_body_, this->request_body_capacity_);
    this->request_body_ = nullptr;
  }
  this->request_body_capacity_ = 0;
  this->request_body_size_ = 0;
  this->request_body_sent_ = 0;

  if (this->sse_line_buffer_ != nullptr) {
    ext.deallocate(reinterpret_cast<uint8_t *>(this->sse_line_buffer_), SSE_LINE_MAX);
    this->sse_line_buffer_ = nullptr;
  }
  this->sse_line_len_ = 0;

  if (this->speaker_buffer_ != nullptr) {
    ext.deallocate(this->speaker_buffer_, SPEAKER_BUFFER_SIZE);
    this->speaker_buffer_ = nullptr;
  }
  this->speaker_buffer_index_ = 0;
}

void OpenAIHTTPBase::reset_turn_state_() {
  // Fixed buffers: reset indices only (do NOT free).
  this->recording_size_ = 0;
  this->sse_line_len_ = 0;
  this->speaker_buffer_index_ = 0;
  this->tts_header_skipped_ = false;

  // Variable-size turn buffers: free (these can be ~1MB+ for multimodal
  // base64 and are rebuilt per turn anyway).
  ExternalRAMAllocator<uint8_t> ext(ExternalRAMAllocator<uint8_t>::ALLOC_EXTERNAL);
  if (this->request_body_ != nullptr) {
    ext.deallocate(this->request_body_, this->request_body_capacity_);
    this->request_body_ = nullptr;
  }
  this->request_body_capacity_ = 0;
  this->request_body_size_ = 0;
  this->request_body_sent_ = 0;

  this->sse_event_type_.clear();

  // Reset VAD state for the next turn.
  this->speech_active_ = false;
  this->speech_ended_ = false;
  this->speech_onset_ms_ = 0;
  this->silence_since_ms_ = 0;
}

// --- VAD / recording ---------------------------------------------------------

float OpenAIHTTPBase::compute_rms_(const int16_t *samples, size_t count) {
  if (count == 0) {
    return 0.0f;
  }
  uint64_t sum_sq = 0;
  for (size_t i = 0; i < count; i++) {
    int32_t s = samples[i];
    sum_sq += (uint64_t) s * (uint64_t) s;
  }
  float rms = sqrtf((float) sum_sq / (float) count) / 32767.0f;
  return rms;
}

void OpenAIHTTPBase::drain_ring_buffer_to_recording_() {
  if (this->ring_buffer_ == nullptr || this->recording_buffer_ == nullptr) {
    return;
  }
  // Drain whatever the mic callback has written, in receive_acquire() chunks,
  // copying into the contiguous recording buffer. One acquired chunk per call.
  size_t length = 0;
  void *item = this->ring_buffer_->receive_acquire(length, HTTP_TASK_READ_CHUNK);
  if (item == nullptr || length == 0) {
    return;
  }
  size_t free = this->recording_capacity_ - this->recording_size_;
  size_t to_copy = (length < free) ? length : free;
  if (to_copy > 0) {
    memcpy(this->recording_buffer_ + this->recording_size_, item, to_copy);
    this->recording_size_ += to_copy;
  }
  this->ring_buffer_->receive_release(item);

  // --- VAD: check RMS at most once per VAD_CHECK_INTERVAL_MS ---
  uint32_t now = millis();
  if (now - this->vad_last_check_ms_ < VAD_CHECK_INTERVAL_MS) {
    return;
  }
  this->vad_last_check_ms_ = now;

  // Compute RMS over the last ~20 ms of audio (320 samples at 16 kHz).
  const size_t samples_for_vad = 320;
  size_t bytes_for_vad = samples_for_vad * sizeof(int16_t);
  if (this->recording_size_ < bytes_for_vad) {
    bytes_for_vad = this->recording_size_;
  }
  if (bytes_for_vad == 0) {
    return;
  }
  const int16_t *vad_samples =
      reinterpret_cast<const int16_t *>(this->recording_buffer_ + this->recording_size_ - bytes_for_vad);
  float rms = this->compute_rms_(vad_samples, bytes_for_vad / sizeof(int16_t));

  ESP_LOGV(TAG, "VAD rms=%.4f threshold=%.4f active=%d (recording=%u bytes)",
           rms, this->silence_threshold_, this->speech_active_ ? 1 : 0,
           (unsigned) this->recording_size_);

  if (rms > this->silence_threshold_) {
    // Speech above threshold.
    if (!this->speech_active_) {
      if (this->speech_onset_ms_ == 0) {
        this->speech_onset_ms_ = now;
      }
      if (now - this->speech_onset_ms_ >= SPEECH_ONSET_MS) {
        this->speech_active_ = true;
        ESP_LOGD(TAG, "Speech detected (rms=%.4f)", rms);
      }
    }
    this->silence_since_ms_ = 0;
  } else {
    // Below threshold.
    if (this->speech_active_) {
      if (this->silence_since_ms_ == 0) {
        this->silence_since_ms_ = now;
      } else if (now - this->silence_since_ms_ >= this->silence_duration_ms_) {
        ESP_LOGD(TAG, "Silence for %" PRIu32 " ms, end of speech", now - this->silence_since_ms_);
        this->speech_ended_ = true;
      }
    }
  }
}

// --- SSE processing ----------------------------------------------------------

void OpenAIHTTPBase::process_sse_bytes_(const uint8_t *data, size_t len) {
  // Append bytes to the line buffer, and whenever a '\n' is found, process the
  // complete line. A blank line separates events.
  for (size_t i = 0; i < len; i++) {
    char c = (char) data[i];
    if (c == '\n') {
      if (this->sse_line_len_ < SSE_LINE_MAX) {
        this->sse_line_buffer_[this->sse_line_len_] = '\0';
      } else {
        this->sse_line_buffer_[SSE_LINE_MAX - 1] = '\0';
      }
      this->process_sse_line_(this->sse_line_buffer_, this->sse_line_len_);
      this->sse_line_len_ = 0;
    } else if (c != '\r') {
      if (this->sse_line_len_ < SSE_LINE_MAX - 1) {
        this->sse_line_buffer_[this->sse_line_len_++] = c;
      } else if (this->sse_line_len_ == SSE_LINE_MAX - 1) {
        ESP_LOGW(TAG, "SSE line exceeded %u bytes; truncated (increase SSE_LINE_MAX)",
                 (unsigned) SSE_LINE_MAX);
        this->sse_line_len_++;  // prevent re-warning; excess is dropped below
      }
    }
  }
}

// --- HTTP task plumbing ------------------------------------------------------

esp_err_t OpenAIHTTPBase::http_event_handler_(esp_http_client_event_t *event) {
  // We drive all reads explicitly via esp_http_client_read() in the task, so
  // the only event we handle here is HTTP_EVENT_ON_HEADER — dispatched to the
  // derived class via on_http_header_().
  if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key != nullptr &&
      event->header_value != nullptr) {
    auto *self = static_cast<OpenAIHTTPBase *>(event->user_data);
    if (self != nullptr) {
      self->on_http_header_(event->header_key, event->header_value);
    }
  }
  return ESP_OK;
}

void OpenAIHTTPBase::start_http_task_() {
  this->http_task_should_exit_ = false;

  // Drain any stale data from a previous turn before starting.
  if (this->http_msg_buffer_ != nullptr) {
    uint8_t drain[64];
    while (xMessageBufferReceive(this->http_msg_buffer_, drain, sizeof(drain), 0) > 0) {
      // discard
    }
  } else {
    this->fail_("msg_buffer_alloc", "Message buffer is null");
    return;
  }

  // Create the HTTP task with an internal-RAM stack. PSRAM stacks crash during
  // interrupt-heavy network I/O.
  if (!this->http_task_.create(OpenAIHTTPBase::http_task_fn_, "oai_http",
                               HTTP_TASK_STACK_SIZE, this, HTTP_TASK_PRIORITY, false)) {
    this->fail_("task_alloc", "Failed to create HTTP task");
    return;
  }
}

bool OpenAIHTTPBase::stop_http_task_() {
  this->http_task_should_exit_ = true;

  if (!this->http_task_.is_created()) {
    this->http_task_should_exit_ = false;
    return true;  // nothing to stop
  }

  // Drain the message buffer so the task's xMessageBufferSend (if blocked on
  // a full buffer) unblocks and can check the exit flag.
  if (this->http_msg_buffer_ != nullptr) {
    uint8_t drain[64];
    while (xMessageBufferReceive(this->http_msg_buffer_, drain, sizeof(drain), 0) > 0) {
      // discard
    }
  }

  // Non-blocking check: is the task suspended?
  if (eTaskGetState(this->http_task_.get_handle()) != eSuspended) {
    return false;  // not yet — retry on the next loop pass
  }

  this->http_task_.destroy();
  this->http_task_should_exit_ = false;
  this->close_http_();

  // Drain any messages the task sent before suspending.
  if (this->http_msg_buffer_ != nullptr) {
    uint8_t drain[64];
    while (xMessageBufferReceive(this->http_msg_buffer_, drain, sizeof(drain), 0) > 0) {
      // discard
    }
  }
  return true;
}

void OpenAIHTTPBase::http_task_fn_(void *arg) {
  // This task runs on its own FreeRTOS task, separate from the main loop.
  // It performs the full HTTP request lifecycle:
  //   1. Build URL + content type (virtual hook)
  //   2. esp_http_client_init
  //   3. esp_http_client_open (DNS + TCP + TLS handshake — blocking)
  //   4. esp_http_client_write loop (sends request body — blocking per chunk)
  //   5. esp_http_client_fetch_headers (waits for response start — blocking)
  //   6. esp_http_client_read loop (reads response chunks — blocking)
  //      For TTS targets: feeds the speaker directly (virtual hook).
  //      For chat/STT: each chunk sent to the main loop via message buffer.
  //   7. On EOF: send DONE. On error: send ERROR. On cancel: just exit.
  //   8. esp_http_client_cleanup
  OpenAIHTTPBase *self = static_cast<OpenAIHTTPBase *>(arg);

  {
  char url[512];
  const char *content_type = "application/json";
  if (!self->build_http_url_and_content_type_(url, sizeof(url), content_type)) {
    // No valid target — shouldn't happen, but handle gracefully.
    goto task_done;
  }

  ESP_LOGV(TAG, "HTTP POST %s (content_length=%u)", url, (unsigned) self->request_body_size_);

  esp_http_client_config_t config = {};
  config.url = url;
  config.cert_pem = nullptr;
  config.disable_auto_redirect = false;
  config.max_redirection_count = 5;
  config.event_handler = OpenAIHTTPBase::http_event_handler_;
  config.user_data = self;
  config.buffer_size = HTTP_TASK_READ_CHUNK * 2;
  config.buffer_size_tx = HTTP_WRITE_CHUNK * 2;
  config.timeout_ms = HTTP_TIMEOUT_MS;

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
  if (strstr(url, "https:") != nullptr) {
    config.crt_bundle_attach = esp_crt_bundle_attach;
  }
#endif

  self->http_client_ = esp_http_client_init(&config);
  if (self->http_client_ == nullptr) {
    ESP_LOGE(TAG, "esp_http_client_init failed");
    self->send_http_error_(self->http_msg_buffer_, "http_init", "esp_http_client_init failed");
    goto task_done;
  }

  esp_http_client_set_method(self->http_client_, HTTP_METHOD_POST);
  esp_http_client_set_header(self->http_client_, "Content-Type", content_type);
  // Let the derived class set request-specific headers (Authorization, etc.).
  self->set_http_extra_headers_(self->http_client_);

  // Open the connection with the exact content length.
  esp_err_t err = esp_http_client_open(self->http_client_, self->request_body_size_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_http_client_open failed: %s", esp_err_to_name(err));
    self->close_http_();
    self->send_http_error_(self->http_msg_buffer_, "http_open", esp_err_to_name(err));
    goto task_done;
  }

  // Write the request body in chunks.
  size_t sent = 0;
  while (sent < self->request_body_size_ && !self->http_task_should_exit_) {
    size_t remaining = self->request_body_size_ - sent;
    size_t to_write = (remaining < HTTP_WRITE_CHUNK) ? remaining : HTTP_WRITE_CHUNK;
    int written = esp_http_client_write(self->http_client_,
                                        reinterpret_cast<const char *>(self->request_body_ + sent), to_write);
    if (written < 0) {
      ESP_LOGE(TAG, "esp_http_client_write failed");
      self->close_http_();
      self->send_http_error_(self->http_msg_buffer_, "http_write", "Failed to write request body");
      goto task_done;
    }
    sent += written;
  }

  if (self->http_task_should_exit_) {
    self->close_http_();
    goto task_done;
  }

  // Fetch response headers (blocking — waits for server to begin responding).
  int64_t header_len = esp_http_client_fetch_headers(self->http_client_);
  if (header_len < 0) {
    ESP_LOGE(TAG, "esp_http_client_fetch_headers failed");
    self->close_http_();
    self->send_http_error_(self->http_msg_buffer_, "http_headers", "Failed to fetch response headers");
    goto task_done;
  }

  // Check the HTTP status code.
  int status = esp_http_client_get_status_code(self->http_client_);
  if (!self->is_http_status_acceptable_(status)) {
    ESP_LOGE(TAG, "HTTP status %d", status);
    self->close_http_();
    char status_msg[32];
    snprintf(status_msg, sizeof(status_msg), "HTTP %d", status);
    self->send_http_error_(self->http_msg_buffer_, "http_status", status_msg);
    goto task_done;
  }

  // Read the response body.
  if (self->http_feeds_speaker_()) {
    // TTS: feed the speaker directly (derived class implements the read loop).
    self->http_feed_speaker_(self->http_client_);
  } else {
    // Chat/STT: send DATA messages via the message buffer.
    uint8_t send_buf[HTTP_TASK_READ_CHUNK + 1];
    send_buf[0] = (uint8_t) HttpMsgType::DATA;

    while (!self->http_task_should_exit_) {
      int got = esp_http_client_read(self->http_client_,
                                     reinterpret_cast<char *>(send_buf + 1), HTTP_TASK_READ_CHUNK);
      if (got < 0) {
        ESP_LOGE(TAG, "esp_http_client_read failed");
        self->close_http_();
        self->send_http_error_(self->http_msg_buffer_, "http_read", "Failed to read response");
        goto task_done;
      }
      if (got == 0) {
        break;  // EOF
      }
      xMessageBufferSend(self->http_msg_buffer_, send_buf, (size_t) got + 1, portMAX_DELAY);
    }
  }

  // Clean up the HTTP client.
  self->close_http_();

  // Send DONE to signal the loop that the response is complete.
  if (!self->http_task_should_exit_) {
    uint8_t done = (uint8_t) HttpMsgType::DONE;
    xMessageBufferSend(self->http_msg_buffer_, &done, 1, portMAX_DELAY);
  }

  UBaseType_t high_water = uxTaskGetStackHighWaterMark(nullptr);
  ESP_LOGV(TAG, "HTTP task stack high-water mark: %u words free", (unsigned) high_water);
  }  // end of scope for all locals

task_done:
  // A FreeRTOS task must NEVER return from its entry function.
  vTaskSuspend(nullptr);
}

void OpenAIHTTPBase::close_http_() {
  if (this->http_client_ != nullptr) {
    esp_http_client_cleanup(this->http_client_);
    this->http_client_ = nullptr;
  }
}

void OpenAIHTTPBase::send_http_error_(MessageBufferHandle_t buf, const char *code, const char *message) {
  uint8_t code_len = (uint8_t) std::min(strlen(code), (size_t) 120);
  uint8_t msg_len = (uint8_t) std::min(strlen(message), (size_t)(sizeof(uint8_t[128]) - 3 - code_len));
  size_t total = 3 + code_len + msg_len;
  uint8_t err_buf[128];
  err_buf[0] = (uint8_t) HttpMsgType::ERROR_;
  err_buf[1] = code_len;
  memcpy(err_buf + 2, code, code_len);
  err_buf[2 + code_len] = msg_len;
  memcpy(err_buf + 3 + code_len, message, msg_len);
  xMessageBufferSend(buf, err_buf, total, portMAX_DELAY);
}

}  // namespace esphome::openai_common

#endif  // USE_OPENAI_COMMON
