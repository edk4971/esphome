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

Next observation: this confirmed that `micro_wake_word` does start and detect. `Hey Mycroft` produced a mWW detection event and called `OpenAIAssistant::request_start()`, but there was still no evidence of traffic reaching the Realtime endpoint. That narrows the problem to the websocket connection path after `request_start()`. I added:

- explicit logs before opening the websocket, including the full generated endpoint URL with the `model` query parameter;
- a log after `esp_websocket_client_start()` succeeds;
- verbose logs for every websocket event;
- state transition logs;
- a 15 second connection timeout that reports `connection-timeout`, disconnects the client, returns to `IDLE`, and triggers `on_idle`;
- an `openai_assistant.on_idle` YAML hook to restart wake-word detection after connection timeout/failure;
- explicit mWW model IDs and startup-time enable calls for all three models, because mWW model enabled/disabled state is persisted in flash and can override the simple model ordering assumption.

The fact that only `Hey Mycroft` worked even after reordering models is likely explained by persisted mWW model state. Enabling all three configured model IDs in `start_wake_word` should remove that ambiguity for the next run.

Follow-up: after fixing a VLAN/firewall issue, the websocket connection succeeded and the endpoint loaded `gpt-realtime`. The ESP received `session.created` and `session.updated`, then started the microphone and entered `STREAMING_MICROPHONE`. No speech/VAD/response events followed, and ESP-IDF later logged `websocket_client: Could not lock ws-client within 0 timeout`. This strongly suggests the next problem is in the microphone audio send path rather than network reachability. I added audio-stream diagnostics:

- microphone callback byte counter;
- successfully sent PCM byte and chunk counters;
- websocket send failure counter;
- ring-buffer availability in a 5 second periodic log while streaming;
- warning logs for failed websocket sends;
- a small per-loop cap of four audio chunks to avoid one loop pass monopolizing websocket sends;
- changed websocket send timeout from `0` to `50` so transient websocket-client lock contention is visible and less likely to drop every frame immediately.

The next useful runtime log is `Audio stream stats: ...`. If `mic_rx` increases but `sent` remains zero or `send_failures` rises, the websocket send path is failing. If both `mic_rx` and `sent` increase but the server never reports speech/VAD events, the likely issue is audio format/session configuration or server-side VAD expectations.

Next observation: audio streaming works. The ESP logged microphone bytes and successful websocket audio sends, then received `input_audio_buffer.speech_started` and `input_audio_buffer.speech_stopped`. The endpoint also generated a response transcript (`I'm ready.`). Two implementation issues were identified:

- The YAML was still passing `media_player: speaker_media_player` into `openai_assistant`, but the runtime response path only plays raw Realtime PCM through `speaker::Speaker`. Because no raw `speaker` was configured on `openai_assistant`, audio deltas could not be played. The package now uses `speaker: box_speaker` for `openai_assistant`; the top-level `speaker_media_player` remains available for announcement sounds and other package media use.
- On `input_audio_buffer.speech_stopped`, the component called `signal_stop_()`, which manually sent `input_audio_buffer.commit` and `response.create`. The Realtime server was already creating a response from server VAD, so these manual events likely caused the `error` events seen immediately after speech stopped. The session update now explicitly sets `turn_detection.create_response = true`, and the speech-stopped handler only stops the local microphone instead of sending duplicate commit/create events.

I also added logging of Realtime API error details (`code`, `message`, and `param`) and added handling for `response.audio.done`, `response.output_audio.done`, and `response.output_item.done` so the component can finish a response and return to idle even if the endpoint does not emit exactly `response.done`.

Next observation: STT, server VAD, TTS, and the LLM all triggered on the endpoint. The ESP received transcription and the first response transcript delta (`The`) but then appeared hung. The logs showed a critical architectural problem: Realtime websocket data was being processed directly on ESP-IDF's `websocket_task`, and that processing triggered ESPHome automations, text sensor publishes, and display redraws. Display redraws took ~150 ms and were visible in logs as running on `websocket_task`. This can starve or block the websocket client task and explains the `Could not lock ws-client within 50 timeout` send failures and the stalled response stream.

To fix this, websocket event handling was split:

- The ESP-IDF websocket task now only records lightweight flags or copies complete JSON text frames into a small pending queue, then calls `App.wake_loop_threadsafe()`.
- `OpenAIAssistant::loop()` now drains pending websocket events/messages and runs `handle_json_message_()` from the normal ESPHome loop context.
- `on_client_connected`, `on_client_disconnected`, `on_error`, text sensor publishes, display-triggering automations, STT/TTS triggers, and response completion handling no longer run directly on `websocket_task`.

