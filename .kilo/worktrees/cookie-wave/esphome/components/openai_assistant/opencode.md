# Project Summary: OpenAI Assistant Component for ESPHome

## Overview
This is an external component for ESPHome, `openai_assistant`, that enables an ESP32 device to communicate directly with a user-configurable OpenAI Realtime API. This bypasses the traditional Home Assistant interlocutor, reducing latency and enabling standalone voice interaction. This is designed to be used completely offline, on a local OpenAI endpoint, but should be compatible with any OpenAI API compatible endpoint. This is also designed as a drop-in replacement for the built-in ESPHome Voice Assistant component.

## Sources Used
- **ESPHome Core**: [Internal logic references.] (https://github.com/esphome/esphome/tree/dev/esphome/core)
    - `application.h` `log.h` `defines.h` `automation.h` `component.h` and `helpers.h`
- **ESPHome Components**: [Internal logic references.] https://github.com/esphome/esphome/tree/dev/esphome/components
    - `api` `audio` `ring_buffer` `microphone` `media_player` `micro_wake_word` `speaker` and `socket`
    - `voice_assistant`: as the exemplar; to implement `on_listening`, `on_tts_start`, `on_tts_end`, etc. and state machine logic. To be a true drop-in replacement `openai_assistant` needs to expose all the same methods so the user only needs to find/replace `voice_assist` with `openai_assist` in https://github.com/esphome/wake-word-voice-assistants/blob/main/esp32-s3-box-3/esp32-s3-box-3.yaml.
- **ESPHome start-components**: https://github.com/esphome/starter-components
    - Several exemplar external-components that demonstrate how to create an external_component
- **Espressif Demos**:
    - `esp-box` (`chatgpt_demo`): Verified the feasibility of direct API calls from ESP32. https://github.com/espressif/esp-box/tree/master/examples/chatgpt_demo
    - `esp-webrtc-solution` (`openai_demo`): Evaluated WebRTC vs. WebSockets for real-time streaming. https://github.com/espressif/esp-webrtc-solution/tree/main/solutions/openai_demo
    - LocalAI openai-realtime transports: https://localai.io/features/openai-realtime/#transports
    - ESPHome Code Standards: https://developers.esphome.io/contributing/code/ and https://github.com/esphome/esphome/blob/dev/AGENTS.md

## key differences
User must configure the following in their ESPHome config yaml:
    - api_key: "YOUR_OPENAI_API_KEY" # or "" if your endpoint does not require an API
    - model: "gpt-4o" #or any other realtime capable model
    - endpoint: "wss://api.openai.com/v1/realtime"  # Or your local endpoint (e.g. ws://192.168.1.x:8000/v1/realtime)
    - system_prompt: "You are a realtime voice assistant. Use any MCP tools you have access to. If you are given instructions to control devices in a house use the "homeassistant" MCP tools, execute the command, and respond quickly. If you are asked a general knowledge question, use the "openzim" MCP tools to research the answer, think, and respond with a concise summary. Only ask follow-up questions if you are not sure what to do. Limit your tool calls to a maximum of three per conversation"