# openai_responses — known issues / TODO

## Switch to server-side MCP via LocalAI metadata.mcp_servers

### Background
The responses component currently uses **client-side** MCP tool execution:
- Sends tools as `type:"function"` (client-executed) in the request body
- Prefetches tool definitions via `tools/list` on wifi connect
- When the LLM returns a `function_call`, the ESP calls the MCP server's
  `tools/call` endpoint itself
- Sends `function_call_output` back in a round-2 request with
  `previous_response_id`
- ~1000 lines of client-side MCP state machine code (McpPhase enum,
  initialize/initialized/tools_list/tools_call states, session management,
  routing)

### The problem
This approach has several issues:
1. **Two network round-trips** (ESP → MCP server → ESP → LLM → ESP) instead
   of one (LLM → MCP server → LLM, all server-side)
2. **Requires `previous_response_id` + `store:true`** to work — if the
   endpoint doesn't properly support it, the system prompt is lost on round 2
   (the round-2 request deliberately omits `instructions` per the spec,
   expecting the server to restore them from the stored response)
3. **System prompt loss after tool calls** — confirmed in endpoint logs: the
   second response (round 2) does not contain the system prompt. If the
   endpoint silently ignores `previous_response_id`, the LLM has no
   instructions on round 2, leading to degraded behavior (e.g., MCP tools
   passed to TTS instead of called properly)
4. **Two separate response IDs per turn** — this is spec-correct (each round
   is a separate response), but it means the server must support stateful
   response storage

### The solution: LocalAI server-side MCP via metadata
Per LocalAI's documentation, all API endpoints (including `/v1/responses`)
support MCP server selection through the standard `metadata` field:

```bash
curl http://localhost:8080/v1/responses \
  -H "Content-Type: application/json" \
  -d '{
    "model": "my-mcp-model",
    "input": "What is the weather in New York?",
    "metadata": {"mcp_servers": "weather-api"}
  }'
```

When `metadata.mcp_servers` is present:
- Only the named MCP servers are activated for this request
- Server names must match the keys in the model's MCP config YAML
- The `mcp_servers` key is consumed by the MCP engine and stripped before
  reaching the backend
- The server connects to the MCP servers, imports tools, executes tool calls
  itself, and returns the final text response — all in a single response

For Open Responses specifically: if the model has MCP config and no
user-provided tools, all MCP servers are auto-activated (backward compatible).
So we may not even need to send `metadata.mcp_servers` if the model is
configured with MCP servers server-side.

### What the refactor would look like
- **Remove** the entire client-side MCP state machine:
  - `McpPhase` enum, `McpCallType` enum
  - `fetch_tools_()`, `process_mcp_response_()`, `extract_mcp_json_()`
  - `build_cached_tools_json_()` (converts MCP tools → function tools)
  - `start_mcp_tool_call_()`, MCP HTTP request building/response handling
  - `mcp_servers_` config vector and all per-server session state
  - `prefetch_tools()` (the `wifi.on_connect` no-op stub can stay)
  - States: `FETCHING_TOOLS`, `EXECUTING_TOOLS`, `READING_MCP`
  - `tools_file`, `tools_cache_ttl`, `mcp_servers` config keys
- **Add** `metadata.mcp_servers` to the request body (comma-separated
  server names from the YAML config, or omit if relying on auto-activation)
- **Remove** the round-2 `previous_response_id` path for tool calls — the
  server handles everything in one response, so there's no round 2
- **Keep** `function_call` handling only if we want to support non-LocalAI
  endpoints that require client-side execution (could be a compile-time flag)
- The `mcp_servers` YAML config would change from `{name, url, api_key}` to
  just `{name}` (the server name matching the model's MCP config), since the
  server handles the URL/auth

### Benefits
- Eliminates ~1000 lines of client-side MCP code
- Eliminates the two-response round-trip (single response per turn)
- Eliminates the system prompt loss issue (no round 2)
- Matches how the realtime component already works (server-side MCP)
- Faster tool execution (server-to-server, no ESP in the middle)
- Simpler mental model: one request → one response

### Risks / open questions
- **LocalAI-specific**: `metadata.mcp_servers` is a LocalAI extension, not
  part of the OpenAI Responses API spec. The standard OpenAI approach is
  `type:"mcp"` tools in the request body (which realtime uses). Need to
  decide: support both? Make it config-selectable?
- **Tool name visibility**: with client-side execution, the ESP knows which
  tools are available (from `tools/list`). With server-side, the ESP doesn't
  see the tool list — it only sees the final text response. The `on_tool_start`
  automation may need to rely on SSE events like `mcp_list_tools` or
  `mcp_call` output items instead of the ESP's own tool routing.
- **MCP server config**: the model's MCP config YAML lives on the server,
  not the ESP. The ESP would only send server names, not URLs/auth. This
  changes the deployment model — MCP server URLs/tokens move from
  `secrets.yaml` to the LocalAI model config.

## `on_tool_start` automation not firing during tool execution

During testing, the "Searching..." display page (driven by `on_tool_start`)
was not shown on the responses component, while it consistently appears on
the conversations component. The conversations component fires
`on_tool_start` during its tool execution state (state 13 → 11), but the
responses component's tool execution path (through `EXECUTING_TOOLS` /
`READING_MCP` states) may not be firing the callback at the right time (or
at all). Needs investigation — the `on_tool_start` callback should fire when
the LLM requests a tool call, before the ESP begins executing it.
