# OpenAI Assistant Component
# 
## Overview
# This component provides a direct connection between an ESP32 device and the OpenAI Realtime API (or a compatible local endpoint), bypassing the need for a Home Assistant intermediary. Ideally, this is a drop-in replacement for voice_assistant, simply replace voice_assistant with openai_assistant in your yaml config, and add configs for: api_key, model, endpoint, and system_prompt.
#  
## Why this component?
#  
# The standard `voice_assistant` component in ESPHome relies on Home Assistant to proxy requests to OpenAI. This can introduce latency and overhead. The `openai_assistant` component allows the ESP32 to handle the interaction directly:
# - **Lower Latency**: Direct WebSocket connection for real-time audio streaming.
# - **Edge Independence**: Works without a local Home Assistant instance for the core voice interaction.
# - **Real-time Feedback**: Uses the OpenAI Realtime API to provide immediate transcription and audio responses.
#  
## Features
#  
# - **Realtime API Support**: Native integration with OpenAI's Realtime API via WebSockets.
# - **Local Endpoint Support**: Can be configured to point to a local inference server (e.g., LocalAI, vLLM).
# - **Local Wake Word**: Integrates with the `micro_wake_word` component for local trigger detection.
# - **Audio Streaming**: Handles 24kHz, 12-bit mono PCM audio for both input and output.
# - **Live Transcription**: Automatically updates `text_request` and `text_response` sensors for real-time UI feedback.
# - **Custom System Prompt**: Define a system-level prompt to set the assistant's personality and behavior.
#  
## Installation
#  
# Add this component as an external component in your ESPHome configuration:
#  
# ```yaml
# external_components:
#   - source: github.com/edk4971/openai_assistant  # Replace with actual repo if different
#     components: [ openai_assistant ]
#  
# openai_assistant:
#   api_key: "YOUR_OPENAI_API_KEY"
#   model: "gpt-4o"
#   endpoint: "wss://api.openai.com/v1/realtime"  # Or your local endpoint (e.g. ws://192.168.1.x:8000/v1/realtime)
#   instructions: "You are a helpful home automation assistant."
#   system_prompt: "You are a helpful assistant for a smart home. Be concise."
#   text_request: text_sensor.transcription
#   text_response: text_sensor.response
#   use_wake_word: true
#   micro_wake_word:
#     id: my_wake_word
#     # ... wake word configuration ...
#   microphone:
#     id: my_mic
#     # ... microphone configuration ...
#   media_player:
#     id: my_speaker
#     # ... media player configuration ...
# ```
#  
## Dependencies
#  
# - `microphone`
# - `media_player`
# - `micro_wake_word`
# - `text_sensor`
# - `network`
# - `socket`
# 
(End of file - total 61 lines)
