# Prompt: Re-implement the ESPHome `openai_realtime` component from scratch

## 1. Project goal

Create an ESPHome external component named `openai_realtime` that lets an ESP32 device talk **directly** to an OpenAI-compatible Realtime API WebSocket endpoint, bypassing Home Assistant for the voice loop. The component should model the behavior of the built-in `voice_assistant` (status screens, text boxes, STT/TTS triggers) but is a new implementation, not a drop-in replacement.

The component is **beta-only** and **auto-response-only**. It does **not** execute Home Assistant services locally. MCP tools and tool execution are assumed to live on the Realtime API endpoint/server side; the device only logs tool/MCP-related events for debugging. A top-level `api:` component may still be present in the device YAML, but it is used only for the ESPHome dashboard connection, not for voice control.

PSRAM is required because base64 audio decoding, pending websocket message buffering, and tool-call state need more RAM than is typically available in internal ESP32 memory.

Primary supported flow:
- Local `micro_wake_word` detects a wake word.
- The component is started (via the `micro_wake_word.on_wake_word_detected` automation or an explicit `openai_realtime.start` action).
- The component opens a Realtime API WebSocket, streams microphone PCM audio, and receives server VAD + transcription + TTS audio deltas.
- If the model requests a tool call (function or MCP), the endpoint is assumed to handle it server-side; the device logs the event for debugging and does not send `function_call_output`.
- Response audio is played through an ESPHome `speaker`.
- Request/response text is published to optional `text_sensor` entities.
- When finished, the component returns to idle and restarts `micro_wake_word` so the device can detect the next wake word.

`micro_wake_word` is the sole initiation path; there is no button-based fallback.

## 2. Reference material you must read

Read these files before writing any code:

- `esphome/AGENTS.md` — coding standards, heap-allocation rules, C++ style, automation patterns.
- `esphome/esphome/components/voice_assistant/__init__.py`, `.h`, `.cpp` — exemplar for state machine, triggers, and actions.
- `esphome/esphome/components/psram/__init__.py` and `psram.h` — how to require and use PSRAM.
- `esphome/esphome/components/api/` — only if the device YAML includes a top-level `api:` component for the ESPHome dashboard; `openai_realtime` does not call Home Assistant services.
- `esphome/esphome/components/micro_wake_word/__init__.py` and `micro_wake_word.h` — how wake-word detection integrates with automations.
- `esphome/esphome/components/microphone/__init__.py` and `microphone_source.h` — how to consume microphone audio.
- `esphome/esphome/components/speaker/speaker.h` — how to play raw PCM.
- `esphome/esphome/components/ring_buffer/ring_buffer.h` — producer/consumer audio buffering.
- `esphome/esphome/components/audio/audio.h` — audio stream info helpers.
- `esphome/esphome/components/json/json_util.h` — JSON parsing helpers.
- `esphome/esphome/core/automation.h`, `component.h`, `helpers.h`, `alloc_helpers.h`, `log.h`, `defines.h`.
- `esphome/openapi.yaml` — the OpenAI Realtime API schema to validate events against.

You may also inspect:
- `esphome/examples/esp-box/examples/chatgpt_demo`
- `esphome/examples/esp-webrtc-solution/solutions/openai_demo`

## 3. Target platform and dependencies

- ESP32 only.
- ESP-IDF framework only.
- PSRAM is **required**. The target device must have PSRAM, and the YAML must include a `psram:` component.
- Add the managed IDF dependency `espressif/esp_websocket_client` (version `1.4.0` or later).
- `AUTO_LOAD = ["audio", "json", "ring_buffer"]`.
- `DEPENDENCIES = ["microphone", "micro_wake_word", "network", "psram"]`.

## 4. YAML-facing configuration

Implement a `CONFIG_SCHEMA` in `esphome/esphome/components/openai_realtime/__init__.py` with at least these options. PSRAM must be enabled in the device YAML with a top-level `psram:` component.

