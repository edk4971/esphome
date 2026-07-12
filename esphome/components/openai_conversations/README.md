# openai_conversations

A voice assistant component for ESPHome that connects to any
OpenAI-compatible server (LLM, STT, TTS) over the standard **Chat
Completions** and **Audio** HTTP APIs. Designed for the ESP32-S3-Box-3 with
full support for `micro_wake_word`, PSRAM, and client-side MCP tool
execution.

Inspired by ESPHome's built-in `voice_assistant`, but written to be
backend-agnostic: it talks to any OpenAI-compatible endpoint and interfaces
with Home Assistant primarily through MCP tool servers rather than the HA
WebSocket API. It can even be used without Home Assistant entirely — any
MCP-compatible tool server works.

## Features

- **Wake-word activated** — integrates with `micro_wake_word`; the component
  owns the MWW lifecycle (stop on start, restart after each turn)
- **Two audio pipelines** — multimodal (send audio directly to the LLM) or
  STT + LLM (transcribe first, then chat)
- **Client-side MCP tools** — when the LLM requests tool calls, the component
  executes them via MCP servers and re-queries the LLM with the results
  (up to 5 rounds per turn)
- **Streaming chat** — SSE delta streaming for lower latency
- **TTS playback** — fetches audio from `/v1/audio/speech` and plays it
  through the i2s speaker
- **Home Assistant integration** — text sensors for transcript/response,
  HA-exposed mute switch, media_player announcement pipeline, and the
  `HassBroadcast` MCP tool
- **PSRAM-optimized** — all audio buffers, HTTP request bodies, and JSON
  documents live in PSRAM to preserve internal heap
- **Local VAD** — energy-based voice activity detection with configurable
  threshold, onset, and silence duration

## Installation

This is an external ESPHome component. Add it to your project via the
`external_components` source:

```yaml
external_components:
  - source:
      type: git
      ref: main
      url: https://github.com/edk4971/esphome
    components: [ openai_conversations ]
```

If you already have an `external_components` block, just add the
`openai_conversations` entry to it.

## Requirements

- **ESP32** with **PSRAM** (ESP-IDF framework only; all buffers use external
  RAM). Tested on the ESP32-S3-Box-3.
- An **OpenAI-compatible server** reachable over HTTP(S) that supports:
  - `POST /v1/chat/completions` (with `stream: true` / SSE)
  - `POST /v1/audio/speech` (TTS)
  - `POST /v1/audio/transcriptions` (STT — only if using Mode 2)
- ESPHome components: `microphone`, `speaker`, `micro_wake_word`, `psram`,
  `network`

