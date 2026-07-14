# ESPHome Voice Assistant Components

Custom ESPHome components for voice assistants that connect directly to
OpenAI-compatible servers — no Home Assistant voice pipeline required.

## Components

### [openai_conversations](./esphome/components/openai_conversations/) — ✅ Working

A voice assistant using the Chat Completions + Audio HTTP APIs. Supports
multimodal audio, STT+LLM, client-side MCP tool execution, streaming chat,
and TTS playback. Designed for the ESP32-S3-Box-3.

### [openai_responses](./esphome/components/openai_responses/) — ✅ Working

A fork of `openai_conversations` that uses the Responses API
(`/v1/responses`) instead of Chat Completions. The key advantage is
server-side conversation state via `store: true` + `previous_response_id`:
round-2+ requests (after tool execution) resend only the tool results, not
the full tools array (~10KB+) and message history. Same hardware, VAD, MCP,
TTS, and STT features as `openai_conversations`, plus:

- **Streaming TTS** — generates audio per-sentence as text deltas arrive,
  using a 2MB PSRAM ring buffer for crackle-free continuous playback
- **Markdown stripping** — removes markdown from response text before TTS
- **Tool-call text leak suppression** — detects and suppresses leaked tool
  calls from TTS
- **MDI icon display** — vector-drawn Material Design Icons instead of PNGs
  (faster render, saves ~460KB flash)

### [openai_realtime](./esphome/components/openai_realtime/) — ⚠️ Nonfunctional

An incomplete, abandoned attempt using the Realtime WebSocket API.
Development stopped because the Realtime API isn't uniformly supported
across OpenAI-compatible endpoints. Kept in the repo in case someone wants
to continue the effort.
