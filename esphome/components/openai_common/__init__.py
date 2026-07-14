"""ESPHome ``openai_common`` external component.

Shared infrastructure for the ``openai_responses``, ``openai_conversations``
and ``openai_realtime`` (config key ``openai_assistant``) voice assistant
components. Provides reusable C++ classes:

* :cpp:class:`esphome::openai_common::OpenAIBase` — mic/speaker/MWW wiring,
  callbacks, config fields, speaker buffer, MWW lifecycle, teardown.
* :cpp:class:`esphome::openai_common::OpenAIHTTPBase` — HTTP task, VAD, MCP,
  SSE, request builders (shared by responses + conversations).
* :cpp:class:`esphome::openai_common::PsramAudioBuffer` — 2 MB PSRAM SPSC
  ring buffer + dedicated feeder task for continuous speaker playback.

This is a dependency-only component: it has no ``CONFIG_SCHEMA`` and is
auto-loaded by the three components (like ``audio``/``json``/``ring_buffer``).
"""

import esphome.codegen as cg

# Components auto-loaded so their C++ is available without the user listing
# them in their YAML. ``audio`` provides AudioStreamInfo, ``json`` provides the
# ArduinoJson parse/build helpers, ``ring_buffer`` provides the thread-safe mic
# buffer used to hand audio from the mic callback to the main loop.
AUTO_LOAD = ["audio", "json", "ring_buffer"]

# Hard requirements: a microphone source, wake-word detection, networking and
# PSRAM (all audio/HTTP buffers live in external RAM).
DEPENDENCIES = ["microphone", "micro_wake_word", "network", "psram"]

CODEOWNERS = ["@edk4971"]

openai_common_ns = cg.esphome_ns.namespace("openai_common")