This should prevent display/text automation work from blocking the websocket receive task. It should also stop the race where `input_audio_buffer.speech_stopped` arrives on `websocket_task` while the main loop is still sending queued microphone audio frames; the speech-stopped event is now processed before the loop state machine continues, so the state can move to `STOP_MICROPHONE` before more audio append frames are sent.

Follow-up: after moving processing to the main loop, the ESP still stalled after the first response transcript token (`The`). The logs showed three relevant behaviors:

- response transcript deltas can arrive in a burst, and publishing every tiny delta can still cause lots of UI/text work;
- a large fragmented websocket text payload was seen (`payload_len=63934`) before response creation/output events;
- if the endpoint sends transcript deltas but does not send a final `response.done`/`response.output_item.done`, the component remains in `AWAITING_RESPONSE` or `STREAMING_RESPONSE` forever.

I added another robustness pass:

- pending JSON queue capacity increased from 8 to 64 messages;
- fragmented websocket completion now uses `payload_offset + data_len >= payload_len`, not only accumulated string size, and pads to the reported offset if necessary;
- response activity timestamp is updated for every `response.*` event;
- `AWAITING_RESPONSE` and `STREAMING_RESPONSE` now finish the response after 15 seconds of response inactivity, which restarts wake-word detection instead of hanging forever;
- transcript-only responses now move the state to `STREAMING_RESPONSE` even if no audio delta is received;
- response text sensor updates are throttled to at most once every 250 ms during transcript deltas, with final transcript events still publishing immediately;
- large audio deltas are decoded in chunks into the fixed speaker buffer instead of being dropped outright when the decoded payload is larger than remaining buffer space.

If the endpoint still only sends `response.output_audio_transcript.delta` and never sends `response.output_audio.delta`, the ESP will display text but will not play audio. The new `Received realtime audio delta but no speaker is configured` and speaker-buffer warnings help distinguish missing audio deltas from playback-buffer issues.

Follow-up: the response no longer hangs; after 15 seconds of response inactivity the component returns to `IDLE` and restarts mWW. The remaining observations were: no audible output, ongoing websocket ping logs after returning to idle, and full display redraws taking ~130-180 ms. I changed:

- non-continuous responses now disconnect/destroy the Realtime websocket after `finish_response_()` returns to `IDLE`, so idle devices should not keep logging websocket ping frames forever;
- decoded response PCM is now scaled by `volume_multiplier` before being written to the speaker buffer;
- response audio diagnostics now log `Received realtime audio: <bytes> bytes in <chunks> chunks` whenever `response.audio.delta` or `response.output_audio.delta` is actually received.

If that audio diagnostic never appears, the endpoint is only sending transcript deltas (`response.output_audio_transcript.delta`) and not audio deltas, regardless of the endpoint-side TTS log. In that case the next fix is session/endpoint configuration for audio output, not speaker volume. If the diagnostic appears but no sound is heard, focus on speaker sample rate/resampling, DAC volume, and PCM scaling.

Display redraws are still expensive because this package uses full-screen MIPI SPI updates. Partial display invalidation is not currently handled in this component; the practical workaround is to reduce how often assistant events call `draw_display` or avoid redrawing for every transcript/token update.

Admin cleanup and no-speech handling:

- Removed the temporary top-level `openai.yaml` force-start diagnostics that were marked with `#debug` / `//debug`, including the extra `esphome.on_boot`, extra `api.on_client_connected`, and `logger.level: VERBOSE` entries. Wake-word startup should now rely on the cleaner package lifecycle in `esp32-openai.yaml`.
- Added a no-speech timeout for the failure mode where local mWW wakes the assistant, the Realtime websocket connects, and the ESP streams microphone audio, but server VAD never emits `input_audio_buffer.speech_started` because the user spoke too early/quietly or not at all.
- The timeout starts when the microphone reaches `STREAMING_MICROPHONE`. If no `input_audio_buffer.speech_started` event is processed within 5 seconds, the component logs `No speech detected within 5000ms; returning to idle`, stops the microphone, returns to `IDLE`, triggers `on_idle`, and disconnects the non-continuous Realtime websocket. This restarts local mWW through the YAML `on_idle` path instead of streaming silence indefinitely.

