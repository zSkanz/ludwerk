# AI: the kickoff and the ledger

The owner, on 2026-09-26, asked for a strong AI integration: a complete MCP
server that can do nearly anything in the engine, up to working autonomously,
and a professional, complete AI tab in the editor, researched against how the
best tools do it. The decision is
[ADR 0108](../decisions/0108-the-engine-is-driven-by-ai-through-one-tool-registry-an-mcp-server-and-an-ai-panel.md).
**Read it before this file** -- its Context carries the research (MCP
2026-07-28, the engines' MCP servers and their failure modes, the in-editor
assistants and AI IDEs). This file is the order of work and where each piece
stands.

> **Names after the rename.** This ledger runs after
> [`rename-kickoff.md`](rename-kickoff.md) (ADR 0109). Where it says `ludwerk`
> (the command), read the brand's command (`ludwerk`); `@engine/` is
> `@engine/`; `engine://` is `engine://`; `ENG_*` is `ENG_*`; `project.toml` is
> `project.toml`. The ADRs keep their original words; ADR 0109 is the mapping.

## When this starts

- After [`export-and-server-kickoff.md`](export-and-server-kickoff.md) (ADRs
  0104 to 0106) and [`views-kickoff.md`](views-kickoff.md) (ADR 0107), unless
  the owner reorders. The owner's words: when those are done, *"ela vai meter
  marcha nisso também"*.
- **Every tool that touches something those ledgers add is written against what
  they shipped**: the three script services, `SceneService`, `NetworkService`'s
  run-time calls and Play with N players (ADR 0106 §5), `CameraTexture`,
  `ViewportFrame` and `SubWorld`, and export. Read their Findings sections
  first.

The idea in brief:

- **One tool registry in the engine**, reached three ways: `ludwerk mcp` (stdio,
  MCP 2026-07-28), the editor's AI panel, and external agents hosted in the
  panel over the Agent Client Protocol.
- **About fifty tools**: named verbs plus a `*_manage` per domain, a `batch`
  with one undo step, `luau_execute`, screenshots, console reads, play sessions
  and input simulation.
- **Permissions** by class and mode (Ask, Plan, Agent, Autonomous), approvals
  in the editor, and a hard list of things never allowed.
- **The AI panel**: threads, modes, models, context chips, tool cards,
  approvals, a plan view, a changes review, checkpoints, steering and meters.
- **Autonomous runs** with a budget, from the panel or `ludwerk ai run`.

Legend: `[ ]` not started · `[~]` in progress · `[x]` done.

## What must hold at every stage

- `scripts/localgate.ps1` green, Linux and Android included, before every
  push; then CI read.
- **No determinism trace moves**: tools act through the same edit and play
  paths as a person, and nothing here runs in the simulation on its own.
- **No secret in a file, a log, or a tool result.** A test sets an API key and
  asserts it appears in no file under the project, the user's editor state, the
  log, or any tool's output. Keys only in the OS credential store or the
  environment.
- **The editor never waits on a model.** Provider calls happen in the `ludwerk
  mcp` process; the editor's frame time does not move with the panel open
  during a run (measure it, record in `docs/perf-baselines.md`).
- **The gate never calls a paid model.** Tests use a scripted fake provider;
  the real-model evaluation runs on demand and nightly, never in
  `localgate.ps1` or CI's required jobs.
- R3 (every panel string an i18n key), R4 (`luau_execute` sandboxed), R7 (no
  other engine named in code), R17, and the naming rules for anything exposed
  to Luau.

## Stage A0 — a secure channel and a tool registry

- [ ] **Authenticate every client** on the dev channel (ADR 0035): a token per
      session from the OS's secure random generator (not `math.random`),
      required for every role, loopback only. An unauthenticated or wrong-token
      connection is refused with a keyed reason. Test: an observer without the
      token cannot send `shutdown`.
- [ ] **Protocol 2** on the channel: request ids, replies matched to requests,
      progress messages, cancellation; `eval` implemented as `luau_execute`
      with a context (edit, server, client).
