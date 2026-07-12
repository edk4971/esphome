# ESPHome Voice Assistant Components

Custom ESPHome components for voice assistants that connect directly to
OpenAI-compatible servers — no Home Assistant voice pipeline required.

## Components

### [openai_conversations](./esphome/components/openai_conversations/) — ✅ Working

A voice assistant using the Chat Completions + Audio HTTP APIs. Supports
multimodal audio, STT+LLM, client-side MCP tool execution, streaming chat,
and TTS playback. Designed for the ESP32-S3-Box-3.

### [openai_realtime](./esphome/components/openai_realtime/) — ⚠️ Nonfunctional

An incomplete, abandoned attempt using the Realtime WebSocket API.
Development stopped because the Realtime API isn't uniformly supported
across OpenAI-compatible endpoints. Kept in the repo in case someone wants
to continue the effort.