Follow-up from a later run: VAD did detect speech, so the no-speech timeout was correctly not used. The assistant entered `STREAMING_RESPONSE`, then finished via the 15 second response inactivity timeout. There were still no `Received realtime audio: ...` logs, which means the endpoint is emitting transcript events but not audio delta events to the ESP. I added a response-finished summary log with response audio byte/chunk counts. If it prints `response_audio=0 bytes in 0 chunks`, the next fix needs to be on the Realtime session/endpoint side so the server sends `response.output_audio.delta` / `response.audio.delta`, not just `response.output_audio_transcript.delta`.

I also moved non-continuous websocket disconnect before `on_idle` trigger execution in `finish_response_()`. The previous order could make the `openai_assistant` loop include both idle automations and websocket teardown in one long operation; disconnecting before `on_idle` should reduce the long idle transition and prevents wake-word restart from racing with websocket teardown.

Latest observation: `Response finished: response_audio=0 bytes in 0 chunks` confirms the ESP is not receiving playable `response.audio.delta` / `response.output_audio.delta` events. It is only receiving transcript deltas. I added an explicit Realtime `voice` config option, defaulting to `alloy`, and included it in `session.update` as `session.voice`. The ESPHome package now sets `voice: alloy` beside `model: gpt-realtime`.

I also changed `finish_response_()` to publish the accumulated response transcript before finishing, so even if the endpoint never sends a final transcript/done event the display should not remain stuck on just the first throttled token. If the next run still prints `response_audio=0`, the endpoint/session is still transcript-only from the ESP's perspective and needs endpoint-side audio delta configuration or a different Realtime event format mapping.

Crash/oversized payload note: the ESP crashed once in `std::string::reserve()` while handling a fragmented websocket payload around 65 KB. ArduinoJson also failed to deserialize a similarly large message (`Can not allocate more memory for deserialization`). These large Realtime JSON messages are too big for the current ESP parser path and are not needed for the control flow we use. I added a defensive `MAX_JSON_MESSAGE_SIZE` of 8192 bytes. Complete or fragmented text frames larger than that are now dropped without allocation and logged as oversized, avoiding websocket-task heap allocation crashes. If a future endpoint sends actual audio deltas as very large JSON frames, the endpoint should be configured to use smaller delta chunks, or the component will need a streaming extractor for `type`/`delta` instead of full JSON deserialization.

Follow-up: oversized frames are still appearing, now around 72 KB, and response audio remains zero. I added lightweight type extraction from the first fragment of oversized JSON messages. Oversized-drop logs now include `type='<event type>'`. If the type is `response.audio.delta` or `response.output_audio.delta`, the component logs an additional warning that an oversized audio delta was dropped and the endpoint should be configured to send smaller audio chunks. If the type is something like `conversation.item.added`, then it is just large bookkeeping/user-audio history and not the missing playback audio.

Latest observation: the oversized frame type showed as `input_audio`, so it is input-audio conversation/history data, not output audio. `response_audio=0` still means no playable audio delta reached the ESP. To make the response request explicit, I changed server VAD config from `create_response: true` to `create_response: false`. Server VAD should still detect speech and commit the input buffer; after the ESP receives `input_audio_buffer.committed`, it now sends its own `response.create` with:

```json
{
  "type": "response.create",
  "response": {
    "modalities": ["audio", "text"],
    "voice": "<configured voice>",
    "output_audio_format": "pcm16"
  }
}
```

This avoids relying on the endpoint's default auto-created response behavior, which appears to produce transcript text but no audio deltas for this endpoint. Manual stop still commits the buffer and calls the same explicit response-create helper.

Correction after checking `openapi.yaml` lines 59333-59449 and `RealtimeResponseCreateParams`: the initial explicit `response.create` body still used beta/old fields (`modalities`, top-level `voice`, and `output_audio_format`). The GA schema uses `output_modalities`, and audio output configuration is nested under `audio.output`. It also states that audio output automatically includes a transcript, and `text` output disables audio, so the correct response request should ask only for `output_modalities: ["audio"]`.

The component now sends:

```json
{
  "type": "response.create",
  "response": {
    "output_modalities": ["audio"],
    "audio": {
      "output": {
        "format": {"type": "audio/pcm", "rate": 24000},
        "voice": "<configured voice>"
      }
    }
  }
}
```

The speaker stream info was also changed from 16 kHz to 24 kHz to match the OpenAPI `audio/pcm` output format, which only supports 24 kHz PCM.

