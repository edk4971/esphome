"""ESPHome ``openai_conversations`` external component.

A voice assistant that talks to an OpenAI-compatible server over the **Chat
Completions** + **Audio** HTTP APIs (request-based, *not* the Realtime
WebSocket API). It is intended for an ESP32-S3-Box-3 (PSRAM, mic, speaker and
``micro_wake_word``).

Two pipelines are supported, selected by whether ``stt_model`` is set:

* **Multimodal (Mode 1)** — the recorded WAV is sent directly to a multimodal
  chat model via an ``audio_url`` data URL. No separate STT step.
* **STT + LLM (Mode 2)** — the WAV is first transcribed with
  ``/v1/audio/transcriptions``, then the transcript text is sent to the chat
  model.

The response text is then synthesised with ``/v1/audio/speech`` and streamed to
the speaker.
"""

import json

from esphome import automation
from esphome.automation import register_action, register_condition
import esphome.codegen as cg
from esphome.components import esp32, micro_wake_word, microphone, speaker, text_sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_MICROPHONE,
    CONF_ON_CLIENT_CONNECTED,
    CONF_ON_CLIENT_DISCONNECTED,
    CONF_ON_ERROR,
    CONF_ON_IDLE,
    CONF_ON_START,
    CONF_SPEAKER,
)
from esphome.core import CORE

# Components auto-loaded so their C++ is available without the user listing them
# in their YAML. ``audio`` provides AudioStreamInfo, ``json`` provides the
# ArduinoJson parse/build helpers, ``ring_buffer`` provides the thread-safe mic
# buffer used to hand audio from the mic callback to the main loop.
AUTO_LOAD = ["audio", "json", "ring_buffer"]

# Hard requirements: a microphone source, wake-word detection, networking and
# PSRAM (all audio/HTTP buffers live in external RAM).
DEPENDENCIES = ["microphone", "micro_wake_word", "network", "psram"]

CODEOWNERS = ["@edk4971"]

# --- Config key constants -----------------------------------------------------
# These are not in esphome/const.py because they are specific to this component.
CONF_API_KEY = "api_key"
CONF_ENDPOINT_BASE = "endpoint_base"
CONF_CHAT_MODEL = "chat_model"
CONF_STT_MODEL = "stt_model"
CONF_STT_LANGUAGE = "stt_language"
CONF_TTS_MODEL = "tts_model"
CONF_TTS_VOICE = "tts_voice"
CONF_TTS_SAMPLE_RATE = "tts_sample_rate"
CONF_SYSTEM_PROMPT = "system_prompt"
CONF_SILENCE_THRESHOLD = "silence_threshold"
CONF_SILENCE_DURATION_MS = "silence_duration_ms"
CONF_MAX_RECORDING_MS = "max_recording_ms"
CONF_VOLUME_MULTIPLIER = "volume_multiplier"
CONF_TOOLS_FILE = "tools_file"
CONF_TOOLS_CACHE_TTL = "tools_cache_ttl"
CONF_TEXT_REQUEST = "text_request"
CONF_TEXT_RESPONSE = "text_response"
CONF_MICRO_WAKE_WORD = "micro_wake_word"
CONF_SILENCE_DETECTION = "silence_detection"
CONF_WAKE_WORD = "wake_word"

# --- MCP server configuration ---
CONF_MCP_SERVERS = "mcp_servers"
CONF_MCP_SERVER_NAME = "name"
CONF_MCP_SERVER_URL = "url"
CONF_MCP_SERVER_API_KEY = "api_key"

CONF_ON_END = "on_end"
CONF_ON_LISTENING = "on_listening"
CONF_ON_STT_END = "on_stt_end"
CONF_ON_TTS_END = "on_tts_end"
CONF_ON_TTS_START = "on_tts_start"
CONF_ON_TTS_STREAM_END = "on_tts_stream_end"
CONF_ON_TTS_STREAM_START = "on_tts_stream_start"
CONF_ON_WAKE_WORD_DETECTED = "on_wake_word_detected"

# --- Codegen namespace + classes ---------------------------------------------
openai_conversations_ns = cg.esphome_ns.namespace("openai_conversations")
OpenAIConversations = openai_conversations_ns.class_(
    "OpenAIConversations", cg.Component
)