```yaml
psram:

api:  # optional; only for the ESPHome dashboard, not for voice control

openai_realtime:
  api_key: "..."                         # required string; may be "" for local endpoints
  model: "gpt-realtime"                  # required string
  endpoint: "wss://..."                  # required; must start with ws:// or wss://
  system_prompt: "..."                   # optional, default "You are a helpful voice assistant."
  voice: ""                              # optional string, and optional per Realtime API
  language: "en"                         # optional ISO-639-1 language string (2-8 chars)
  microphone: ...                        # microphone source; enforce 16-bit mono input
  speaker: ...                           # required; raw PCM response audio sink
  micro_wake_word: ...                   # required; sole trigger for starting a conversation
  noise_suppression_level: 0             # optional 0-4, stored only
  auto_gain: 0                           # optional 0-31 dBFS, stored only
  volume_multiplier: 1.0                 # optional float > 0, applied to decoded PCM
  text_request: ...                      # optional text_sensor; publishes the user's transcribed speech
  text_response: ...                     # optional text_sensor; publishes the assistant's transcribed response

  # Automation triggers modeled on voice_assistant
  on_listening: ...
  on_start: ...
  on_wake_word_detected: ...
  on_stt_end: ...                        # receives transcript string
  on_stt_vad_start: ...
  on_stt_vad_end: ...
  on_tts_start: ...                      # receives transcript string
  on_tts_end: ...                        # receives transcript string
  on_tts_stream_start: ...
  on_tts_stream_end: ...
  on_end: ...
  on_error: ...                          # receives (code, message)
  on_idle: ...
  on_client_connected: ...
  on_client_disconnected: ...
```

Actions to register:
- `openai_realtime.start` with optional `silence_detection` and templatable `wake_word`.
- `openai_realtime.stop`.

There is no `openai_realtime.start_continuous` action and no continuous-listening mode.

Conditions to register:
- `openai_realtime.is_running`.
- `openai_realtime.connected`.

## 5. Required supporting components for the reference hardware

The reference device is an **ESP32-S3-Box-3** running the ESP32-S3-Box package. The following components are required in the YAML for `openai_realtime` to execute on that hardware:

### Core platform and connectivity
- `esp32:` with `framework: type: esp-idf` and 240 MHz CPU.
- `psram:` — required by `openai_realtime` for audio/JSON/tool buffers. On the S3-Box use octal mode at 80 MHz.
- `wifi:` — required for the Realtime API WebSocket.
- `api:` — optional; only for the ESPHome dashboard, not used by `openai_realtime` for voice control.
- `ota:`, `logger:`, and `captive_portal:` are standard for the package but not strictly required for `openai_realtime` runtime.

### Audio input chain
- `i2c:` — the box ADC/DAC share an I2C bus.
- `i2s_audio:` — the shared I2S bus for microphone and speaker.
- `audio_adc:` platform `es7210` — the box's analog-to-digital converter for the microphone.
- `microphone:` platform `i2s_audio` — the actual microphone entity, 16-bit mono at 16 kHz.

### Audio output chain
- `audio_dac:` platform `es8311` — the box's digital-to-analog converter for the speaker.
- `speaker:` platform `i2s_audio` — the raw speaker sink, 16-bit mono at **24 kHz**; `openai_realtime` sends decoded PCM here.

### Wake word
- `micro_wake_word:` — required; triggers `openai_realtime.start` on detection.

### Display/UI (modeled on voice_assistant, not required for audio loop)
- `spi:`, `display:` (mipi_spi S3BOX), `image:`, `font:`, `text_sensor:` (template), `color:` — used for the status screen and transcript boxes.
- `output:` (ledc) and `light:` (monochromatic) — screen backlight.
- `binary_sensor:` (gpio) — physical button handling.
- `switch:` (template mute switch, speaker enable) — user controls.

### Notes for the eventual `esp32-openai.yaml`
- Keep `microphone` and `speaker` as separate `i2s_audio` entities; do not rely on `media_player` for the assistant's response audio.
- `media_player:` (speaker platform) is optional and may remain in the package only for independent user-initiated announcements; it is not the assistant's PCM output path.
- Remove continuous listening / streaming wake word support and any references to `voice_assistant` triggers/actions; replace them with `openai_realtime` equivalents.
- The `micro_wake_word.on_wake_word_detected` automation should call `openai_realtime.start` with the detected wake word.
- After `openai_realtime.on_idle`, restart `micro_wake_word` so the device resumes listening.

## 6. C++ architecture requirements

### 6.1 State machine

Use an explicit `enum class State` with these states and transitions:

```
IDLE -> CONNECTING -> START_MICROPHONE -> STARTING_MICROPHONE -> STREAMING_MICROPHONE
  -> STOP_MICROPHONE -> STOPPING_MICROPHONE -> AWAITING_RESPONSE -> STREAMING_RESPONSE
  -> (finish) -> IDLE
```

