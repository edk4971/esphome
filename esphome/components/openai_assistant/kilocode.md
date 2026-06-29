# OpenAI Assistant Implementation Note

## What I Was Trying To Do

I started the first implementation pass for the `openai_assistant` ESPHome component. The goal was to create a direct ESP32-to-OpenAI-compatible Realtime API component that can eventually act as a drop-in or near-drop-in replacement for ESPHome's built-in `voice_assistant`, without routing voice traffic through Home Assistant.

The concrete target for this pass was a minimal but real component scaffold that can be built on:

- Validate the required YAML options: `api_key`, `model`, `endpoint`, and `system_prompt`.
- Accept a microphone source and stream 16-bit mono PCM audio to a Realtime API websocket.
- Receive Realtime API text/audio events.
- Play raw PCM response audio through an ESPHome `speaker` when configured.
- Publish request and response text to optional `text_sensor` entities.
- Expose the common voice-assistant-style automations and actions such as start, stop, connected, is_running, STT/TTS triggers, and error handling.

This is not intended to be the final complete component. It is the first coding baseline so it can be compiled and tested in a real ESP-IDF/Docker environment.

## How I Went About It

I inspected the existing `openai_assistant` folder first. It only contained the planning/reference files `README.md` and `opencode.md`, so I added the standard ESPHome component files:

- `__init__.py`
- `openai_assistant.h`
- `openai_assistant.cpp`

In `__init__.py`, I followed ESPHome component codegen conventions:

- Created the `openai_assistant` namespace and `OpenAIAssistant` C++ class binding.
- Added a schema for the required OpenAI Realtime configuration.
- Reused `microphone.microphone_source_schema()` so the component gets a `MicrophoneSource` with enforced 16-bit mono input.
- Added optional `speaker`, `text_request`, and `text_response` references.
- Added automation triggers modeled after `voice_assistant` names where practical.
- Registered `openai_assistant.start`, `openai_assistant.start_continuous`, and `openai_assistant.stop` actions.
- Registered `openai_assistant.is_running` and `openai_assistant.connected` conditions.
- Marked the component ESP32 + ESP-IDF only, because the initial runtime uses ESP-IDF's `esp_websocket_client`.
- Added the managed IDF dependency `espressif/esp_websocket_client`.

In C++, I implemented a compact runtime state machine:

- `IDLE`
- `CONNECTING`
- `START_MICROPHONE`
- `STARTING_MICROPHONE`
- `STREAMING_MICROPHONE`
- `STOP_MICROPHONE`
- `STOPPING_MICROPHONE`
- `AWAITING_RESPONSE`
- `STREAMING_RESPONSE`

The microphone callback writes processed microphone bytes into a `ring_buffer::RingBuffer`. The loop drains that buffer in 32 ms chunks, base64-encodes each chunk, and sends it as `input_audio_buffer.append` Realtime API JSON over the websocket.

When stopping or when server VAD reports `input_audio_buffer.speech_stopped`, the component sends:

- `input_audio_buffer.commit`
- `response.create`

Incoming websocket text frames are parsed as JSON. Fragmented websocket payloads are buffered into `rx_message_` before parsing. The handler currently recognizes the common Realtime API event names for:

- session created/updated
- input speech start/stop
- transcription deltas/completion
- response transcript deltas/completion
- audio deltas
- response completion
- errors

Response audio deltas are expected as base64 PCM16 and are decoded into a small speaker buffer, then drained to the configured ESPHome `speaker` from `loop()`.

## Sources I Used

Primary local sources:

- `esphome/components/openai_assistant/README.md`
- `esphome/components/openai_assistant/opencode.md`
- `esphome/components/voice_assistant/__init__.py`
- `esphome/components/voice_assistant/voice_assistant.h`
- `esphome/components/voice_assistant/voice_assistant.cpp`
- `esphome/components/microphone/__init__.py`
- `esphome/components/microphone/microphone_source.h`
- `esphome/components/speaker/speaker.h`
- `esphome/components/ring_buffer/ring_buffer.h`
- `esphome/components/audio/audio.h`
- `esphome/components/json/json_util.h`
- `esphome/core/automation.h`
- `esphome/core/component.h`
- `esphome/core/helpers.h`
- `esphome/core/alloc_helpers.h`

External/reference material already listed in `opencode.md` and read before implementation:

