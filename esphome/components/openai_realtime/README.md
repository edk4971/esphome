# openai_realtime

> **Audio playback is now fixed** — the component uses the proven `PsramAudioBuffer`
> from `openai_common` (the same 2 MB PSRAM ring buffer + feeder task pattern
> from `openai_responses`) for continuous, crackle-free speaker playback.
>
> The Realtime API is still beta and may not be uniformly supported across
> different OpenAI-compatible endpoints. If your endpoint doesn't support the
> Realtime WebSocket API, use [`openai_conversations`](../openai_conversations/)
> or [`openai_responses`](../openai_responses/) instead.

---

# ESPHome OpenAI Realtime (Realtime API)

An ESPHome component that connects an ESP32 device directly to an OpenAI-compatible Realtime API WebSocket endpoint for voice conversations.

## Features

- Direct WebSocket connection to an OpenAI Realtime API endpoint — no Home Assistant pipeline required for the voice loop.
- Beta Realtime API dialect only (`OpenAI-Beta: realtime=v1`).
- Server-side voice activity detection (VAD) with auto-response.
- Microphone audio streamed as `input_audio_buffer.append`.
- Response audio decoded from base64 and played through an ESPHome `speaker`
  via a 2 MB PSRAM ring buffer + dedicated feeder task (from `openai_common`),
  ensuring continuous, crackle-free playback decoupled from the main loop.
- Optional `text_sensor` entities for transcribed request/response text.
- Automation callbacks (`on_listening`, `on_stt_end`, `on_tool_start`,
  `on_tts_start`, `on_tts_stream_start/end`, `on_end`, `on_error`, `on_idle`)
  matching the pattern used by `openai_responses` and `openai_conversations`.
- MCP tool events fire `on_tool_start` so the display can show "Searching...".
- Server-side tool execution (the Realtime API connects to MCP servers itself).

## Installation

```yaml
external_components:
  - source:
      type: git
      ref: main
      url: https://github.com/edk4971/esphome
    components: [ openai_realtime, openai_common ]
```

> **Shared infrastructure:** This component depends on `openai_common`, which
> provides the `PsramAudioBuffer` ring buffer + feeder task and the
> `LazyCallbackManager` callback pattern. It is auto-loaded.
>
> **Shared hardware config:** A complete S3-Box-3 config lives in
> [`openai_common/common.yaml`](../openai_common/common.yaml). The component
> YAML in [`esp32-openai-realtime.yaml`](./esp32-openai-realtime.yaml) is a
> thin wrapper. To switch between components, change the `!include` line at
> the bottom of `common.yaml`.
>
> **Endpoint:** Accepts `ws://`, `wss://`, `http://`, or `https://`. HTTP(S)
> endpoints are auto-converted to WS(S) so all three components can share the
> same `openai_endpoint_base` secret.

## Requirements

- ESP32 with PSRAM (ESP-IDF framework only).
- An OpenAI-compatible server with a Realtime API WebSocket endpoint.
- ESPHome components: `microphone`, `speaker`, `micro_wake_word`, `psram`,
  `network`.

## Configuration

```yaml
psram:

# Optional: only needed for the ESPHome dashboard, not for voice control
api:

openai_realtime:
  api_key: "YOUR_OPENAI_API_KEY"  # may be "" for local endpoints
  model: "gpt-4o-realtime-preview-2024-12-17"
  endpoint: "wss://api.openai.com/v1/realtime"
  system_prompt: "You are a helpful voice assistant."
  voice: "alloy"
  language: "en"
  microphone: box_mic
  speaker: box_speaker
  micro_wake_word: mww
  volume_multiplier: 4.0
  text_request: request_text
  text_response: response_text
  on_listening:
    - lambda: id(status_text).publish_state("Listening...");
  on_stt_end:
    - lambda: id(request_text).publish_state(x);
  on_tts_end:
    - lambda: id(response_text).publish_state(x);
  on_idle:
    - micro_wake_word.start:
```

See [`esp32-openai-realtime.yaml`](./esp32-openai-realtime.yaml) for a full ESP32-S3-Box-3 example.

## Actions and Conditions

Actions:
- `openai_realtime.start` — optional `silence_detection` (bool) and templatable `wake_word` (string).
- `openai_realtime.stop`

Conditions:
- `openai_realtime.is_running`
- `openai_realtime.connected`

There is no `start_continuous` action and no continuous-listening mode.

## Triggers

- `on_listening`
- `on_start`
- `on_wake_word_detected`
- `on_stt_vad_start`
- `on_stt_vad_end`
- `on_stt_end` — receives transcript string
- `on_tts_start` — receives transcript string
- `on_tts_end` — receives transcript string
- `on_tts_stream_start`
- `on_tts_stream_end`
- `on_end`
- `on_error` — receives `(code, message)`
- `on_idle`
- `on_client_connected` — OpenAI WebSocket connected
- `on_client_disconnected` — OpenAI WebSocket disconnected

## Tool and MCP behavior

The component recognizes Realtime API function-call and MCP events and logs them. It does **not** execute Home Assistant services locally or send `function_call_output`. If your Realtime API endpoint has an MCP server attached, configure the tools and system prompt on the server side.

## Audio sample rates

- Microphone input: 16 kHz mono 16-bit PCM (no resampling required).
- Speaker output: 24 kHz mono 16-bit PCM.

## Hardware example

The `esp32-openai.yaml` file targets an ESP32-S3-Box-3. It includes:

- ESP-IDF at 240 MHz
- Octal PSRAM at 80 MHz
- `es7210` audio ADC and `es8311` audio DAC
- `i2s_audio` microphone at 16 kHz and speaker at 24 kHz
- `micro_wake_word` with the `ok_nabu` model
- Status/request/response `text_sensor` entities
- Optional display section (commented out; enable for your specific display model)

## Limitations

- Beta Realtime API dialect only; no GA schema support.
- Auto-response only; no manual `response.create` mode.
- `noise_suppression_level` and `auto_gain` are accepted and stored but not applied to audio.
- No client-side Home Assistant service execution.