# Actions/conditions registered with the automation framework. These are
# Parented<> templates so the generated code stores a pointer to the parent
# OpenAIConversations component.
StartAction = openai_conversations_ns.class_(
    "StartAction", automation.Action, cg.Parented.template(OpenAIConversations)
)
StopAction = openai_conversations_ns.class_(
    "StopAction", automation.Action, cg.Parented.template(OpenAIConversations)
)
IsRunningCondition = openai_conversations_ns.class_(
    "IsRunningCondition", automation.Condition, cg.Parented.template(OpenAIConversations)
)


def _validate_endpoint_base(value):
    """Ensure the server base URL uses an explicit http(s) scheme.

    The C++ side concatenates ``endpoint_base`` with fixed ``/v1/...`` paths,
    so a missing scheme would silently produce a relative URL.
    """
    value = cv.string(value)
    if not value.startswith(("http://", "https://")):
        raise cv.Invalid("endpoint_base must start with http:// or https://")
    # Strip any trailing slash so concatenation does not double it up.
    return value.rstrip("/")


def _validate_mcp_server_url(value):
    """Ensure the MCP server URL uses an explicit http(s) scheme."""
    value = cv.string(value)
    if not value.startswith(("http://", "https://")):
        raise cv.Invalid("mcp_server url must start with http:// or https://")
    return value


MCP_SERVER_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_MCP_SERVER_NAME): cv.string,
        cv.Required(CONF_MCP_SERVER_URL): _validate_mcp_server_url,
        cv.Optional(CONF_MCP_SERVER_API_KEY, default=""): cv.string,
    }
)


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(OpenAIConversations),
            cv.Required(CONF_ENDPOINT_BASE): _validate_endpoint_base,
            cv.Optional(CONF_API_KEY, default=""): cv.string,
            cv.Required(CONF_CHAT_MODEL): cv.string,
            # When set, the component runs in STT+LLM mode (Mode 2). When
            # omitted, multimodal mode (Mode 1) is used and the WAV is sent
            # directly to the chat model.
            cv.Optional(CONF_STT_MODEL): cv.string,
            # ISO 639-1 language code (e.g. "en", "fr", "de") sent as the
            # ``language`` form field in the STT multipart request. Helps the
            # transcription model; set to "" to omit (auto-detect).
            cv.Optional(CONF_STT_LANGUAGE, default="en"): cv.string,
            cv.Required(CONF_TTS_MODEL): cv.string,
            cv.Optional(CONF_TTS_VOICE, default=""): cv.string,
            cv.Optional(CONF_TTS_SAMPLE_RATE, default=24000): cv.positive_int,
            cv.Optional(
                CONF_SYSTEM_PROMPT, default="You are a helpful voice assistant."
            ): cv.string,
            cv.Optional(CONF_MICROPHONE, default={}): microphone.microphone_source_schema(
                min_bits_per_sample=16,
                max_bits_per_sample=16,
                min_channels=1,
                max_channels=1,
            ),
            cv.Required(CONF_SPEAKER): cv.use_id(speaker.Speaker),
            cv.Required(CONF_MICRO_WAKE_WORD): cv.use_id(
                micro_wake_word.MicroWakeWord
            ),
            # RMS below this (relative to full-scale int16) is treated as
            # silence. 0.002 works for the S3-Box-3 (idle ~0.0005, speech
            # ~0.004). Tune per hardware.
            cv.Optional(CONF_SILENCE_THRESHOLD, default=0.002): cv.float_range(
                min=0.0, min_included=False
            ),
            cv.Optional(
                CONF_SILENCE_DURATION_MS, default=700
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_MAX_RECORDING_MS, default=30000
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_VOLUME_MULTIPLIER, default=1.0): cv.float_range(
                min=0.0, min_included=False
            ),
            # A pre-formatted chat-completions "tools" JSON array. Delivered to
            # C++ as a generated raw-string header (large JSON corrupts through
            # cg.add() string escaping).
            cv.Optional(CONF_TOOLS_FILE): cv.file_,
            # How long (ms) the live MCP tools cache is valid before a refresh
            # is needed. Default 24h. Set to 0s to refresh every turn. Only
            # effective when mcp_servers is configured.
            cv.Optional(
                CONF_TOOLS_CACHE_TTL, default="24h"
            ): cv.positive_time_period_milliseconds,
            # MCP servers for client-side tool execution. When the LLM returns
            # finish_reason=tool_calls, the component calls tools/call on each
            # configured server, injects the results, and re-queries the LLM.
            cv.Optional(CONF_MCP_SERVERS): cv.ensure_list(MCP_SERVER_SCHEMA),
            cv.Optional(CONF_TEXT_REQUEST): cv.use_id(text_sensor.TextSensor),
            cv.Optional(CONF_TEXT_RESPONSE): cv.use_id(text_sensor.TextSensor),
            cv.Optional(CONF_ON_LISTENING): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_START): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_WAKE_WORD_DETECTED): automation.validate_automation(
                single=True
            ),
            cv.Optional(CONF_ON_STT_END): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_TTS_START): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_TTS_END): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_TTS_STREAM_START): automation.validate_automation(
                single=True
            ),
            cv.Optional(CONF_ON_TTS_STREAM_END): automation.validate_automation(
                single=True
            ),
            cv.Optional(CONF_ON_END): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_ERROR): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_IDLE): automation.validate_automation(single=True),
            # Accepted for source compatibility with openai_realtime configs but
            # unused: there is no persistent connection in the HTTP model.
            cv.Optional(CONF_ON_CLIENT_CONNECTED): automation.validate_automation(
                single=True
            ),
            cv.Optional(CONF_ON_CLIENT_DISCONNECTED): automation.validate_automation(
                single=True
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    cv.only_with_framework("esp-idf"),
)