Key rules:
- `request_start()` must refuse to start if state is not `IDLE`.
- `request_stop()` must stop the microphone or signal the server depending on current state.
- `loop()` drives all transitions; never run heavy work on the ESP-IDF websocket task.

### 6.2 Threading model (critical)

Obey these rules exactly:

- The ESP-IDF websocket event callback (`websocket_event_handler_`) must **only** set lightweight flags or copy complete JSON text frames into a pending queue, then call `App.wake_loop_threadsafe()`.
- All JSON parsing, automation triggers, text sensor publishes, state transitions, and speaker playback must happen in `OpenAIRealtime::loop()` from the ESPHome main loop context.
- Use two mutex-protected pending structures:
  - `std::vector<std::string> pending_json_messages_` for complete server JSON frames.
  - boolean flags for `client_connected`, `client_disconnected`, `websocket_error`, `session_update`.
- Cap the pending JSON queue (e.g., 64 messages) and drop with a warning when full.

### 6.3 Audio buffering

- Microphone callback writes into a `ring_buffer::RingBuffer`.
- `loop()` drains the ring buffer in fixed chunks (e.g., 32 ms = 1024 bytes for 16 kHz mono PCM16) and sends each chunk as `input_audio_buffer.append`.
- Limit chunks sent per loop pass (e.g., max 4) to prevent one loop iteration from monopolizing the WebSocket send path.
- Allocate the ring buffer and any speaker buffers in `allocate_buffers_()`; deallocate in `deallocate_buffers_()` when returning to idle.
- Use PSRAM for all large or long-lived buffers: ring buffer, speaker decode buffer, pending JSON message queue, and tool-call argument buffers. Use `ExternalRAMAllocator<uint8_t>` or `RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::ALLOC_EXTERNAL)` so allocations prefer PSRAM and do not consume limited internal RAM.
- Speaker output: use a fixed `uint8_t` decode buffer (e.g., 4096 bytes). Decode base64 audio deltas in chunks; if the buffer is full, drain it to the speaker first. Apply `volume_multiplier` to decoded PCM with clipping. Set speaker stream info to match the actual output sample rate (see §7.3).

### 6.4 Fragmented WebSocket frames

Server JSON frames may be fragmented. Buffer them in `rx_message_` using `payload_offset` and `payload_len`. Drop any complete or fragmented payload larger than a safe limit (e.g., 8192 bytes) to avoid heap allocation crashes. For oversized frames, extract the `"type"` field from the first fragment so logs can report what was dropped.

### 6.5 Wake-word lifecycle

`micro_wake_word` is the only way a conversation starts; there is no physical button fallback. The component must cooperate cleanly with it:

- Accept a required `micro_wake_word` reference in the schema and store it as a pointer in C++.
- Provide an `on_wake_word_detected` trigger that fires when `openai_realtime.start` is called with a non-empty wake word.
- The recommended YAML wiring is:
  ```yaml
  micro_wake_word:
    # ... models ...
    on_wake_word_detected:
      - openai_realtime.start:
          wake_word: !lambda return wake_word;
  ```
- When the conversation finishes and `on_idle` fires, the YAML is responsible for restarting `micro_wake_word`. The component must not try to own or directly start/stop `micro_wake_word` internally; that creates microphone-ownership conflicts.
- While `openai_realtime` is running, the microphone is owned by `openai_realtime` (via `MicrophoneSource`). `micro_wake_word` must not also try to read from the same microphone. The YAML package should ensure only one of them is active at a time.
- If no speech is detected within a timeout after streaming begins, return to `IDLE`, disconnect, and trigger `on_idle` so `micro_wake_word` can restart.

## 7. Realtime API protocol compliance

### 7.1 API dialect

This component targets the OpenAI Realtime API **beta** (`OpenAI-Beta: realtime=v1`) only. There is no `api_version` option and no GA dialect support. All client events must use the flat beta field names (`modalities`, `input_audio_format`, `output_audio_format`, root-level `voice`, etc.).

### 7.2 Beta `session.update`

If targeting beta, the `session.update` event should look like the working implementation:

