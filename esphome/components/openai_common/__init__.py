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
import esphome.config_validation as cv
from esphome import automation
from esphome.automation import register_action, register_condition
from esphome.const import CONF_ID

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

CONF_SILENCE_DETECTION = "silence_detection"
CONF_WAKE_WORD = "wake_word"


def register_generic_openai_actions(
    component_cls, action_schema, start_action_cls, stop_action_cls, is_running_cond_cls
):
    """Register ``openai.start``, ``openai.stop`` and ``openai.is_running``.

    Each component calls this once from its own ``__init__.py`` to register
    the generic action/condition aliases that ``common.yaml`` uses. Only one
    component is loaded at a time (via ``!include``), so there is no conflict.

    Returns nothing — the ``@register_action`` / ``@register_condition``
    decorators populate the global registry as a side effect of import.
    """
    _generic_start_schema = action_schema.extend(
        {
            cv.Optional(CONF_SILENCE_DETECTION, default=True): cv.boolean,
            cv.Optional(CONF_WAKE_WORD): cv.templatable(cv.string),
        }
    )

    @register_action(
        "openai.start",
        start_action_cls,
        _generic_start_schema,
        synchronous=True,
    )
    async def openai_start_to_code(config, action_id, template_arg, args):
        var = cg.new_Pvariable(action_id, template_arg)
        await cg.register_parented(var, config[CONF_ID])
        cg.add(var.set_silence_detection(config[CONF_SILENCE_DETECTION]))
        if (wake_word := config.get(CONF_WAKE_WORD)) is not None:
            templ = await cg.templatable(wake_word, args, cg.std_string)
            cg.add(var.set_wake_word(templ))
        return var

    @register_action(
        "openai.stop",
        stop_action_cls,
        action_schema,
        synchronous=True,
    )
    async def openai_stop_to_code(config, action_id, template_arg, args):
        var = cg.new_Pvariable(action_id, template_arg)
        await cg.register_parented(var, config[CONF_ID])
        return var

    @register_condition(
        "openai.is_running",
        is_running_cond_cls,
        action_schema,
    )
    async def openai_is_running_to_code(config, condition_id, template_arg, args):
        var = cg.new_Pvariable(condition_id, template_arg)
        await cg.register_parented(var, config[CONF_ID])
        return var
