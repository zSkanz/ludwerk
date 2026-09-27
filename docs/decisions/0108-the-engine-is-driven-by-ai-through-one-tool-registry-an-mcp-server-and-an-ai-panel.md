# 0108 — The engine is driven by AI through one tool registry, an MCP server and an AI panel

- Status: accepted (to be built after the ledgers of ADRs 0104 to 0107; see
  `docs/briefs/ai-kickoff.md`)
- Date: 2026-09-26
- Decided by: the owner, on 2026-09-26, in conversation: *"quero que nossa engine
  tenha uma forte integração com ia então precisamos ter um mcp bem completo
  capaz de fazer muita coisa nela eu diria até que ele poderia agir como
  autonomo fazendo tudo ... também quero uma aba focada em ai saca algo bem
  profissional e completo"*. It is editor work (post-v1 phase 1, open), and he
  opened the AI part of it here.
- Relates to: [0035](0035-engine-is-a-websocket-client-of-the-dev-server.md)
  (the dev control channel this builds on and secures),
  [0063](0063-https-stays-refused-and-tls-comes-from-the-platform.md) (HTTPS comes from the platform), ADRs 0104 to 0107
  (everything they add becomes tools).

## Context

### What the engine already has (surveyed 2026-09-26)

- **A control channel**: `luaug dev` runs a WebSocket server (Lute); the engine
  dials it and takes `reload`, `sample` (the world hash at a tick), `ping`,
  `shutdown` and `asset-changed`, drained at the frame's safe point. `eval` is
  reserved and unimplemented. **Only the engine authenticates**: any other
  client joins as an observer, sees every reply and can forward any command,
  `shutdown` included; the token comes from `math.random`.
- **Edits have one door**: the editor's `EditorCommands` (create, delete,
  reparent, rename, paste, save, play, undo...) is plain data drained at the
  safe point. Undo is whole-world snapshots with labels and coalescing, 64 deep.
  There is no action registry or command palette.
- **Observation exists in pieces**: headless runs, `--screenshot`, conformance
  specs with a JSON report, the world hash, the log sink (chainable) and the
  console's 400-line ring, `DebugService.MessageOut`, a Luau REPL
  (`ScriptRuntime::evaluate`), and an in-process language service (Luau
  analysis, new solver) with diagnostics, completion and signatures.
- **The API is machine-readable**: `api/api-dump.json` from the IDL, and a
  markdown page per class; the dump has no descriptions yet.
- **No HTTP server, no TLS** in C++ (`engine/net` is a plain-HTTP client, a
  WebSocket client, a TCP client). Lute serves WebSockets.

### The protocol, and what others built (researched 2026-09-26)

- **MCP 2026-07-28** is current and breaking. It has a stateless core:
  `server/discover` instead of `initialize`, per-request `_meta`, and state
  passed as server-minted handles in tool arguments. Its transports are stdio
  and Streamable HTTP, with HTTP+SSE deprecated. Other changes:
  - multi-round-trip requests for elicitation;
  - progress and cancellation are kept;
  - **tasks** for long operations, now an official extension;
  - `subscriptions/listen`;
  - roots, sampling and logging are deprecated;
  - tools have `outputSchema` and `structuredContent`, image content,
    annotations (read-only, destructive, idempotent) and a deterministic list
    order.
- **Tool design that works** (Anthropic's guidance, and the engines' servers):
  - **Tools:**
    - a small core of named verbs, plus a per-domain `manage(op, ...)`;
    - a `batch` that lands as one undo step;
    - an `execute code` escape hatch with a context selector;
    - screenshots returned as images, with camera control;
    - console reads filtered and scoped to a play session;
    - input simulation, so an agent can actually playtest;
    - an explicit instance id when several editors are open;
    - tools a project registers itself.
  - **Output:** concise output by default, pagination, readable names instead of
    raw ids, and errors the model can act on.
  - **Trust:** approve a client on first connection, bind to loopback, and use
    tokens between hops.
- **How it fails**, from the same servers' issue trackers:
  - the bridge dies when the editor reloads;
  - "entered play mode" is reported when play was silently cancelled;
  - blocking calls time out on long work;
  - hundreds of tool schemas, or a dumped scene tree, blow the context;
  - each call is its own undo step.
- **The in-editor assistants converge**, in the engines and in the AI IDEs:
  - **Modes:** Ask (read-only), Plan (an editable plan approved before
    anything changes) and Agent.
  - **Control:** approval-gated changes with *allow once / for this session /
    always*, a checkpoint on every message that restores the world and the
    files, an aggregated review of what changed, and stop and steer mid-run.
  - **Context:** chips and `@`-mentions (the selection, a script, the console's
    errors, a screenshot), a project rules file (`AGENTS.md`), thread history,
    a token and cost meter, and compaction.
  - **Models:** a provider and model picker with your own key or a local model.
  - **External agents:** a built-in MCP server, and hosting an outside agent
    (Claude Code, Codex, Gemini CLI) in the panel, through the Agent Client
    Protocol, which hands the editor's MCP server to the agent.

