# openai_realtime — known issues / TODO

## Audio language misdetected
Audio sent to the server is commonly identified as the wrong language (e.g.
Russian instead of English), causing Cyrillic responses and font codepoint
warnings. The session.update already sends
`"input_audio_transcription":{"language":"en"}`, but the server-side STT
may not respect it. When this happens, the audio in the endpoint log also
sounds an octave higher than expected.

## MCP tools passed to TTS instead of called by the backend
MCP tools declared in `session.update` with `type:"mcp"` are meant to be
called server-side (the backend connects to the MCP servers, imports tools,
and executes them). Instead they appear to be passed through to the TTS as
text, meaning the model describes the tool rather than invoking it.

## TTS streaming support
Piper TTS may not support the streaming format the Realtime API requires
("context cancelled" errors seen with piper). Chatterbox TTS
(`chatterbox-tts-crispasr`) appears to work as an alternative. Need to
confirm whether piper can handle streaming at all, or whether the config
should default to a streaming-capable TTS engine for realtime.
