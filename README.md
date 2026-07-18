# ESPHome Voice Assistant Components

Custom ESPHome components for voice assistants that connect directly to
OpenAI-compatible servers — no Home Assistant voice pipeline required.

## Components

All three components share infrastructure via
[`openai_common`](./esphome/components/openai_common/), which provides the
`OpenAIBase`/`OpenAIHTTPBase` base classes, `PsramAudioBuffer` (2MB PSRAM ring
buffer + feeder task), and a unified MCP client. A shared S3-Box-3 hardware
config lives in
[`common.yaml`](./esphome/components/openai_common/common.yaml) — switch
between components by changing one `!include` line.

### [openai_responses](./esphome/components/openai_responses/) — ✅ Working

Uses the Responses API (`/v1/responses`) with server-side conversation state
via `store: true` + `previous_response_id`. The most feature-rich component:

- Streaming TTS, markdown stripping, tool-call text leak suppression
- Client-side MCP tool execution (up to 5 rounds per turn)
- Local VAD, multimodal or STT+LLM audio modes
- MDI vector icon display pages

### [openai_conversations](./esphome/components/openai_conversations/) — ✅ Working

Uses the Chat Completions API (`/v1/chat/completions`). Same hardware, VAD,
MCP, TTS, and STT features as `openai_responses`, but without streaming TTS
or server-side conversation state.

### [openai_realtime](./esphome/components/openai_realtime/) — ⚠️ Not working

Uses the Realtime API WebSocket endpoint for low-latency voice conversations.
Server-side VAD, server-side tool execution, and base64 audio delta playback
via the shared `PsramAudioBuffer` for crackle-free continuous audio. Accepts
`ws://`, `wss://`, `http://`, or `https://` endpoints (auto-converted).

See [`todo.md`](./esphome/components/openai_realtime/todo.md) for known issues.