> **Tested with:** ESP32-S3-Box-3 hardware and [LocalAI](https://localai.io/)
> as the backend server. Other ESP32 variants with PSRAM and other
> OpenAI-compatible servers (e.g. llama.cpp, Ollama, vLLM) should work but
> are untested.

## Quick start

A complete example config for the ESP32-S3-Box-3 is in
[`esp32-openai.yaml`](./esp32-openai.yaml). The minimal component block:

```yaml
openai_conversations:
  id: openai
  endpoint_base: "http://my-server:8080"
  api_key: ""                      # empty if server doesn't require auth

  # Models
  chat_model: "gemma-4-12b-it-GGUF-mcp"
  # stt_model: "whisper-large-turbo"  # omit for multimodal mode (default)
  tts_model: "voice-en-US-joe-medium"
  tts_voice: "joe"
  tts_sample_rate: 22050

  system_prompt: "You are a helpful voice assistant."

  # Hardware
  microphone: box_mic
  speaker: box_speaker
  micro_wake_word: mww

  # Audio / VAD
  volume_multiplier: 2.5
  silence_threshold: 0.002
  silence_duration_ms: 700ms
  max_recording_ms: 30000ms

  # Text sensors (visible in Home Assistant)
  text_request: request_text
  text_response: response_text
```

Wire the wake word to start a conversation:

```yaml
micro_wake_word:
  # ... microphone, models ...
  on_wake_word_detected:
    - openai_conversations.start:
        id: openai
```

## How it works

A conversation turn flows through these stages:

```
Wake word → stop MWW → start mic → listen (VAD) → record speech →
  stop mic → build request → send to server →
  [STT (Mode 2 only) →] chat completions (streamed) →
  [tool calls → MCP servers → re-query LLM →] →
  TTS → play audio → restart MWW → idle
```

The component owns the `micro_wake_word` lifecycle: it stops MWW when a turn
starts (to get exclusive mic access) and restarts it after the speaker stops
(the i2s bus is shared between mic and speaker on the S3-Box-3).

### Two audio modes

**Mode 1: Multimodal** (default — omit `stt_model`)

Sends the recorded WAV directly to a multimodal chat model via an `audio_url`
data URL. The LLM transcribes and reasons in a single call — no separate STT
step, lower latency.

```
User speaks → mic → WAV → POST /v1/chat/completions (audio_url) →
  LLM + tools → text → POST /v1/audio/speech → WAV → speaker
```

**Mode 2: STT + LLM** (set `stt_model`)

Transcribes first, then sends the text transcript to the chat model. Use this
for non-multimodal models or when you want a separate STT stage.

```
User speaks → mic → WAV → POST /v1/audio/transcriptions → text →
  POST /v1/chat/completions (text) → LLM + tools → text →
  POST /v1/audio/speech → WAV → speaker
```

### MCP tool execution

When `mcp_servers` is configured, the component handles tool calls
client-side — no server-side MCP support required:

1. The chat response streams via SSE. If `finish_reason: "tool_calls"` is
   received, the component does **not** TTS the response.
2. The component calls `tools/call` (JSON-RPC) on the appropriate MCP server,
   passing the tool name and arguments from the LLM's response.
3. The tool result is injected as a `role: "tool"` message in the
   conversation history.
4. The LLM is re-queried with the tool results. It may call more tools (up to
   5 rounds) or produce a final text response, which is TTS'd.

**Session management:**
- On first use, each MCP server is queried with `tools/list` to build a
  tool-name → server routing map (cached via `tools_cache_ttl`).
- Stateless servers skip the handshake on subsequent calls.
- Stateful servers use the full `initialize` → `notifications/initialized`
  → `tools/list` handshake. If a session expires (HTTP 400), the component
  automatically re-initializes and retries.

**Boot pre-warming** — call from `wifi.on_connect` to warm the tools cache
and load models before the first wake-word turn:

```yaml
wifi:
  on_connect:
    - delay: 5s
    - lambda: id(openai).prefetch_tools();
    - lambda: id(openai).prewarm_models();
```

### Home Assistant broadcast

Broadcasts involve two separate mechanisms:

1. **Initiating** — when a user says "broadcast dinner is ready", the LLM
   calls the `HassBroadcast` MCP tool, which tells HA to send a TTS
   announcement to all `media_player` entities.
2. **Receiving** — HA sends the announcement to each ESP's `media_player`
   via the ESPHome **API** (not MCP). The `media_player` announcement
   pipeline plays it through the speaker.

To support broadcasts, add a `speaker` media_player that wraps the i2s
speaker. The media_player and the component share the same speaker — the
component stops MWW when an announcement starts (to free the i2s bus) and
restarts it when the announcement ends:

```yaml
media_player:
  - platform: speaker
    id: box_media_player
    name: "${friendly_name} Speaker"
    announcement_pipeline:
      speaker: box_speaker
      format: wav
      sample_rate: 22050
      num_channels: 1
    volume_initial: 0.65
    on_announcement:
      - micro_wake_word.stop:
          id: mww
    on_idle:
      - if:
          condition:
            lambda: 'return !id(openai).is_running();'
          then:
            - micro_wake_word.start:
                id: mww
```

A wake-word guard prevents starting a conversation during an announcement:

```yaml
micro_wake_word:
  on_wake_word_detected:
    - if:
        condition:
          lambda: |-
            return id(box_media_player).state !=
                media_player::MediaPlayerState::MEDIA_PLAYER_STATE_ANNOUNCING;
        then:
          - openai_conversations.start:
              wake_word: !lambda return wake_word;
```

## Configuration

### Core settings

- **endpoint_base** (*Required*, string): Base URL of the OpenAI-compatible
  server, e.g. `http://host:8080`. Must start with `http://` or `https://`.
  The component appends `/v1/chat/completions`, `/v1/audio/transcriptions`,
  `/v1/audio/speech`, and `/backend/load`.
- **api_key** (*Optional*, string, default `""`): API key. Sent as
  `Authorization: Bearer <key>`. Leave empty if the server doesn't require
  auth.
- **chat_model** (*Required*, string): Chat model name.
- **stt_model** (*Optional*, string): Transcription model for Mode 2. Omit for
  multimodal mode (Mode 1).
- **tts_model** (*Required*, string): TTS model name.
- **tts_voice** (*Optional*, string, default `""`): TTS voice name.
- **tts_sample_rate** (*Optional*, int, default `24000`): Sample rate of the
  TTS WAV output. The speaker's `AudioStreamInfo` is set to this rate before
  playback so the i2s clock matches. Set to match your TTS server's output
  (e.g. 22050 for piper).
- **system_prompt** (*Optional*, string, default `"You are a helpful voice
  assistant."`): System prompt sent with each chat request.
- **microphone** (*Required*): A microphone source. Must provide 16-bit mono
  at 16 kHz.
- **speaker** (*Required*): A speaker.
- **micro_wake_word** (*Required*): A `micro_wake_word` instance. The component
  stops it on start and restarts it after each turn (once the speaker stops,
  to avoid i2s bus contention).

### Audio / VAD

- **volume_multiplier** (*Optional*, float, default `1.0`): Multiplier applied
  to TTS PCM samples before playback.
- **silence_threshold** (*Optional*, float, default `0.01`): RMS amplitude
  (0.0–1.0, relative to full-scale int16) below which audio is considered
  silence. On the S3-Box-3 with the ES7210 at default gain, idle noise reads
  ~0.0001–0.0005 and speech reads ~0.004–0.007, so `0.002` sits cleanly
  between them. Tune per hardware.
- **silence_duration_ms** (*Optional*, time, default `700ms`): Duration of
  silence (after speech has started) before end-of-speech is detected.
- **max_recording_ms** (*Optional*, time, default `30000ms`): Maximum
  recording length. If no speech is detected within this time in LISTENING,
  the component returns to idle silently (treated as a user cancel, not an
  error).

### MCP tools

- **tools_file** (*Optional*, file): Path to a JSON file containing a
  pre-formatted chat-completions `tools` array. The file is embedded in the
  firmware as a generated header. Used as a fallback when MCP servers are
  unreachable or not configured. When omitted and `mcp_servers` is configured,
  the tools list is fetched live from the MCP servers.
- **tools_cache_ttl** (*Optional*, time, default `24h`): How long the live
  MCP tools cache (fetched via `tools/list`) is valid before a refresh is
  needed. Set to `0s` to refresh every turn. Only effective when
  `mcp_servers` is configured.
- **mcp_servers** (*Optional*, list): MCP servers for client-side tool
  execution. When the LLM returns `finish_reason: "tool_calls"`, the component
  calls `tools/call` on the appropriate server, injects the results, and
  re-queries the LLM.
  - **name** (*Required*, string): Server identifier (for logging).
  - **url** (*Required*, string): Full URL of the MCP server endpoint (must
    start with `http://` or `https://`).
  - **api_key** (*Optional*, string, default `""`): Bearer token sent as
    `Authorization: Bearer <key>`. Leave empty if the server doesn't require
    auth.

### Sensors

- **text_request** (*Optional*): A text sensor to publish the transcribed user
  request (Mode 2 only; in multimodal mode there is no transcript).
- **text_response** (*Optional*): A text sensor to publish the chat response
  text (streamed as deltas arrive).

### Automations

All triggers are single-automation. They use the callback-manager pattern
(no `Trigger` object overhead when unused).

- **on_start**: Fired when a conversation turn starts (after wake word).
- **on_listening**: Fired when the microphone starts and VAD begins.
- **on_wake_word_detected**: Fired if a `wake_word` was supplied to the
  `start` action.
- **on_stt_end** (`x` = transcript string): Fired when STT completes (Mode 2).
- **on_tts_start** (`x` = response text): Fired before the TTS request is
  sent.
- **on_tts_stream_start**: Fired when the TTS audio stream begins playing.
- **on_tts_stream_end**: Fired when the TTS audio stream finishes.
- **on_tts_end** (`x` = response text): Fired after TTS playback completes.
- **on_end**: Fired at the end of a turn (after teardown).
- **on_error** (`code`, `message` strings): Fired on any error.
- **on_idle**: Fired when the component returns to idle and MWW restarts.

### Actions

- **openai_conversations.start**: Start a conversation turn. Optional
  `silence_detection` (bool, default true) and `wake_word` (templatable
  string).
- **openai_conversations.stop**: Stop the current turn and tear down.

### Conditions

- **openai_conversations.is_running**: True when the component is not idle.

### Public methods (for lambdas)

- **prefetch_tools()**: Pre-fetch `tools/list` from all configured MCP servers.
  Call from `wifi.on_connect` to warm the tools cache before the first
  wake-word turn. No-op if already cached or not idle.
- **prewarm_models()**: POST to `/backend/load` for each configured model
  (chat, STT, TTS). Spawns a background FreeRTOS task so the main loop is not
  blocked. Call from `wifi.on_connect`.

## State machine

```
IDLE → STARTING_TURN → STARTING_MICROPHONE → LISTENING (mic on, VAD) →
  RECORDING (speech detected) → STOPPING_MICROPHONE →
  BUILDING_REQUEST → SENDING_REQUEST →
    READING_CHAT (multimodal) or READING_STT (Mode 2) →
  [Mode 2: SENDING_REQUEST (chat) → READING_CHAT] →
  [tool_calls: EXECUTING_TOOLS → SENDING_REQUEST (MCP) → READING_MCP →
    EXECUTING_TOOLS → ... → SENDING_REQUEST (chat round 2) → READING_CHAT] →
  SENDING_REQUEST (TTS) → READING_TTS → DRAINING_AUDIO → IDLE
```

When MCP tools are configured and the cache is stale, `FETCHING_TOOLS` is
inserted before `STARTING_TURN`:

```
IDLE → FETCHING_TOOLS (tools/list on each MCP server) → STARTING_TURN → ...
```

## Memory

All audio and HTTP buffers live in PSRAM:

- Ring buffer (mic → loop): 16 KB
- Recording buffer: `max_recording_ms * 32` bytes (worst case ~960 KB)
- Request body (JSON + base64, or multipart, or MCP JSON-RPC): sized to payload
- SSE line buffer: 8 KB
- Speaker buffer: 16 KB
- Retained base64 audio (for tool round 2+ multimodal): sized to the b64 payload
- MCP tools cache (`cached_tools_json_`): sized to the tools/list response

Fixed buffers are allocated once at setup and reused across turns. Variable-size
buffers (request body, retained audio) are freed per turn. The tools cache and
MCP server session state persist across turns (refreshed on a TTL basis).

## Notes

- Conversation history is **single-turn**: each request contains only the
  system prompt and the current user utterance. Tool rounds within a turn
  accumulate messages (assistant + tool results) for multi-round tool use,
  but the history is cleared when the turn ends.
- The `delta.reasoning` field (from reasoning models via llama.cpp/GGUF) is
  accumulated separately and is **never** spoken via TTS. It is only used as a
  fallback response if no `delta.content` was received and `finish_reason` is
  `stop` or null.
- When the LLM returns an empty response (no content, no reasoning) with
  `finish_reason: "stop"`, the component uses "Done." as a default
  acknowledgment instead of erroring. This handles broadcast/tool scenarios
  where the LLM has nothing to say after executing a tool.
- The TTS WAV header (44 bytes) is skipped once so only raw PCM reaches the
  speaker. Set `tts_sample_rate` to match your TTS server's output.
- All chat requests use `stream: true` (SSE) to reduce latency.
- The `tools_file` JSON is used as a fallback when MCP servers are
  unreachable. When `mcp_servers` is configured, the live tools list takes
  precedence.

## Chat Completions vs Realtime API

This component uses the Chat Completions + Audio HTTP APIs (request-based),
not the Realtime WebSocket API. The Realtime API requires server-side
`type:"mcp"` tool support, which many local OpenAI-compatible servers lack.
Chat Completions handles tools client-side, making it compatible with any
OpenAI-compatible endpoint.

| Aspect | Realtime API | Chat Completions (this component) |
|--------|-------------|-----------------------------------|
| Connection | Persistent WebSocket | HTTP POST (stateless) |
| Tools | Server must support `type:"mcp"` | Client-side MCP tool execution |
| VAD | Server-side | Client-side (local energy-based) |
| Audio input | Raw PCM16 streamed continuously | Base64-encoded WAV in request body |
| Audio output | Base64 PCM16 deltas via events | Separate `/v1/audio/speech` call |
| Latency | ~1-2s (streaming) | ~3-5s (request/response) |
