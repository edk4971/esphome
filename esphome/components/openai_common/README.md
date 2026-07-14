# openai_common

Shared infrastructure for the `openai_responses`, `openai_conversations`, and
`openai_realtime` (config key `openai_assistant`) ESPHome voice assistant
components.

This is a **dependency-only component** — it has no `CONFIG_SCHEMA` and is
auto-loaded by the three components that depend on it (like `audio`,
`json`, and `ring_buffer`).

## Classes

### `OpenAIBase`

Base class for all three voice assistant components. Holds the shared
infrastructure with no protocol-specific logic:

- Microphone source, speaker, and `micro_wake_word` wiring + setters
- Text sensors (request/response transcript)
- All automation callbacks (`LazyCallbackManager`, template-based registration)
- Shared config fields (endpoint, API key, chat model, system prompt, VAD
  parameters, volume multiplier)
- 16 KiB speaker buffer with volume application (`feed_speaker_()`,
  `flush_speaker_buffer_()`)
- MWW lifecycle helpers (stop on turn start, restart after turn end)
- `teardown_to_idle_()` (virtual — subclass cleans up protocol-specific
  resources), `fail_()`

### `PsramAudioBuffer`

A 2 MB PSRAM single-producer/single-consumer ring buffer with a dedicated
feeder task for continuous, crackle-free speaker playback.

**Producer** (component-specific task) calls `write()` to push decoded PCM
into the ring buffer. `write()` blocks when the buffer is full (natural
backpressure — the producer pauses until the feeder drains).

**Consumer** (the feeder task, owned by this class) reads PCM from the ring
buffer and calls `speaker_->play()` continuously with non-blocking calls and
1 ms yields so the main loop stays responsive.

The ring buffer decouples bursty audio generation (TTS HTTP, base64 decode)
from the steady playback rate of the I2S speaker. The speaker starts once and
never stops until all audio is drained — no crackle, no underruns.

### `OpenAIHTTPBase`

Base class for the HTTP-based components (`openai_responses` and
`openai_conversations`). Inherits from `OpenAIBase` and adds:

- HTTP task plumbing (FreeRTOS task + message buffer)
- Local VAD (energy-based, ring buffer → contiguous PSRAM recording)
- SSE processing (incremental line parsing)
- Request body builders (multimodal, STT multipart, TTS)
- MCP client tool execution (full state machine)
- Buffer lifecycle management

Protocol-specific behavior is provided via virtual hooks (e.g.
`build_chat_request_body_multimodal_()`, `process_sse_line_()`).

### Unified `mcp_client.h`

A single MCP client header in the `esphome::openai_common` namespace, replacing
the per-component copies that differed only in namespace.

## Usage

Users do not configure this component directly. It is auto-loaded when any of
the three voice assistant components is used:

```yaml
openai_responses:
  # ...
  # openai_common is auto-loaded — no need to list it explicitly
```
