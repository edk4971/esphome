# openai_conversations

A voice assistant component for ESPHome that talks to an
OpenAI-compatible server over the **Chat Completions** + **Audio** HTTP APIs
(request-based, *not* the Realtime WebSocket API).

Designed for the ESP32-S3-Box-3 (PSRAM, microphone, speaker, and
`micro_wake_word`).

## Why Chat Completions instead of Realtime

The Realtime API requires the server to support `type:"mcp"` tools in the
Realtime session, which many local OpenAI-compatible servers do not. Chat
Completions handles tools client-side — when the LLM returns
`finish_reason: "tool_calls"`, the component executes the tools via MCP
servers, injects the results, and re-queries the LLM.

| Aspect | Realtime API | Chat Completions (this component) |
|--------|-------------|-----------------------------------|
| Connection | Persistent WebSocket | HTTP POST (stateless) |
| Tools | Server must support `type:"mcp"` | Client-side MCP tool execution |
| VAD | Server-side | Client-side (local energy-based) |
| Audio input | Raw PCM16 streamed continuously | Base64-encoded WAV in request body |
| Audio output | Base64 PCM16 deltas via events | Separate `/v1/audio/speech` call |
| Latency | ~1-2s (streaming) | ~3-5s (request/response) |

## Requirements

- ESP32 (ESP-IDF framework only)
- PSRAM **required** (all audio/HTTP buffers live in external RAM)
- `microphone`, `micro_wake_word`, `network`, `psram` components

## Two modes

The component selects a pipeline based on whether `stt_model` is set:

### Mode 1: Multimodal (default, preferred)

Sends the recorded WAV directly to a multimodal chat model via an `audio_url`
data URL. No separate STT step — the LLM transcribes and reasons in one call.

```
User speaks → mic → WAV → POST /v1/chat/completions (audio_url) →
  LLM + tools → text → POST /v1/audio/speech → WAV → speaker
```

### Mode 2: STT + LLM

Runs STT first, then sends the transcript text to the chat model. Use this
for non-multimodal models.

```
User speaks → mic → WAV → POST /v1/audio/transcriptions → text →
  POST /v1/chat/completions (text) → LLM + tools → text →
  POST /v1/audio/speech → WAV → speaker
```

## MCP Tool Execution

When `mcp_servers` is configured, the component executes tool calls
client-side:

1. The LLM's response streams via SSE. If `finish_reason: "tool_calls"` is
   received, the component does **not** TTS the response.
2. The component calls `tools/call` (JSON-RPC) on the appropriate MCP server,
   passing the tool name and arguments from the LLM's response.
3. The tool result is injected as a `role: "tool"` message in the conversation
   history.
4. The LLM is re-queried with the tool results. It may call more tools (up to
   `MAX_TOOL_ROUNDS = 5`) or produce a final text response, which is TTS'd.

### MCP session management

- On first use, each MCP server is queried with `tools/list` to build a
  tool-name → server routing map. This map is cached (see `tools_cache_ttl`).
- Stateless servers (no `Mcp-Session-Id` in the `initialize` response) skip
  the handshake entirely on subsequent calls.
- Stateful servers (e.g. openzim-mcp) use the `initialize` →
  `notifications/initialized` → `tools/list` handshake. If a session expires
  (HTTP 400 on a tool call), the component automatically re-initializes and
  retries.

### Boot pre-warming

Call `prefetch_tools()` and `prewarm_models()` from `wifi.on_connect` to
pre-warm the tools cache and load models into memory before the first
wake-word turn:

```yaml
wifi:
  on_connect:
    - delay: 5s
    - lambda: id(openai).prefetch_tools();
    - lambda: id(openai).prewarm_models();
```

## HA Broadcast (media_player)

To receive Home Assistant broadcasts (via the `HassBroadcast` MCP tool), add a
`speaker` media_player to the YAML. This wraps the i2s speaker as a
`media_player` entity that HA can target for announcements.

```yaml
media_player:
  - platform: speaker
    id: box_media_player
    name: "${friendly_name} Speaker"
    announcement_pipeline:
      speaker: box_speaker       # same speaker used by openai_conversations
      format: wav
      sample_rate: 22050
      num_channels: 1
    volume_initial: 0.65
```

### i2s bus coexistence

The `media_player` announcement pipeline and `openai_conversations` TTS both
use the same i2s speaker. They never overlap because:

1. A **wake-word guard** prevents starting a conversation while the
   media_player is announcing:
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
2. If the media_player starts an announcement while a conversation is in
   progress, the i2s speaker's state machine (`STATE_RUNNING`) prevents the
   announcement pipeline from starting until TTS finishes.

### Broadcast flow