- ESPHome core/component coding patterns.
- ESPHome `voice_assistant` as the main API and state-machine exemplar.
- ESPHome audio, microphone, speaker, ring buffer, socket, and API components.
- Espressif `esp-box` `chatgpt_demo`, mainly for feasibility of direct OpenAI calls from ESP32.
- Espressif `esp-webrtc-solution` `openai_demo`, mainly for Realtime API session setup and event usage.
- LocalAI Realtime API transports documentation, especially the WebSocket transport note that audio is sent and received as raw PCM in Realtime API protocol messages.
- ESPHome coding standards from `AGENTS.md`.

## Decisions I Made And Why

I chose WebSocket first instead of WebRTC.

WebRTC is attractive for latency and Opus transport, but it would require a much larger stack: SDP exchange, peer connection setup, RTP/audio codec negotiation, data channels, and more IDF/media dependencies. For a first ESPHome component pass, WebSocket is smaller, easier to validate, and matches LocalAI's documented Realtime transport for raw PCM. This keeps the component closer to ESPHome's existing event loop model.

I made the component ESP32 ESP-IDF only.

The implementation uses `esp_websocket_client`, which is an ESP-IDF component. Supporting Arduino or non-ESP32 targets would require a separate websocket/TLS abstraction and more conditional code. I intentionally avoided that compatibility layer in the first pass.

I used ESPHome `MicrophoneSource` instead of talking directly to a microphone component.

`MicrophoneSource` already handles channel selection, bit-depth conversion, gain, start/stop, and callback integration. This matches the built-in `voice_assistant` pattern and reduces duplicated audio handling.

I used `ring_buffer::RingBuffer` for microphone buffering.

The microphone callback can occur independently of the main component `loop()`. The ring buffer gives a simple producer/consumer boundary. This follows the `voice_assistant` approach while keeping the first implementation simpler than its zero-copy `RingBufferAudioSource` path.

I used base64 JSON audio messages instead of binary websocket frames.

OpenAI Realtime's WebSocket protocol uses JSON events such as `input_audio_buffer.append` with base64 audio. That is more interoperable with OpenAI-compatible endpoints and LocalAI than inventing a binary frame protocol.

I exposed voice-assistant-like triggers and actions, but did not copy the full Home Assistant API bridge.

The point of this component is to bypass Home Assistant for the voice loop. So I kept familiar YAML-facing hooks (`on_stt_end`, `on_tts_start`, `on_end`, etc.) but implemented them from Realtime API events instead of ESPHome API protobuf messages.

I kept `media_player` out of the first runtime path.

The built-in `voice_assistant` supports both `speaker` and `media_player`. For direct Realtime API audio deltas, raw `speaker` playback is the most direct mapping. `media_player` usually expects URLs or higher-level media calls, not continuous raw PCM websocket chunks. That can be added later if there is a clear buffering/URL strategy.

I made `text_request` and `text_response` optional existing `text_sensor` references.

The README described these as UI feedback hooks. Referencing existing text sensors keeps ownership clear and avoids creating nested platform entities in this first pass.

I did not implement `micro_wake_word` wiring yet.

`use_wake_word` exists in the schema to preserve the intended shape, but actual local wake-word integration needs careful coordination with `micro_wake_word`'s own microphone source and detection trigger. I avoided a rushed integration that might fight over microphone ownership.

I used a small speaker decode buffer and drop-on-overflow behavior.

This first pass avoids unbounded buffering. If response audio arrives faster than the speaker can drain, the component logs and drops that chunk rather than allocating more heap at runtime. A later pass should improve this with a ring buffer or backpressure strategy.

I avoided tests in this pass because this workspace does not contain the normal ESPHome `tests/` fixture tree, and the user indicated they will test in Docker.

## Known Follow-Ups

- Compile in the Docker ESPHome environment and fix any `esp_websocket_client` API/version mismatches.
- Confirm whether `espressif/esp_websocket_client` version `1.4.0` is available and appropriate for the target ESP-IDF version.
- Verify the exact Realtime API event names against the target endpoint, especially OpenAI GA vs beta and LocalAI compatibility.
- Add direct `micro_wake_word` support.
- Decide whether to support `media_player` or keep the component speaker-only for output.
- Add proper audio resampling if target endpoints require 24 kHz input/output instead of 16 kHz PCM16.
- Improve audio output buffering to avoid drops under network burst conditions.
- Consider using stack-first JSON serialization helpers more consistently to reduce heap churn.
- Add component validation YAML once a complete test fixture environment is available.