FINAL_VALIDATE_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_MICROPHONE): microphone.final_validate_microphone_source_schema(
            "openai_conversations", sample_rate=16000
        ),
    },
    extra=cv.ALLOW_EXTRA,
)


async def to_code(config):
    """Generate the C++ component instance and wire up all config."""
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # Microphone source: a MicrophoneSource wraps the raw mic and converts to
    # 16-bit mono. The component owns its data callback.
    mic_source = await microphone.microphone_source_to_code(config[CONF_MICROPHONE])
    cg.add(var.set_microphone_source(mic_source))

    spkr = await cg.get_variable(config[CONF_SPEAKER])
    cg.add(var.set_speaker(spkr))

    mww = await cg.get_variable(config[CONF_MICRO_WAKE_WORD])
    cg.add(var.set_micro_wake_word(mww))

    # API/server configuration
    cg.add(var.set_endpoint_base(config[CONF_ENDPOINT_BASE]))
    cg.add(var.set_api_key(config[CONF_API_KEY]))
    cg.add(var.set_chat_model(config[CONF_CHAT_MODEL]))
    if (stt_model := config.get(CONF_STT_MODEL)) is not None:
        cg.add(var.set_stt_model(stt_model))
    cg.add(var.set_stt_language(config[CONF_STT_LANGUAGE]))
    cg.add(var.set_tts_model(config[CONF_TTS_MODEL]))
    cg.add(var.set_tts_voice(config[CONF_TTS_VOICE]))
    cg.add(var.set_tts_sample_rate(config[CONF_TTS_SAMPLE_RATE]))
    cg.add(var.set_system_prompt(config[CONF_SYSTEM_PROMPT]))

    # VAD / recording tuning
    cg.add(var.set_silence_threshold(config[CONF_SILENCE_THRESHOLD]))
    cg.add(var.set_silence_duration_ms(config[CONF_SILENCE_DURATION_MS]))
    cg.add(var.set_max_recording_ms(config[CONF_MAX_RECORDING_MS]))
    cg.add(var.set_volume_multiplier(config[CONF_VOLUME_MULTIPLIER]))

    # MCP tools cache TTL (only meaningful with mcp_servers, but always set).
    cg.add(var.set_tools_cache_ttl_ms(config[CONF_TOOLS_CACHE_TTL]))

    # Tools: read the user-supplied JSON file and emit it as a generated C++
    # header with a raw string literal. The header lives in src/ (on the include
    # path) and is conditionally included by the .cpp when
    # USE_OPENAI_CONVERSATIONS_TOOLS is defined. We avoid passing the JSON
    # through cg.add() because codegen string escaping mangles large payloads
    # with many embedded quotes/escapes.
    if (tools_file := config.get(CONF_TOOLS_FILE)) is not None:
        with open(str(tools_file), encoding="utf-8") as f:
            tools_json = f.read()
        # Validate it parses so a bad file fails at codegen, not at runtime.
        json.loads(tools_json)

        from esphome.helpers import write_file_if_changed

        header_path = CORE.relative_build_path("src", "openai_conversations_tools.h")
        header_content = (
            "#pragma once\n"
            "namespace esphome::openai_conversations {\n"
            'static const char *const TOOLS_JSON = R"JSON('
            + tools_json
            + ')JSON";\n'
            "}  // namespace esphome::openai_conversations\n"
        )
        write_file_if_changed(header_path, header_content)
        cg.add_define("USE_OPENAI_CONVERSATIONS_TOOLS")
        cg.add(var.set_has_tools(True))

    # MCP servers: configure each for client-side tool execution.
    if (mcp_servers := config.get(CONF_MCP_SERVERS)) is not None:
        for server in mcp_servers:
            cg.add(
                var.add_mcp_server(
                    server[CONF_MCP_SERVER_NAME],
                    server[CONF_MCP_SERVER_URL],
                    server[CONF_MCP_SERVER_API_KEY],
                )
            )
        cg.add_define("USE_OPENAI_CONVERSATIONS_MCP")
        cg.add_define("MAX_PARALLEL_TOOLS", 4)
        cg.add_define("MAX_TOOL_ROUNDS", 5)
        cg.add_define("MAX_TOOL_RESULT_BYTES", 8192)

    # Text sensors for the user transcript (STT mode only) and the response.
    if (text_request_id := config.get(CONF_TEXT_REQUEST)) is not None:
        sens = await cg.get_variable(text_request_id)
        cg.add(var.set_text_request_sensor(sens))
    if (text_response_id := config.get(CONF_TEXT_RESPONSE)) is not None:
        sens = await cg.get_variable(text_response_id)
        cg.add(var.set_text_response_sensor(sens))

    # Automations. Each is registered as a lightweight callback (no Trigger
    # subclass) via build_callback_automation: the parent exposes a templatized
    # add_on_*_callback() method and a LazyCallbackManager, and the generated
    # forwarder struct fires the automation directly.
    #
    # (conf_key, callback_method, args) tuples.
    callback_entries = (
        (CONF_ON_LISTENING, "add_on_listening_callback", []),
        (CONF_ON_START, "add_on_start_callback", []),
        (CONF_ON_WAKE_WORD_DETECTED, "add_on_wake_word_detected_callback", []),
        (CONF_ON_STT_END, "add_on_stt_end_callback", [(cg.std_string, "x")]),
        (CONF_ON_TTS_START, "add_on_tts_start_callback", [(cg.std_string, "x")]),
        (CONF_ON_TTS_END, "add_on_tts_end_callback", [(cg.std_string, "x")]),
        (CONF_ON_TTS_STREAM_START, "add_on_tts_stream_start_callback", []),
        (CONF_ON_TTS_STREAM_END, "add_on_tts_stream_end_callback", []),
        (CONF_ON_END, "add_on_end_callback", []),
        (CONF_ON_IDLE, "add_on_idle_callback", []),
        (
            CONF_ON_ERROR,
            "add_on_error_callback",
            [(cg.std_string, "code"), (cg.std_string, "message")],
        ),
    )
    for conf_key, method, args in callback_entries:
        if (conf := config.get(conf_key)) is not None:
            await automation.build_callback_automation(var, method, args, conf)

    cg.add_define("USE_OPENAI_CONVERSATIONS")

    # esp_http_client is excluded from ESPHome's ESP-IDF build by default (to
    # save compile time). Re-enable it explicitly; audio's to_code (auto-loaded)
    # also does this, but we declare the dependency here so the component is
    # self-contained.
    esp32.include_builtin_idf_component("esp_http_client")