## Decision

### 1. One tool registry, three doors

**Every capability an AI has is a tool in one registry, in the engine.** Each
tool is registered once and reached three ways:

1. **The MCP server** (`luaug mcp`), for Claude Code, Cursor, Codex, Claude
   Desktop and any MCP client.
2. **The AI panel** in the editor (§5), whose own agent calls the same tools.
3. **An external agent hosted in the panel** over the Agent Client Protocol,
   which is handed the same MCP server.

A tool is a name, a title, a description, an input and an output JSON Schema,
annotations, and a handler. The handler runs **on the main thread at the
frame's safe point**, like every edit today, and returns structured content and
text; a screenshot also returns an image. The registry answers `tools/list` in
a fixed order. Nothing about a tool is written twice: the MCP server, the panel
and the documentation all read the registry.

### 2. The MCP server

- **`luaug mcp` speaks MCP over stdio**, at protocol 2026-07-28. It is started
  by the client, which is how every client expects a local server. Streamable
  HTTP on loopback comes later, for clients that cannot spawn a process.
- **It is the dev server grown up.** It runs the WebSocket server that editors
  and players dial, which is ADR 0035's shape. That keeps the engine a client,
  lets it survive a reload, and lets the bridge outlive any one engine. A
  reload or a crash of the engine is a reconnect, not a dead bridge: calls made
  while no engine is connected fail with an actionable error, and a reconnect
  replays nothing.
- **Several engines at once**: `engine_list` shows each connected editor or
  player (project, scene, state), and every tool takes an optional `engine`.
  With exactly one engine connected, it is implied.
- **An agent can work with no editor open**: `engine_launch` starts the player
  headless (or the editor, if asked) on a project and waits for it to connect.
  This is what makes autonomy possible from a terminal or a CI job.
- **Long work is a task**, per the tasks extension, with progress:
  - a play session, which is also a handle;
  - a test run;
  - an import;
  - an export;
  - a batch of hundreds of edits.

  Nothing blocks past a few seconds.
- **State is a handle**, per 2026-07-28: `play_start` returns a
  `play_session`, and later calls name it. An expired handle is an actionable
  error.
- **Setup is one command**: `luaug mcp install --client=claude-code|cursor|
  codex|claude-desktop` writes that client's configuration, and the Project
  Settings AI page shows the same command with a copy button.

### 3. The tools

A core of named verbs, grouped by domain. Everything else in a domain goes
through that domain's `*_manage(op, params)`, so the catalog stays around fifty
tools and not five hundred. Instances are always named by their **path**
(`Workspace.House.Door`), not by an id.