```json
{
  "type": "session.update",
  "session": {
    "instructions": "<system_prompt>",
    "voice": "<voice>",
    "modalities": ["text", "audio"],
    "input_audio_format": "pcm16",
    "output_audio_format": "pcm16",
    "input_audio_transcription": {"language": "<language>"},
    "turn_detection": {
      "type": "server_vad",
      "create_response": true
    }
  }
}
```

- Set `turn_detection.create_response: true`. The component is auto-response-only; it never sends a manual `response.create`.

### 7.3 Audio sample rates

- The ESPHome Box microphone is 16 kHz mono 16-bit PCM. The beta dialect accepts 16 kHz input, so no resampling is required.
- Output audio from the Realtime API beta endpoint is 24 kHz mono 16-bit PCM. Set the speaker stream info to 24 kHz, 16-bit, mono. The ESP32-S3-Box-3 `i2s_audio` speaker should be configured for 24 kHz.

### 7.4 `response.create`

Beta shape (kept for documentation; the component does not send it in auto mode):

```json
{
  "type": "response.create",
  "response": {
    "modalities": ["text", "audio"],
    "voice": "<voice>",
    "output_audio_format": "pcm16"
  }
}
```

Rules:
- The component is auto-response-only. It relies on `turn_detection.create_response: true` in `session.update` and does **not** send `response.create` itself.

### 7.5 Server events to handle

Handle these incoming events. All processing must be on the main loop.

| Event | Required handling |
|---|---|
| `session.created` / `session.updated` | Mark session configured. Optionally validate returned config. |
| `input_audio_buffer.speech_started` | Set `speech_started_ = true`; trigger `on_stt_vad_start`. |
| `input_audio_buffer.speech_stopped` | Trigger `on_stt_vad_end`; move state toward `STOP_MICROPHONE` if using server VAD. |
| `input_audio_buffer.committed` | Set `input_committed_ = true`; record timestamp. |
| `conversation.item.input_audio_transcription.delta` | Append `delta` to request text sensor. |
| `conversation.item.input_audio_transcription.completed` | Set request text, trigger `on_stt_end`, set `transcription_completed_ = true`. |
| `conversation.item.input_audio_transcription.failed` | Log warning. |
| `response.output_audio_transcript.delta` / `response.audio_transcript.delta` | Append to response text; throttle text sensor updates (e.g., 250 ms); move to `STREAMING_RESPONSE`. |
| `response.output_audio_transcript.done` / `response.audio_transcript.done` | Publish final transcript; trigger `on_tts_end`. |
| `response.output_audio.delta` / `response.audio.delta` | Decode base64 PCM, apply volume, play to speaker; move to `STREAMING_RESPONSE`. |
| `response.output_audio.done` / `response.audio.done` | Mark TTS stream ended. |
| `response.output_item.done` | Finish response. |
| `response.done` | Log status/status_details/output_modalities/output; finish response. |
| `error` | Log `error.type`, `code`, `message`, `param`, `event_id`; trigger `on_error`. |

Notes:
- The `response.audio.*` event names are beta aliases. Handle both aliases for compatibility.
- Update a `last_response_event_time_` timestamp on every `response.*` event.
- In `AWAITING_RESPONSE` and `STREAMING_RESPONSE`, if no response event arrives for a timeout period (e.g., 15 seconds), call `finish_response_()` to avoid hanging forever.

### 7.6 Tool and MCP events

The component recognizes Realtime API tool-calling and MCP events, but it does **not** execute tools locally. Tool/MCP execution is assumed to happen on the Realtime API endpoint/server side (e.g., an MCP server attached to the session). The device logs these events for debugging.

#### Events to handle

Add these to the incoming-event handler:

| Event | Required handling |
|---|---|
| `response.function_call_arguments.delta` | Append `delta` to a per-call arguments buffer. |
| `response.function_call_arguments.done` | Finalize arguments; log the call. Do not send `function_call_output`. |
| `response.mcp_call_arguments.delta` | Append `delta` to a per-call arguments buffer. |
| `response.mcp_call_arguments.done` | Finalize arguments; log the call. Do not send `function_call_output`. |
| `response.mcp_call.in_progress` | Log that an MCP call is in progress. |
| `response.mcp_call.completed` / `response.mcp_call.failed` | Log the result; do not send further output for this call. |
| `mcp_list_tools.in_progress` / `completed` / `failed` | Log only; the endpoint uses these to discover tools. |

#### Tool-call state

