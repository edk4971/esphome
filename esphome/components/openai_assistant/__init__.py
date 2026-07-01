from esphome import automation
from esphome.automation import register_action, register_condition
import esphome.codegen as cg
from esphome.components import (
    esp32,
    media_player,
    micro_wake_word,
    microphone,
    speaker,
    text_sensor,
)
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_MEDIA_PLAYER,
    CONF_MICROPHONE,
    CONF_MODEL,
    CONF_ON_CLIENT_CONNECTED,
    CONF_ON_CLIENT_DISCONNECTED,
    CONF_ON_ERROR,
    CONF_ON_IDLE,
    CONF_ON_START,
    CONF_SPEAKER,
)

AUTO_LOAD = ["audio", "json", "ring_buffer"]
DEPENDENCIES = ["microphone", "network"]

CODEOWNERS = ["@edk4971"]

CONF_API_KEY = "api_key"
CONF_ENDPOINT = "endpoint"
CONF_SYSTEM_PROMPT = "system_prompt"
CONF_TEXT_REQUEST = "text_request"
CONF_TEXT_RESPONSE = "text_response"
CONF_VOICE = "voice"
CONF_LANGUAGE = "language"
CONF_ON_END = "on_end"
CONF_ON_LISTENING = "on_listening"
CONF_ON_STT_END = "on_stt_end"
CONF_ON_STT_VAD_END = "on_stt_vad_end"
CONF_ON_STT_VAD_START = "on_stt_vad_start"
CONF_ON_TTS_END = "on_tts_end"
CONF_ON_TTS_START = "on_tts_start"
CONF_ON_TTS_STREAM_END = "on_tts_stream_end"
CONF_ON_TTS_STREAM_START = "on_tts_stream_start"
CONF_ON_WAKE_WORD_DETECTED = "on_wake_word_detected"
CONF_USE_WAKE_WORD = "use_wake_word"
CONF_SILENCE_DETECTION = "silence_detection"
CONF_AUTO_GAIN = "auto_gain"
CONF_MICRO_WAKE_WORD = "micro_wake_word"
CONF_NOISE_SUPPRESSION_LEVEL = "noise_suppression_level"
CONF_VOLUME_MULTIPLIER = "volume_multiplier"
CONF_WAKE_WORD = "wake_word"
CONF_ON_TIMER_STARTED = "on_timer_started"
CONF_ON_TIMER_UPDATED = "on_timer_updated"
CONF_ON_TIMER_CANCELLED = "on_timer_cancelled"
CONF_ON_TIMER_FINISHED = "on_timer_finished"
CONF_ON_TIMER_TICK = "on_timer_tick"

openai_assistant_ns = cg.esphome_ns.namespace("openai_assistant")
OpenAIAssistant = openai_assistant_ns.class_("OpenAIAssistant", cg.Component)