- [ ] **The tool registry** (C++, `engine/app` or a new `engine/tools` module
      if `checklayers` prefers): name, title, description, input and output
      schema, annotations, permission class, handler; run at the frame's safe
      point on the main thread; `tools/list` in a fixed order. The channel
      exposes `tools/list` and `tools/call`.
- [ ] **Undo per call**: a tool call that changes the world is one undo step
      labelled with the tool; nested calls inside `batch` coalesce into it.
- [ ] **Check Lute for HTTPS** (`@lute/net` request to an `https://` URL). If it
      works, note it in Findings; if not, stage A4 starts with ADR 0063's
      platform TLS.
- [ ] The first tools, to prove the path end to end: `engine_state`,
      `tree_view`, `instance_get`, `instance_set`, `luau_execute`.

## Stage A1 — `ludwerk mcp`

- [ ] `tools/cli/commands/mcp.luau`: MCP 2026-07-28 over stdio --
      `server/discover`, tools (list, call, `outputSchema`,
      `structuredContent`, image content, annotations), resources and
      templates, prompts, progress, cancellation, and the tasks extension for
      long work. It runs the WebSocket server engines dial (replacing `ludwerk
      dev`'s role when both are wanted; `ludwerk dev` keeps working alone).
- [ ] Several engines: `engine_list`, an optional `engine` argument on every
      tool, implied when there is one. Reconnect on an engine's reload or
      restart; a call with no engine fails with an actionable message.
- [ ] `engine_launch`: start the player headless or the editor on a project,
      wait for it to connect, return its id.
- [ ] **Handles**: `play_session` and the like, minted by the server, with
      their lifetime in the tool description and an actionable error on expiry.
- [ ] `ludwerk mcp install --client=claude-code|cursor|codex|claude-desktop`
      writes that client's configuration; Project Settings > AI shows the
      command with a copy button, and which clients are connected.
- [ ] First connection approval in the editor: *Allow once / Always for this
      project / Deny*, remembered in the user's editor state.
- [ ] Tests: a scripted MCP client (in Luau, in `tests/mcp/`) discovers the
      server, lists tools in a stable order, calls one against a headless
      engine, runs a task to completion with progress, cancels one, and is
      refused without the token.

## Stage A2 — the catalog

- [ ] Every tool in ADR 0108 §3's table, with honest annotations and a
      permission class. Paths, not ids. Concise by default, `detail` on
      request, cursors for pages, a 25k-token cap with a note.
- [ ] `batch`: all-or-nothing, one undo step, a per-operation result list.
- [ ] `play_*`: states are checked, not assumed -- `play_start` returns only
      once play has actually begun, or an error saying why it did not. Play
      with N players uses ADR 0106 §5.
- [ ] `input_*` and `character_move_to` drive the game as a player would
      (virtual input states and the navigation service's agents).
- [ ] `screenshot` returns an image: the viewport, a named camera, or a path's
      instance framed; size capped; also a `CameraTexture` view (ADR 0107).
- [ ] `console_read`: a cursor, levels, and a play session's lines only.
- [ ] The `*_manage` tools: materials, stamps, terrain, voxels, UI -- each
      op documented in the tool's description with an example.
- [ ] Project tools: `ModuleScript`s under `src/tools/` returning `{ Name,
      Description, Input, Run }`, listed after the engine's own.
- [ ] Tests: one per tool, against a headless engine and a fixture project; a
      golden `tools/list` (the catalog is an API: a change to it is reviewed).

## Stage A3 — knowledge

- [ ] The IDL's `Doc` strings into `api-dump.json` (the generator), so
      `engine://api/<Class>` and `engine://api/dump` carry descriptions.
- [ ] `engine://manual/<page>` from `docs/manual`.
- [ ] `docs/agents.md`, served as `engine://guide/agents`: the engine for an
      agent in under 3,000 words -- API naming, `--!strict`, the three script
      services, scenes, input actions, common mistakes and their fixes. Kept
      current by a docs-lint check that every class it names exists.
- [ ] `ludwerk new` writes `AGENTS.md` from the template; the agent reads
      `AGENTS.md` and `CLAUDE.md`.
- [ ] Prompts: *build a level*, *fix the errors*, *playtest this*, *explain
      this script*, *add multiplayer to this*.

## Stage A4 — the agent loop and providers

- [ ] The loop in `ludwerk mcp`: messages, tool use, streaming, images, parallel
      tool calls, compaction when the context fills; one loop for every mode.
- [ ] Providers: Anthropic Messages API (default: Claude's current models);
      OpenAI-compatible with a base URL (OpenAI, Ollama, LM Studio). *Test
      connection* in settings.
- [ ] Keys in the OS credential store (Credential Manager / Keychain / Secret
      Service) with the environment as fallback; *Reset credentials*. The
      secret test from "What must hold".
- [ ] The permission layer: classes, modes, approvals (once / session /
      always-for-project), the never-list of ADR 0108 §4 enforced here and
      tested one by one.
- [ ] A fake provider for tests that replays a scripted conversation, so the
      loop, approvals and checkpoints are tested with no network.

## Stage A5 — the AI panel

- [ ] The dock, **AI**: threads (list, search, rename, delete, cost), the
      composer (mode, model, context chips, `@` mentions, drag from the
      Explorer, pasted images), streaming markdown, tool cards (one line,
      expandable, screenshot thumbnails), approval cards with the raw input.
- [ ] Plan mode: the plan as an editable markdown checklist in a side view;
      *Approve and run*; steps tick as they land.
- [ ] Changes review: script diffs with per-hunk keep or revert; instance
      created / deleted / changed table; assets list.
- [ ] Checkpoints (ADR 0108 §6): world snapshot plus file copies under
      `.engine/ai/checkpoints/`, *Restore* on every user message.
- [ ] Stop, steer (typing while it runs), completion notification; the token,
      context and cost meters; *Compact*.
- [ ] Entry points from the editor: *Ask AI about this error* (console),
      *Explain* / *Fix* (script selection), *Ask AI* (Explorer selection).
- [ ] First-use notice (what will be sent, to whom); nothing sent before a
      provider is set.
- [ ] Icons in the theme; every string an i18n key; the panel usable at the
      editor's smallest supported size.
- [ ] Tests with the fake provider: a thread that creates parts, is approved,
      reviewed, restored; a denied approval changes nothing; the panel's frame
      cost recorded.

## Stage A6 — autonomous runs and external agents

- [ ] Autonomous mode: a goal, a budget (tool calls, minutes, money) shown and
      enforced, the stop rules of ADR 0108 §8, a checkpoint per plan step, and
      the final report with *Restore to before*.
- [ ] `ludwerk ai run "<goal>" --project <dir> --budget ...`: the same loop,
      headless, a report file at the end.
- [ ] External agents over the Agent Client Protocol: detect Claude Code (its
      ACP adapter), Codex and Gemini CLI; run one as a subprocess with `ludwerk
      mcp` handed to it; its messages, tool calls, plans and permission
      requests in the same panel.
- [ ] The tool evaluation suite (`tests/ai-eval/`): ten real tasks -- build a
      small level, fix a broken script, add a door that opens, make a HUD,
      make it multiplayer, playtest and report a bug -- scored by checks on the
      resulting world; run on demand and nightly with a real model; results
      in `docs/perf-baselines.md`'s sibling, `docs/ai-eval.md`.

## Stage A7 — documentation and closing

- [ ] Manual: *AI in Ludwerk* (the panel, modes, approvals, checkpoints,
      autonomous runs, privacy), *Connecting an AI client* (`ludwerk mcp
      install`, each client), *Writing project tools*.
- [ ] The tool reference, generated from the registry, like the API reference.
- [ ] CHANGELOG, PROGRESS.md, and a *tools* line in the ledger template for
      every future ADR (ADR 0108 Consequences).
- [ ] This ledger's Findings section.

## Findings

(Filled in as the work finds things.)
