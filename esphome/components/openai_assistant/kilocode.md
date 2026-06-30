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

## Post-Compile Update

The component compiled cleanly in the user's Docker environment after fixing the destructor declaration. The original header declared `~OpenAIAssistant() override`, but the ESPHome `Component` base in this build does not provide a destructor signature that can be overridden. The fix was to keep the destructor but remove `override`.

After the first YAML validation pass, I added compatibility for the ESPHome Box voice assistant package shape:

- `openai_assistant.start` accepts templated `wake_word`, matching `voice_assistant.start`.
- `system_prompt` is optional with a default of `You are a helpful voice assistant.`
- `media_player`, `micro_wake_word`, `noise_suppression_level`, `auto_gain`, and `volume_multiplier` are accepted by the schema.
- `on_wake_word_detected` is accepted and triggered when `openai_assistant.start` receives a non-empty wake word.
- Timer triggers and `openai_assistant::Timer`/`get_timers()` exist as a compatibility surface for the ESPHome Box display lambdas.

## Wake Word Startup Fix

After the first successful install, the device booted and dumped both `micro_wake_word` and `openai_assistant` configuration, but saying the configured wake words caused no logs and no Realtime API connection attempts. The key diagnosis was that the ESPHome Box package had been copied from `voice_assistant`, where `on_client_connected` means the ESPHome native API/Home Assistant client connected. In `openai_assistant`, `on_client_connected` means the OpenAI Realtime websocket connected. That websocket only connects after `openai_assistant.start` runs, so using `openai_assistant.on_client_connected` to start `micro_wake_word` created a circular startup dependency.

The YAML lifecycle was changed so wake-word startup happens from paths that actually occur at boot/runtime:

- `api.on_client_connected` now clears `init_in_progress`, starts wake-word detection, sets the display phase, and redraws.
- The boot fallback after the initialization delay now also starts wake-word detection if initialization is still in progress.
- `openai_assistant.on_client_connected`/`on_client_disconnected` no longer start or stop `micro_wake_word`; those are OpenAI websocket events, not Home Assistant/API readiness events.
- `hey_jarvis` was moved to the first `micro_wake_word.models` entry because ESPHome `micro_wake_word` enables only the first configured wake-word model by default.
- Temporary `logger.log` diagnostics were added to the wake-word start/stop scripts, mWW detection callback, and mute switch handlers so the next installed build shows whether wake-word detection is being started and whether the mute button automations are firing.

Follow-up observation: the device still booted normally but did not show the package-level diagnostic logs. That suggests the package-level script triggers may still not be reached in the user's actual `/config/openai.yaml` build, or the native API connection path may differ from the assumptions. To remove package/callback ambiguity, I added top-level diagnostics directly in `openai.yaml`:

- `logger.level: VERBOSE`
- `esphome.on_boot` at priority `-100` that waits 5 seconds, clears `init_in_progress`, forces `use_wake_word` false, unmutes the microphone, starts `micro_wake_word`, then redraws the display.
- `api.on_client_connected` with the same force-start sequence.

This is intentionally diagnostic and somewhat heavy-handed. If it proves that mWW starts and wake-word detection works, the final cleanup should move this back into one clean lifecycle path and remove duplicated startup logic.

## What Is Still A Shell

`media_player` is accepted and stored, but it is not used for response output.

The current runtime path decodes Realtime API `response.audio.delta` PCM16 chunks and sends them to `speaker::Speaker`. That is the right primitive for streaming raw PCM from a websocket. `media_player` is a higher-level entity better suited to URLs, files, announcements, and pipeline-managed playback. Implementing true `media_player` output would either require buffering response audio into a media pipeline format or creating a custom stream source that the media player pipeline can consume. That is extra complexity with little benefit for the direct Realtime audio path. My recommendation is to keep actual assistant response output speaker-based and use `media_player` only for independent announcements/timer sounds in the ESPHome Box package.

To make the current package behavior line up with the implementation, the YAML should ideally pass `speaker: box_speaker` to `openai_assistant` instead of only `media_player: speaker_media_player`. Keeping `media_player` accepted is useful for drop-in validation compatibility, but it should not be the primary response output path unless we intentionally build a media pipeline integration.