StartAction = openai_assistant_ns.class_(
    "StartAction", automation.Action, cg.Parented.template(OpenAIAssistant)
)
StartContinuousAction = openai_assistant_ns.class_(
    "StartContinuousAction", automation.Action, cg.Parented.template(OpenAIAssistant)
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
Timer = openai_assistant_ns.struct("Timer")


def _websocket_endpoint(value):
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
            cv.Optional(CONF_VOICE, default="alloy"): cv.string,
            cv.Optional(CONF_LANGUAGE): cv.All(cv.string, cv.Length(min=2, max=8)),
            cv.Required(CONF_ENDPOINT): _websocket_endpoint,
            cv.Optional(
                CONF_SYSTEM_PROMPT, default="You are a helpful voice assistant."
            ): cv.string,
            cv.Optional(CONF_MICROPHONE, default={}): microphone.microphone_source_schema(
                min_bits_per_sample=16,
                max_bits_per_sample=16,
                min_channels=1,
                max_channels=1,
            ),
            cv.Exclusive(CONF_MEDIA_PLAYER, "output"): cv.use_id(
                media_player.MediaPlayer
            ),
            cv.Exclusive(CONF_SPEAKER, "output"): cv.use_id(speaker.Speaker),
            cv.Optional(CONF_TEXT_REQUEST): cv.use_id(text_sensor.TextSensor),
            cv.Optional(CONF_TEXT_RESPONSE): cv.use_id(text_sensor.TextSensor),
            cv.Optional(CONF_USE_WAKE_WORD, default=False): cv.boolean,
            cv.Optional(CONF_MICRO_WAKE_WORD): cv.use_id(micro_wake_word.MicroWakeWord),
            cv.Optional(CONF_NOISE_SUPPRESSION_LEVEL, default=0): cv.int_range(0, 4),
            cv.Optional(CONF_AUTO_GAIN, default="0dBFS"): cv.All(
                cv.float_with_unit("decibel full scale", "(dBFS|dbfs|DBFS)"),
                cv.int_range(0, 31),
            ),
            cv.Optional(CONF_VOLUME_MULTIPLIER, default=1.0): cv.float_range(
                min=0.0, min_included=False
            ),
            cv.Optional(CONF_ON_LISTENING): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_START): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_WAKE_WORD_DETECTED): automation.validate_automation(
                single=True
            ),
            cv.Optional(CONF_ON_STT_END): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_STT_VAD_START): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_STT_VAD_END): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_TTS_START): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_TTS_END): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_TTS_STREAM_START): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_TTS_STREAM_END): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_END): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_ERROR): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_IDLE): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_CLIENT_CONNECTED): automation.validate_automation(
                single=True
            ),
            cv.Optional(CONF_ON_CLIENT_DISCONNECTED): automation.validate_automation(
                single=True
            ),
            cv.Optional(CONF_ON_TIMER_STARTED): automation.validate_automation(
                single=True
            ),
            cv.Optional(CONF_ON_TIMER_UPDATED): automation.validate_automation(
                single=True
            ),
            cv.Optional(CONF_ON_TIMER_CANCELLED): automation.validate_automation(
                single=True
            ),
            cv.Optional(CONF_ON_TIMER_FINISHED): automation.validate_automation(
                single=True
            ),
            cv.Optional(CONF_ON_TIMER_TICK): automation.validate_automation(single=True),
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

    if spkr_id := config.get(CONF_SPEAKER):
        spkr = await cg.get_variable(spkr_id)
        cg.add(var.set_speaker(spkr))

    if media_player_id := config.get(CONF_MEDIA_PLAYER):
        mp = await cg.get_variable(media_player_id)
        cg.add(var.set_media_player(mp))

    if mww_id := config.get(CONF_MICRO_WAKE_WORD):
        mww = await cg.get_variable(mww_id)
        cg.add(var.set_micro_wake_word(mww))

    if text_request_id := config.get(CONF_TEXT_REQUEST):
        sens = await cg.get_variable(text_request_id)
        cg.add(var.set_text_request_sensor(sens))

    if text_response_id := config.get(CONF_TEXT_RESPONSE):
        sens = await cg.get_variable(text_response_id)
        cg.add(var.set_text_response_sensor(sens))

    cg.add(var.set_api_key(config[CONF_API_KEY]))
    cg.add(var.set_model(config[CONF_MODEL]))
    cg.add(var.set_voice(config[CONF_VOICE]))
    if language := config.get(CONF_LANGUAGE):
        cg.add(var.set_language(language))
    cg.add(var.set_endpoint(config[CONF_ENDPOINT]))
    cg.add(var.set_system_prompt(config[CONF_SYSTEM_PROMPT]))
    cg.add(var.set_use_wake_word(config[CONF_USE_WAKE_WORD]))
    cg.add(var.set_noise_suppression_level(config[CONF_NOISE_SUPPRESSION_LEVEL]))
    cg.add(var.set_auto_gain(config[CONF_AUTO_GAIN]))
    cg.add(var.set_volume_multiplier(config[CONF_VOLUME_MULTIPLIER]))

    if CONF_ON_LISTENING in config:
        await automation.build_automation(
            var.get_listening_trigger(), [], config[CONF_ON_LISTENING]
        )
    if CONF_ON_START in config:
        await automation.build_automation(var.get_start_trigger(), [], config[CONF_ON_START])
    if CONF_ON_WAKE_WORD_DETECTED in config:
        await automation.build_automation(
            var.get_wake_word_detected_trigger(), [], config[CONF_ON_WAKE_WORD_DETECTED]
        )
    if CONF_ON_STT_END in config:
        await automation.build_automation(
            var.get_stt_end_trigger(), [(cg.std_string, "x")], config[CONF_ON_STT_END]
        )
    if CONF_ON_STT_VAD_START in config:
        await automation.build_automation(
            var.get_stt_vad_start_trigger(), [], config[CONF_ON_STT_VAD_START]
        )
    if CONF_ON_STT_VAD_END in config:
        await automation.build_automation(
            var.get_stt_vad_end_trigger(), [], config[CONF_ON_STT_VAD_END]
        )
    if CONF_ON_TTS_START in config:
        await automation.build_automation(
            var.get_tts_start_trigger(),
            [(cg.std_string, "x")],
            config[CONF_ON_TTS_START],
        )
    if CONF_ON_TTS_END in config:
        await automation.build_automation(
            var.get_tts_end_trigger(), [(cg.std_string, "x")], config[CONF_ON_TTS_END]
        )
    if CONF_ON_TTS_STREAM_START in config:
        await automation.build_automation(
            var.get_tts_stream_start_trigger(), [], config[CONF_ON_TTS_STREAM_START]
        )
    if CONF_ON_TTS_STREAM_END in config:
        await automation.build_automation(
            var.get_tts_stream_end_trigger(), [], config[CONF_ON_TTS_STREAM_END]
        )
    if CONF_ON_END in config:
        await automation.build_automation(var.get_end_trigger(), [], config[CONF_ON_END])
    if CONF_ON_ERROR in config:
        await automation.build_automation(
            var.get_error_trigger(),
            [(cg.std_string, "code"), (cg.std_string, "message")],
            config[CONF_ON_ERROR],
        )
    if CONF_ON_IDLE in config:
        await automation.build_automation(var.get_idle_trigger(), [], config[CONF_ON_IDLE])
    if CONF_ON_CLIENT_CONNECTED in config:
        await automation.build_automation(
            var.get_client_connected_trigger(), [], config[CONF_ON_CLIENT_CONNECTED]
        )
    if CONF_ON_CLIENT_DISCONNECTED in config:
        await automation.build_automation(
            var.get_client_disconnected_trigger(), [], config[CONF_ON_CLIENT_DISCONNECTED]
        )
    if on_timer_started := config.get(CONF_ON_TIMER_STARTED):
        await automation.build_automation(
            var.get_timer_started_trigger(), [(Timer, "timer")], on_timer_started
        )
    if on_timer_updated := config.get(CONF_ON_TIMER_UPDATED):
        await automation.build_automation(
            var.get_timer_updated_trigger(), [(Timer, "timer")], on_timer_updated
        )
    if on_timer_cancelled := config.get(CONF_ON_TIMER_CANCELLED):
        await automation.build_automation(
            var.get_timer_cancelled_trigger(), [(Timer, "timer")], on_timer_cancelled
        )
    if on_timer_finished := config.get(CONF_ON_TIMER_FINISHED):
        await automation.build_automation(
            var.get_timer_finished_trigger(), [(Timer, "timer")], on_timer_finished
        )
    if on_timer_tick := config.get(CONF_ON_TIMER_TICK):
        await automation.build_automation(
            var.get_timer_tick_trigger(),
            [(cg.std_vector.template(Timer).operator("const").operator("ref"), "timers")],
            on_timer_tick,
        )

    cg.add_define("USE_OPENAI_ASSISTANT")
    esp32.add_idf_component(name="espressif/esp_websocket_client", ref="1.4.0")


OPENAI_ASSISTANT_ACTION_SCHEMA = cv.Schema(
    {cv.GenerateID(): cv.use_id(OpenAIAssistant)}
)


@register_action(
    "openai_assistant.start_continuous",
    StartContinuousAction,
    OPENAI_ASSISTANT_ACTION_SCHEMA,
    synchronous=True,
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
    if CONF_SILENCE_DETECTION in config:
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
async def openai_assistant_connected_to_code(config, condition_id, template_arg, args):
    var = cg.new_Pvariable(condition_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var