1. User says "broadcast dinner is ready" to one ESP.
2. The LLM calls `HassBroadcast` via MCP.
3. HA generates TTS audio and sends the URL to all `media_player` entities.
4. Each ESP's `media_player` fetches the URL and plays it through the speaker.
5. The originating ESP speaks "Done." (or the LLM's confirmation) via TTS.

If the LLM returns an empty response after a broadcast (no confirmation text),
the component uses "Done." as a default acknowledgment instead of erroring.

## Configuration

```yaml
openai_conversations:
  id: openai
  endpoint_base: "http://my-server:8080"
  api_key: ""  # can be empty if the server doesn't require auth

  # Models
  chat_model: "gemma-4-12b-it-GGUF-mcp"
  # stt_model: "whisper-large-turbo"  # omit for multimodal mode
  tts_model: "voice-en_US-joe-medium"
  tts_voice: "joe"
  tts_sample_rate: 22050  # sample rate of the TTS WAV output

  system_prompt: "You are Jeeves, a voice assistant..."

  # Hardware
  microphone: box_mic
  speaker: box_speaker
  micro_wake_word: mww

  # Audio / VAD settings
  volume_multiplier: 2.5
  silence_threshold: 0.002  # RMS (0..1) below this = silence
  silence_duration_ms: 700ms  # ms of silence before considering speech ended
  max_recording_ms: 30000ms  # safety cap on recording length

  # Tools (optional). A pre-formatted chat-completions tools JSON array.
  # Used as a fallback when MCP servers are unreachable.
  tools_file: tools.json

  # MCP tools cache: how long (ms) the live tools/list cache is valid.
  tools_cache_ttl: 24h

  # MCP servers (for client-side tool execution)
  mcp_servers:
    - name: ha
      url: !secret ha_url
      api_key: !secret ha_token
    - name: openzim
      url: !secret openzim_url
      api_key: !secret openzim_token

  # Text sensors for the transcript / response
  text_request: request_text
  text_response: response_text

  # Automations
  on_listening: ...
  on_start: ...
  on_stt_end: ...
  on_tts_start: ...
  on_tts_end: ...
  on_tts_stream_start: ...
  on_tts_stream_end: ...
  on_end: ...
  on_error: ...
  on_idle: ...
```

## Configuration variables

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
- **volume_multiplier** (*Optional*, float, default `1.0`): Multiplier applied
  to TTS PCM samples before playback.
- **tools_file** (*Optional*, file): Path to a JSON file containing a
  pre-formatted chat-completions `tools` array. The file is embedded in the
  firmware as a generated header. Used as a fallback when MCP servers are
  unreachable or not configured. When omitted and `mcp_servers` is configured,
  the tools list is fetched live from the MCP servers.
- **tools_cache_ttl** (*Optional*, time, default `24h`): How long the live
  MCP tools cache (fetched via `tools/list`) is valid before a refresh is
  needed. The component fetches `tools/list` from each MCP server when the
  cache is stale. Set to `0s` to refresh every turn. Only effective when
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
IDLE → STARTING_MICROPHONE → LISTENING (mic on, VAD running) →
  RECORDING (speech detected) → STOPPING_MICROPHONE →
  BUILDING_REQUEST → SENDING_REQUEST →
    READING_CHAT (multimodal) or READING_STT (Mode 2) →
  [Mode 2: SENDING_REQUEST (chat) → READING_CHAT] →
  [tool_calls: EXECUTING_TOOLS → SENDING_REQUEST (MCP) → READING_MCP →
    EXECUTING_TOOLS → ... → SENDING_REQUEST (chat round 2) → READING_CHAT] →
  SENDING_REQUEST (TTS) → READING_TTS → DRAINING_AUDIO → IDLE
```

When MCP tools are configured and the cache is stale, `FETCHING_TOOLS` is
inserted before `STARTING_MICROPHONE`:

```
IDLE → FETCHING_TOOLS (tools/list on each MCP server) → STARTING_MICROPHONE → ...
```

The component owns the `micro_wake_word` lifecycle: it stops MWW on start
(wake word detection stops the mic; the assistant needs exclusive mic access)
and restarts it after each turn once the speaker has fully stopped (the i2s
bus is shared between mic and speaker).

## Memory

All audio and HTTP buffers live in PSRAM:

- Ring buffer (mic → loop): 16 KB
- Recording buffer: `max_recording_ms * 32` bytes (worst case ~960 KB)
- Request body (JSON + base64, or multipart, or MCP JSON-RPC): sized to payload
- SSE line buffer: 8 KB
- Speaker buffer: 16 KB
- Retained base64 audio (for tool round 2+ multimodal): sized to the b64 payload
- MCP tools cache (`cached_tools_json_`): sized to the tools/list response

All buffers are allocated on entry to the states that need them and freed on
every transition to IDLE (success or error). The tools cache and MCP server
session state persist across turns (refreshed on a TTL basis).

## Notes

- Conversation history is **single-turn**: each request contains only the
  system prompt and the current user utterance. Tool rounds within a turn
  accumulate messages (assistant + tool results) for multi-round tool use,
  but the history is cleared when the turn ends.
- Tools are executed **client-side** via MCP servers. When the LLM returns
  `finish_reason: "tool_calls"`, the component parses `delta.tool_calls`,
  calls the appropriate MCP server's `tools/call`, and re-queries the LLM with
  the results. Up to 5 rounds of tool calls are allowed per turn.
- The `delta.reasoning` field (from reasoning models via llama.cpp/GGUF) is
  accumulated separately and is **never** spoken via TTS. It is only used as a
  fallback response if no `delta.content` was received and `finish_reason` is
  `stop` or null (for servers that route visible output via `reasoning`).
- When the LLM returns an empty response (no content, no reasoning) with
  `finish_reason: "stop"`, the component uses "Done." as a default
  acknowledgment instead of erroring. This handles broadcast/tool scenarios
  where the LLM has nothing to say after executing a tool.
- The TTS WAV header (44 bytes) is skipped once so only raw PCM reaches the
  speaker. Set `tts_sample_rate` to match your TTS server's output.
- All chat requests use `stream: true` (SSE) to reduce latency.
- The `tools_file` JSON is used as a fallback when MCP servers are
  unreachable. When `mcp_servers` is configured, the tools list is fetched
  live from the servers and takes precedence over `tools_file`.