`micro_wake_word` is accepted and stored, but ownership coordination is minimal.

On-device wake word already works through the YAML-level `micro_wake_word.on_wake_word_detected -> openai_assistant.start` handoff. That is the cleanest integration because `micro_wake_word` owns wake-word listening, then `openai_assistant` owns the conversation once started. The stored `micro_wake_word` pointer is currently compatibility-only. A deeper integration could let `openai_assistant` directly start/stop mWW, but the existing scripts already manage that and avoid hidden microphone ownership conflicts.

`use_wake_word` / `start_continuous` is only a compatibility mode, not true streaming wake word detection.

The built-in `voice_assistant` can delegate continuous wake-word handling to Home Assistant. This component does not have a Home Assistant pipeline, so `start_continuous` currently starts the direct Realtime microphone stream rather than implementing a separate wake-word detector. For this project, on-device `micro_wake_word` should be the preferred wake-word mode.

Timer support is a shell.

The `Timer` struct, timer triggers, and `get_timers()` exist so the ESPHome Box display/timer lambdas can compile. The OpenAI Realtime API does not provide the same Home Assistant timer event stream that ESPHome `voice_assistant` receives over the native API. Implementing timers would require a local timer manager or tool/function-call handling from the model, plus event generation for `on_timer_started`, `on_timer_updated`, `on_timer_cancelled`, `on_timer_finished`, and `on_timer_tick`. Until that exists, timer UI code can compile but no assistant-created timers will appear.

`noise_suppression_level`, `auto_gain`, and `volume_multiplier` are stored but not applied.

`MicrophoneSource` already supports gain factor at source creation time, but these voice-assistant compatibility options are not mapped into active DSP. True noise suppression would need an audio preprocessing implementation. `volume_multiplier` should be applied when decoding output PCM before writing to the speaker, with clipping, if runtime testing shows output level needs component-side control.

Audio buffering is functional but basic.

The speaker side currently uses a fixed-size buffer and drops an incoming audio chunk if the buffer is full. This avoids unbounded heap growth and fragmentation, but it is not ideal under bursty network conditions. A better implementation would use a ring buffer or backpressure strategy.

Audio format handling is basic.

The component assumes 16 kHz mono PCM16 input and configures Realtime output as PCM16. The ESPHome Box speaker in the current YAML is configured at 48 kHz, while `OpenAIAssistant::setup()` sets the speaker stream info to 16 kHz. If the speaker driver/pipeline does not resample for this direct `speaker::Speaker` path, output quality or playback speed may need a resampler or a 24 kHz/48 kHz endpoint configuration.

## Current Recommendation

Use `speaker` for assistant response audio.

Keep `media_player` accepted for package compatibility and for unrelated announcements/timer sounds, but do not implement Realtime response output through `media_player` unless there is a clear requirement. The direct Realtime API gives us raw PCM chunks, and `speaker` is the natural ESPHome sink for that.

Use YAML-level `micro_wake_word` handoff for wake word support.

The best short-term behavior is `micro_wake_word` detects locally, stops itself through the existing scripts, then calls `openai_assistant.start` with `wake_word`. Direct `micro_wake_word` ownership inside `openai_assistant` can wait until the conversation path is stable.

Leave timers as a compatibility shell for now.

Timers are not part of the basic Realtime voice loop. They should be implemented later as local tool/function-call behavior if timer support is actually needed.

## Known Follow-Ups

- Verify the exact Realtime API event names against the target endpoint, especially OpenAI GA vs beta and LocalAI compatibility.
- Update the YAML to provide `speaker: box_speaker` for assistant response output if runtime testing confirms `media_player` alone produces no audio.
- Add local timer/tool-call support only if timer behavior is required.
- Decide whether `volume_multiplier` should be applied to decoded PCM output.
- Add proper audio resampling if direct speaker playback requires 48 kHz or if the endpoint requires 24 kHz input/output instead of 16 kHz PCM16.
- Improve audio output buffering to avoid drops under network burst conditions.
- Consider using stack-first JSON serialization helpers more consistently to reduce heap churn.
- Add component validation YAML once a complete test fixture environment is available.