| Domain | Named tools |
|---|---|
| engine | `engine_list`, `engine_launch`, `engine_state` (project, scene, selection, play state, error count) |
| tree | `tree_view` (paged, depth, filter, concise or detailed), `instance_find` (name, class, tag, attribute, bounds), `instance_get`, `instance_create`, `instance_set`, `instance_delete`, `instance_move` (reparent), `instance_clone` |
| batch | `batch` (many operations, one undo step, all-or-nothing) |
| scripts | `script_read` (line ranges), `script_edit` (several edits in one call), `script_create`, `script_search` (capped), `script_check` (the language service's diagnostics) |
| code | `luau_execute(code, context = edit \| server \| client)`, the escape hatch |
| play | `play_start` (solo, host plus N clients -- ADR 0106 §5), `play_stop`, `play_step(ticks)`, `play_state` (checked, never assumed) |
| input | `input_action` (an input action's state), `input_keys`, `input_pointer`, `input_touch`, `character_move_to` |
| observe | `screenshot` (the viewport, a camera, or a path's instance framed; returns an image), `console_read` (since a cursor, by level, by play session), `world_hash`, `perf_read` (the F3 figures) |
| scene | `scene_open`, `scene_save`, `scene_new`, `scene_list` |
| assets | `asset_search`, `asset_import`, `material_manage`, `stamp_manage`, `terrain_manage`, `voxel_manage`, `ui_manage` |
| project | `project_settings` (get or set `luaug.toml` keys, never a secret), `check_run` (`luaug check`), `test_run` (the conformance runner, its JSON report), `export_run` (ADR 0104; always asks) |
| editor | `select`, `focus` (frame the viewport on a path), `undo`, `redo` |

- **Every change is undoable**: a tool call that changes the world is one undo
  step labelled with the tool, and a `batch` is one step. Files a tool writes are
  recorded for the checkpoint (§6).
- **Output is bounded**: concise by default, paginated with a cursor, and capped
  (25k tokens) with a note saying how to ask for more. Scene dumps are never
  whole by default.
- **Errors are for the model**: `isError` with what went wrong and what to do.
  For example: *"No instance at Workspace.Hosue.Door; Workspace.House has
  children Door1, Window. Use instance_find to search."*
- **Annotations are honest**: read-only tools say so, destructive ones say so,
  and the permission layer (§4) trusts its own table rather than the client.
- **A project adds its own tools**: a `ModuleScript` under `src/tools/` returns
  `{ Name, Description, Input, Run }`, and the registry exposes it. It runs in
  the edit context, like `luau_execute`.

**Resources and prompts:**

- **Resources**: `luaug://api/<Class>` (the reference page), `luaug://api/dump`,
  `luaug://manual/<page>`, `luaug://guide/agents` (a condensed guide written
  for agents: naming, strict mode, the three script services, common mistakes),
  and `luaug://project/rules` (the project's `AGENTS.md`). The IDL's `Doc`
  strings are added to `api-dump.json` for this.
- **Prompts**: *build a level*, *fix the errors*, *playtest this*, *explain this
  script*, *add multiplayer to this*.

### 4. Permissions, and what an agent may never do

- **Every client authenticates.** The channel ADR 0035 opened gets a token
  from the OS's secure random generator for every role, not only the engine's,
  and binds to loopback. An unauthenticated connection is refused, which closes
  today's observer hole. The first time an MCP client connects to an editor,
  the editor asks: *"Claude Code wants to control this editor: Allow once /
  Always for this project / Deny."*
- **Permission classes**, per tool, in the registry:

  | Class | Examples |
  |---|---|
  | read | tree, get, find, read, screenshot, console, state |
  | edit | create, set, delete, move, scripts, materials, terrain |
  | run | play, input, tests, `luau_execute` in a play session |
  | project | settings, scene save, import, new files |
  | outward | export, anything leaving the machine |

- **Modes** (the panel's selector; an MCP client picks one per connection in
  Project Settings):

  | Mode | Read | Edit, run, project | Outward |
  |---|---|---|---|
  | **Ask** | yes | no | no |
  | **Plan** | yes | only writing the plan | no |
  | **Agent** | yes | asks, or as allowed | asks |
  | **Autonomous** | yes | yes, inside the budget | asks, unless the owner allowed it for this run |

  Approvals are *once*, *for this session*, or *always for this project*,
  remembered per project in the user's editor state, never in the project.
- **Never, in any mode**: a file outside the project folder; a secret (a
  keystore password, an API key -- the permission layer refuses them and the
  tools never return them); git history rewrites or a push; deleting the
  project's `.git`; running a shell command. `luau_execute` runs in the engine's
  sandbox (R4), which already cannot reach the file system beyond what a script
  may.

### 5. The AI panel

A dock, **AI**, beside Explorer and Properties. It is the owner's "professional
and complete" tab, and it is judged against the best of what the survey found.

- **Threads**: a list, per project, with a title, age and cost; new, rename,
  delete, search. They are kept in the user's editor state for the project, not
  in the project folder.
- **The composer**:
  - **Mode**: *Ask / Plan / Agent / Autonomous*.
  - **Model**: providers and models, with favourites.
  - **Context chips**: the selection, the open script, the console's errors, a
    viewport screenshot, a scene. `@` mentions anything in the Explorer or the
    project's files, and dragging an instance from the Explorer attaches it.
    Images can be pasted.
- **The conversation**:
  - Messages stream, as markdown with code blocks.
  - Every tool call is a **card**, collapsed to one line (*"Created 12 parts
    under Workspace.Level"*). It expands to its input and result, and
    screenshots show as thumbnails.
  - A pending approval is a card with *Allow once / For this session / Always
    / Deny*, and the raw input visible.
- **Plan**: in Plan mode the agent writes an editable plan (markdown, a checklist)
  in a side view; *Approve and run* switches to Agent, and steps tick as they
  land.
- **Changes**: a review of everything this thread changed since a point:
  - scripts as diffs, with each hunk kept or reverted;
  - instances as a table of created, deleted and changed properties;
  - assets as a list.
- **Checkpoints**: every user message is one. *Restore* puts the world and the
  files back to how they were at that message, and keeps the conversation.
- **Control**: *Stop* at any time; typing while it runs **steers** (the text is
  delivered at the next step); a notification when a long run ends.
- **Meters**: tokens used and left in the context, the cost of the thread, and
  compaction when it fills (*Compact* on demand).
- **From the rest of the editor**: *Ask AI about this error* on a console line,
  *Explain* and *Fix* on a script selection, *Ask AI* on an Explorer selection.
- **Privacy**:
  - Nothing is sent anywhere until a provider is configured, and the first use
    says what will be sent.
  - No telemetry.
  - The key never leaves the machine, except to the provider it belongs to.

### 6. Checkpoints

- **The world**: the undo stack already snapshots it; a checkpoint is a named
  snapshot kept outside the 64-step history, per thread, until the thread is
  deleted.
- **Files**: every file a tool writes or deletes is copied first into
  `.luaug/ai/checkpoints/<thread>/<n>/` (ignored by git). Restore writes them
  back.
- A checkpoint is taken before the first change after a user message, so a
  message that changed nothing costs nothing.

### 7. Providers and keys

- **Anthropic's Messages API** (tool use, images, streaming) is the first
  provider, with Claude's current models as the default choice.
- **An OpenAI-compatible provider** with a configurable base URL covers OpenAI
  and the local servers (Ollama, LM Studio), which also speak Anthropic's shape.
  A local model works offline.
- **Keys live in the OS's credential store** (Windows Credential Manager, macOS
  Keychain, the Secret Service on Linux), with an environment variable as the
  fallback (`ANTHROPIC_API_KEY`, `OPENAI_API_KEY`). Never in `luaug.toml`,
  never in the project, never in a log, never in a tool result. *Reset
  credentials* removes them.
- **The agent loop runs in the `luaug mcp` process**, not in the editor:
  - a crash in it cannot take the editor down;
  - it already holds the tool channel;
  - HTTPS to a provider comes from its runtime rather than from a TLS stack in
    the engine.

  Whether Lute's HTTP client does HTTPS is checked first (stage A0). If it does
  not, the platform TLS of ADR 0063 is built for it. The panel is the loop's
  UI.
- **External agents over the Agent Client Protocol**: the panel's model picker
  also lists *Claude Code*, *Codex*, *Gemini CLI*, found on the machine. Picked,
  the agent runs as a subprocess, is handed `luaug mcp` as its MCP server, and
  its messages, tool calls, plans and permission requests render in the same
  panel. It runs on the user's own subscription, and nothing is proxied.

### 8. Autonomous mode

The owner's *"agir como autônomo fazendo tudo"*.

- **A goal, not a chat**: *"Make a three-level platformer with a boss"*. The
  agent plans, builds, plays the game through input simulation, reads the
  console and screenshots, fixes what it finds, and repeats.
- **It stops** when the goal is met, or on the first of these:
  - it hits its budget (tool calls, minutes, money -- all set before it starts,
    and shown);
  - the same error comes back three times;
  - it would need an `outward` permission it was not given;
  - the owner presses Stop.
- **Everything is reversible**: it checkpoints at every plan step, and ends with
  a report: what it built, what it tested, what failed, the changes review, and
  *Restore to before*.
- **From the command line**: `luaug ai run "<goal>" --project <dir> --budget
  ...` runs the same loop headless, which is how a night run or a CI job uses
  it.

### 9. Rules and knowledge

- **`AGENTS.md`** at the project root is the project's rules file: every agent
  reads it, the panel's agent included. `CLAUDE.md` is read too, if present.
  `luaug new` writes one from the template, with the engine's conventions: API
  naming, `--!strict`, the three script services, how to test.
- **`luaug://guide/agents`** ships with the engine and is kept current by the
  same generators as the reference.

## Consequences

- **The editor can be driven completely from outside**: a person in Claude Code
  or Cursor can build, test and fix a game without touching the editor, and the
  editor shows it happening.
- **The panel is the same power inside**, with the conveniences only an editor
  can give: context from the selection, approvals in place, changes reviewed
  visually, and restore.
- **The dev channel becomes authenticated** for everybody, which it should have
  been anyway.
- **Every future feature ships with its tools**: a new capability without tools
  is not done. The ledger for each future ADR carries a *tools* line.
- **A tool evaluation suite** (real tasks, run against the MCP server with a real
  model) measures whether the tools are good. It runs nightly or on demand, not
  in the gate: it costs money and is not deterministic.

### Rejected

- **An MCP server inside the editor process on HTTP**, as one engine ships
  it. That needs an HTTP server and TLS decisions in the engine, it dies with
  the editor, and it cannot launch an engine that is not running. The bridge
  process keeps the engine a client.
- **One tool per API member.** Hundreds of schemas blow the context of every
  conversation. Named verbs, `*_manage`, `luau_execute` and search keep it to
  about fifty.
- **The agent loop inside the editor.** A provider outage, a TLS problem or a
  runaway loop would be the editor's problem.
- **API keys in `luaug.toml` or the project.** A project is shared; a key is not.
- **Autonomy without a budget.** An agent that can loop for ever on someone's
  money is not a feature.