# --- Actions / conditions -----------------------------------------------------

OPENAI_CONVERSATIONS_ACTION_SCHEMA = cv.Schema(
    {cv.GenerateID(): cv.use_id(OpenAIConversations)}
)


@register_action(
    "openai_conversations.start",
    StartAction,
    OPENAI_CONVERSATIONS_ACTION_SCHEMA.extend(
        {
            cv.Optional(CONF_SILENCE_DETECTION, default=True): cv.boolean,
            cv.Optional(CONF_WAKE_WORD): cv.templatable(cv.string),
        }
    ),
    synchronous=True,
)
async def openai_conversations_start_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_silence_detection(config[CONF_SILENCE_DETECTION]))
    if (wake_word := config.get(CONF_WAKE_WORD)) is not None:
        templ = await cg.templatable(wake_word, args, cg.std_string)
        cg.add(var.set_wake_word(templ))
    return var


@register_action(
    "openai_conversations.stop",
    StopAction,
    OPENAI_CONVERSATIONS_ACTION_SCHEMA,
    synchronous=True,
)
async def openai_conversations_stop_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@register_condition(
    "openai_conversations.is_running",
    IsRunningCondition,
    OPENAI_CONVERSATIONS_ACTION_SCHEMA,
)
async def openai_conversations_is_running_to_code(
    config, condition_id, template_arg, args
):
    var = cg.new_Pvariable(condition_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var