Important input-audio note: STT and VAD were already working with the existing 16 kHz `i2s_audio` microphone stream, so input capture should not be changed casually. The component now reports the actual input rate in the GA-shaped `session.update` (`audio.input.format = {type: "audio/pcm", rate: 16000}`) but does not change the microphone component sample rate. A brief experiment adding explicit `audio.input.transcription.model = "whisper-1"` was removed to avoid changing a path that was already working. If we later want fully schema-matching 24 kHz input, `i2s_audio` can do it, but local `micro_wake_word` expects 16 kHz, so that likely requires either resampling or separate mic-source handling between mWW and OpenAI capture.

Correction: the GA-shaped `session.update` broke server VAD on the current endpoint. Since VAD/STT had already been working with the earlier beta-style session fields, I reverted `session.update` to the known-working shape (`modalities`, `input_audio_format`, `output_audio_format`, root `turn_detection`) while keeping `turn_detection.create_response = false` so the ESP still sends explicit GA-shaped `response.create` after `input_audio_buffer.committed`. This intentionally mixes the endpoint's working session-update dialect with the OpenAPI-correct response-create body.

The configured voice was also changed from `alloy` to the endpoint's actual voice model, `voice-en_US-joe-medium`.

Latest regression: sending manual `response.create` immediately on `input_audio_buffer.committed` caused the endpoint STT backend to report `transcription_failed` / `context canceled`, and the LLM saw the system prompt but not the user prompt. This suggests the manual response request was racing with or cancelling asynchronous transcription/conversation-item processing. The response creation flow now waits for `conversation.item.input_audio_transcription.completed` before sending `response.create`. If transcription does not complete within 3 seconds after `input_audio_buffer.committed`, it logs a warning and sends the response request anyway as a fallback. This should restore the user prompt path while still letting us explicitly request audio output.

Next observation: VAD, STT, and LLM ingestion worked, then `response.created` was followed almost immediately by `response.done` with no audio deltas. Per the OpenAPI schema, `response.done` includes a full `response` object with `status`, `status_details`, `output_modalities`, and output items. I added `log_response_status_()` to log those fields on every `response.done`, including any status detail error/reason and any transcript present in `response.output[*].content[*].transcript`. This should reveal whether the server is completing successfully with no audio, failing/cancelling, or returning a final transcript only.

STT language note: the endpoint auto-detected French (`fr`) for an English query. The OpenAPI schema includes `language` under `AudioTranscription` with ISO-639-1 examples such as `en`. I added an `openai_assistant.language` config option, exposed in `esp32-openai.yaml` as `language: en`, and include it in the working beta-style session update as `session.input_audio_transcription.language`. This is intentionally minimal: it does not set or change the transcription model, because the current STT path is otherwise working.

Response-create dialect correction: after the race fix, the GA-shaped `response.create` (`output_modalities` + nested `audio.output`) was accepted syntactically by the endpoint but immediately produced `response.done` with `status='cancelled'`, no output modalities, and zero output items. Because the endpoint is demonstrably using/accepting the beta-style session dialect (`modalities`, `input_audio_format`, `output_audio_format`, root `turn_detection`), I switched manual `response.create` back to the matching beta-style response dialect, but kept the important timing fix: it is sent only after transcription completion. The current request is:

```json
{
  "type": "response.create",
  "response": {
    "modalities": ["text", "audio"],
    "voice": "voice-en_US-joe-medium",
    "output_audio_format": "pcm16"
  }
}
```

This re-tests the endpoint's native response dialect without reintroducing the earlier STT cancellation race.

Endpoint concurrency observation: with LLM streaming off, TTS audio streaming on, partial transcription off, and clause splitting off, the endpoint still showed duplicate LLM/task behavior. Logs indicated the endpoint started an LLM run before the ESP's manual `response.create`, then the manual create caused `prediction_failed` / `context canceled` and a second response. This means the endpoint appears to auto-create/schedule a response despite receiving `create_response: false`, or otherwise treats manual `response.create` as competing with its internal pipeline.

I added `response_mode` with values `auto` and `manual`, defaulting the package to `response_mode: auto`. In auto mode, `session.turn_detection.create_response` is true and the ESP does not send manual `response.create` after commit/transcription. Manual mode preserves the explicit response-create behavior for future A/B tests. This should stop the duplicate-response cancellation loop and let the endpoint's native auto-response path run with its configured audio streaming options.

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
