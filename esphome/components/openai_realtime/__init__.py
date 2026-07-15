from esphome import automation
from esphome.automation import register_action, register_condition
import esphome.codegen as cg
from esphome.components import esp32, micro_wake_word, microphone, speaker, text_sensor
from esphome.components.openai_common import register_generic_openai_actions
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_MICROPHONE,
    CONF_MODEL,
    CONF_ON_CLIENT_CONNECTED,
    CONF_ON_CLIENT_DISCONNECTED,
    CONF_ON_ERROR,
    CONF_ON_IDLE,
    CONF_ON_START,
    CONF_SPEAKER,
)
from esphome.core import CORE

AUTO_LOAD = ["audio", "json", "ring_buffer", "openai_common"]
DEPENDENCIES = ["microphone", "micro_wake_word", "network", "psram"]

CODEOWNERS = ["@edk4971"]

CONF_API_KEY = "api_key"
CONF_ENDPOINT = "endpoint"
CONF_SYSTEM_PROMPT = "system_prompt"
CONF_VOICE = "voice"
CONF_LANGUAGE = "language"
CONF_TEXT_REQUEST = "text_request"
CONF_TEXT_RESPONSE = "text_response"
CONF_MICRO_WAKE_WORD = "micro_wake_word"
CONF_NOISE_SUPPRESSION_LEVEL = "noise_suppression_level"
CONF_AUTO_GAIN = "auto_gain"
CONF_VOLUME_MULTIPLIER = "volume_multiplier"
CONF_SILENCE_DETECTION = "silence_detection"
CONF_WAKE_WORD = "wake_word"
CONF_TOOLS = "tools"
CONF_TOOLS_FILE = "tools_file"

CONF_ON_END = "on_end"
CONF_ON_LISTENING = "on_listening"
CONF_ON_STT_END = "on_stt_end"
CONF_ON_STT_VAD_END = "on_stt_vad_end"
CONF_ON_STT_VAD_START = "on_stt_vad_start"
CONF_ON_TOOL_START = "on_tool_start"
CONF_ON_TTS_END = "on_tts_end"
CONF_ON_TTS_START = "on_tts_start"
CONF_ON_TTS_STREAM_END = "on_tts_stream_end"
CONF_ON_TTS_STREAM_START = "on_tts_stream_start"
CONF_ON_WAKE_WORD_DETECTED = "on_wake_word_detected"

openai_assistant_ns = cg.esphome_ns.namespace("openai_assistant")
OpenAIAssistant = openai_assistant_ns.class_("OpenAIAssistant", cg.Component)

StartAction = openai_assistant_ns.class_(
    "StartAction", automation.Action, cg.Parented.template(OpenAIAssistant)
)
StopAction = openai_assistant_ns.class_(
    "StopAction", automation.Action, cg.Parented.template(OpenAIAssistant)
)
IsRunningCondition = openai_assistant_ns.class_(
    "IsRunningCondition", automation.Condition, cg.Parented.template(OpenAIAssistant)
)
ConnectedCondition = openai_assistant_ns.class_(
    "ConnectedCondition", automation.Condition, cg.Parented.template(OpenAIAssistant)
)