- Maintain a small map or struct of in-flight tool calls keyed by `call_id`/`item_id`.
- Store accumulated argument deltas until the `.done` event arrives.
- Allocate tool-call argument buffers in PSRAM.
- Limit the number of in-flight calls and the size of accumulated arguments to prevent unbounded memory use.
- Do not block the main loop waiting for tool results; simply log and continue.

### 7.7 `finish_response_()` behavior

- If TTS was streaming, trigger `on_tts_stream_end` and call `speaker_->finish()`.
- Publish any accumulated response transcript.
- Trigger `on_end`.
- Disconnect the WebSocket **before** triggering `on_idle`, then transition to `IDLE`.
- After `on_idle`, the YAML is expected to restart `micro_wake_word` so the device resumes listening for the wake word.

## 8. Connection lifecycle

- Build the WebSocket URI as `<endpoint>?model=<model>`.
- Headers:
  - `Authorization: Bearer <api_key>` (omit if `api_key` is empty).
  - `OpenAI-Beta: realtime=v1` if targeting the beta dialect or if the endpoint requires it.
- Connection timeout: if not connected and session-configured within a reasonable window (e.g., 25 seconds), trigger `on_error("connection-timeout", ...)` and return to idle.
- Reconnection: do not auto-reconnect. Each user invocation of `request_start()` creates a fresh client.
- On disconnect/error during an active session, return to idle and trigger `on_idle` so the YAML can restart `micro_wake_word`.

## 9. Known pitfalls to avoid

1. **Do not process websocket events on `websocket_task`.** Always defer to the main loop.
2. **Do not use `on_client_connected` to start `micro_wake_word`.** In this component, `on_client_connected` means the OpenAI websocket connected, not the ESPHome native API. Wake-word startup must be driven by `api.on_client_connected` or `esphome.on_boot` in the YAML package.
3. **Do not send `input_audio_buffer.commit` + `response.create` on `speech_stopped`.** The component is auto-response-only; server VAD creates the response via `turn_detection.create_response: true`.
4. **Do not keep the websocket open in idle mode.** Disconnect after every response finishes.
5. **Do not allocate unlimited memory for large JSON frames.** Cap parsed message size and drop oversized frames safely.
6. **Do not publish every transcript delta immediately.** Throttle text sensor updates to avoid blocking the loop with display redraws.
7. **Do not set the speaker stream info to 16 kHz.** Output PCM is 24 kHz mono 16-bit.
8. **Do not allocate large audio/JSON/tool buffers in internal RAM.** PSRAM is required; keep internal RAM free for the network stack and ESP-IDF tasks.

## 10. Shell features (accept but do not implement)

These options are accepted and stored for future expansion, but do not build functional runtime paths for them in the first pass:

- `noise_suppression_level`, `auto_gain`: stored; not applied to audio.

## 11. File deliverables

Create/modify these files:

```
esphome/esphome/components/openai_realtime/
├── __init__.py
├── openai_realtime.h
├── openai_realtime.cpp
├── README.md
└── esp32-openai.yaml   # example ESPHome Box package
```

Delete the old `esphome/esphome/components/openai_realtime/version 1 - gpt55/` directory before starting the rebuild.

## 12. Testing checklist

When done, the component should be verifiable by:

1. `esphome config <your>.yaml` passes validation.
2. The firmware compiles for `esp32s3box` with ESP-IDF and a top-level `psram:` component is present.
3. Boot logs show `PSRAM:` config dump with available size.
4. Boot logs show `OpenAI Assistant:` config dump.
5. Wake-word detection calls `request_start()`.
6. WebSocket opens to `<endpoint>?model=<model>`.
7. `session.created` and `session.updated` are received.
8. Microphone starts and `Audio stream stats:` logs show both `mic_rx` and `sent` increasing.
9. Speaking produces `input_audio_buffer.speech_started`, then `speech_stopped`, then transcription events.
10. Response produces transcript deltas and/or audio deltas.
11. If audio deltas are received, `Received realtime audio: ...` logs appear and sound plays from the speaker.
12. `response.done` logs `status`, `detail_type`, `reason`, `output_modalities`, and output item transcripts.
13. After response completion, the websocket disconnects and `on_idle` triggers; the YAML restarts `micro_wake_word` so the device resumes listening.
14. If no speech is detected within 5 seconds of streaming, the no-speech timeout returns to idle.
