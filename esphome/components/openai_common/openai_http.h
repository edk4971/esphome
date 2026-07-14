#pragma once

#include "esphome/core/defines.h"

#ifdef USE_OPENAI_COMMON

#include "openai_common.h"

#include "esphome/components/ring_buffer/ring_buffer.h"

#include <esp_http_client.h>
#include <freertos/message_buffer.h>

#include <memory>
#include <string>
#include <vector>

namespace esphome::openai_common {

/// HTTP + VAD + MCP infrastructure shared by the responses and conversations
/// components. Inherits from OpenAIBase and adds: HTTP task plumbing, local
/// VAD, recording buffer, SSE processing, request body builders, MCP tool
/// execution, prewarm models, and buffer lifecycle management.
///
/// This is a stub that will be filled in during Step 4 of the consolidation
/// plan. For now it just declares the class with the shared enums.
class OpenAIHTTPBase : public OpenAIBase {
 public:
  OpenAIHTTPBase() = default;
  ~OpenAIHTTPBase() override = default;

 protected:
  // Will be populated in Step 4 with HTTP task, VAD, MCP, SSE, etc.
};

}  // namespace esphome::openai_common

#endif  // USE_OPENAI_COMMON