def _validate_endpoint(value):
    value = cv.string(value)
    if not value.startswith(("ws://", "wss://")):
        raise cv.Invalid("endpoint must start with ws:// or wss://")
    return value


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(OpenAIAssistant),
            cv.Required(CONF_API_KEY): cv.string,
            cv.Required(CONF_MODEL): cv.string,
            cv.Required(CONF_ENDPOINT): _validate_endpoint,
            cv.Optional(
                CONF_SYSTEM_PROMPT, default="You are a helpful voice assistant."
            ): cv.string,
            cv.Optional(CONF_VOICE, default=""): cv.string,
            cv.Optional(CONF_LANGUAGE): cv.All(cv.string, cv.Length(min=2, max=8)),
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
            cv.Optional(CONF_NOISE_SUPPRESSION_LEVEL, default=0): cv.int_range(0, 4),
            cv.Optional(CONF_AUTO_GAIN, default="0dBFS"): cv.All(
                cv.float_with_unit("decibel full scale", "(dBFS|dbfs|DBFS)"),
                cv.int_range(0, 31),
            ),
            cv.Optional(CONF_VOLUME_MULTIPLIER, default=1.0): cv.float_range(
                min=0.0, min_included=False
            ),
            cv.Optional(CONF_TOOLS): cv.ensure_list(
                cv.Schema(
                    {
                        cv.Required("type"): cv.string,
                        cv.Optional("auth_token"): cv.string,
                    },
                    extra=cv.ALLOW_EXTRA,
                )
            ),
            cv.Optional(CONF_TOOLS_FILE): cv.file_,
            cv.Optional(CONF_TEXT_REQUEST): cv.use_id(text_sensor.TextSensor),
            cv.Optional(CONF_TEXT_RESPONSE): cv.use_id(text_sensor.TextSensor),
            cv.Optional(CONF_ON_LISTENING): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_START): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_WAKE_WORD_DETECTED): automation.validate_automation(
                single=True
            ),
            cv.Optional(CONF_ON_STT_END): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_STT_VAD_START): automation.validate_automation(
                single=True
            ),
            cv.Optional(CONF_ON_STT_VAD_END): automation.validate_automation(
                single=True
            ),
            cv.Optional(CONF_ON_TOOL_START): automation.validate_automation(single=True),
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
            "openai_assistant", sample_rate=16000
        ),
    },
    extra=cv.ALLOW_EXTRA,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    mic_source = await microphone.microphone_source_to_code(config[CONF_MICROPHONE])
    cg.add(var.set_microphone_source(mic_source))

    spkr = await cg.get_variable(config[CONF_SPEAKER])
    cg.add(var.set_speaker(spkr))

    mww = await cg.get_variable(config[CONF_MICRO_WAKE_WORD])
    cg.add(var.set_micro_wake_word(mww))

    cg.add(var.set_api_key(config[CONF_API_KEY]))
    cg.add(var.set_model(config[CONF_MODEL]))
    cg.add(var.set_endpoint(config[CONF_ENDPOINT]))
    cg.add(var.set_system_prompt(config[CONF_SYSTEM_PROMPT]))
    cg.add(var.set_voice(config[CONF_VOICE]))
    if language := config.get(CONF_LANGUAGE):
        cg.add(var.set_language(language))
    cg.add(var.set_noise_suppression_level(config[CONF_NOISE_SUPPRESSION_LEVEL]))
    cg.add(var.set_auto_gain(config[CONF_AUTO_GAIN]))
    cg.add(var.set_volume_multiplier(config[CONF_VOLUME_MULTIPLIER]))

    if tools := config.get(CONF_TOOLS):
        import json
        # Convert auth_token (from !secret) into the MCP "authorization" field.
        # Per OpenAI Realtime API docs, MCP tools use "authorization" (a string),
        # not "headers.Authorization". Sending both is a common failure.
        for tool in tools:
            if "auth_token" in tool:
                token = tool.pop("auth_token")
                tool["authorization"] = "Bearer " + token
        tools_json = json.dumps(tools)
    elif tools_file := config.get(CONF_TOOLS_FILE):
        with open(str(tools_file)) as f:
            tools_json = f.read()
    else:
        tools_json = None

    if tools_json:
        # Write tools JSON to a generated header file in src/ (which is in the
        # include search path). Using a raw string literal R"JSON(...)" preserves
        # the content verbatim — passing a 15KB+ string through cg.add() corrupts
        # it because codegen string escaping can't handle hundreds of embedded
        # quotes and escape sequences.
        from esphome.core import CORE as _core
        from esphome.helpers import write_file_if_changed
        header_path = _core.relative_build_path("src", "openai_assistant_tools.h")
        header_content = (
            '#pragma once\n'
            'namespace esphome::openai_assistant {\n'
            'static const char *const TOOLS_JSON = R"JSON('
            + tools_json +
            ')JSON";\n'
            '}  // namespace esphome::openai_assistant\n'
        )
        write_file_if_changed(header_path, header_content)
        cg.add_define("USE_OPENAI_ASSISTANT_TOOLS")
        cg.add(var.set_has_tools(True))

    if text_request_id := config.get(CONF_TEXT_REQUEST):
        sens = await cg.get_variable(text_request_id)
        cg.add(var.set_text_request_sensor(sens))

    if text_response_id := config.get(CONF_TEXT_RESPONSE):
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
        (CONF_ON_STT_VAD_START, "add_on_stt_vad_start_callback", []),
        (CONF_ON_STT_VAD_END, "add_on_stt_vad_end_callback", []),
        (CONF_ON_TOOL_START, "add_on_tool_start_callback", []),
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
        (CONF_ON_CLIENT_CONNECTED, "add_on_client_connected_callback", []),
        (CONF_ON_CLIENT_DISCONNECTED, "add_on_client_disconnected_callback", []),
    )
    for conf_key, method, args in callback_entries:
        if (conf := config.get(conf_key)) is not None:
            await automation.build_callback_automation(var, method, args, conf)

    cg.add_define("USE_OPENAI_ASSISTANT")
    cg.add_define("USE_OPENAI_COMMON")
    esp32.add_idf_component(name="espressif/esp_websocket_client", ref="1.4.0")


OPENAI_ASSISTANT_ACTION_SCHEMA = cv.Schema(
    {cv.GenerateID(): cv.use_id(OpenAIAssistant)}
)


@register_action(
    "openai_assistant.start",
    StartAction,
    OPENAI_ASSISTANT_ACTION_SCHEMA.extend(
        {
            cv.Optional(CONF_SILENCE_DETECTION, default=True): cv.boolean,
            cv.Optional(CONF_WAKE_WORD): cv.templatable(cv.string),
        }
    ),
    synchronous=True,
)
async def openai_assistant_start_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_silence_detection(config[CONF_SILENCE_DETECTION]))
    if wake_word := config.get(CONF_WAKE_WORD):
        templ = await cg.templatable(wake_word, args, cg.std_string)
        cg.add(var.set_wake_word(templ))
    return var


@register_action(
    "openai_assistant.stop",
    StopAction,
    OPENAI_ASSISTANT_ACTION_SCHEMA,
    synchronous=True,
)
async def openai_assistant_stop_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@register_condition(
    "openai_assistant.is_running", IsRunningCondition, OPENAI_ASSISTANT_ACTION_SCHEMA
)
async def openai_assistant_is_running_to_code(config, condition_id, template_arg, args):
    var = cg.new_Pvariable(condition_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@register_condition(
    "openai_assistant.connected", ConnectedCondition, OPENAI_ASSISTANT_ACTION_SCHEMA
)
async def openai_assistant_connected_to_code(
    config, condition_id, template_arg, args
):
    var = cg.new_Pvariable(condition_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


# --- Generic action aliases (work regardless of which component is loaded) ---
_GENERIC_ACTIONS = register_generic_openai_actions(
    OpenAIAssistant, OPENAI_ASSISTANT_ACTION_SCHEMA, StartAction, StopAction, IsRunningCondition
)
