# 0225 — The editor plays a game's worlds on one machine

- Status: accepted; built, with the places in the editor of "As built"
- Date: 2026-10-14
- Builds on: ADR 0106 §5 (Play with players: a match of separate processes),
  ADR 0220 (the manager, `servers.toml`), ADR 0221 (instances), ADR 0222
  (Travel)

## The question

A game with a town, a field and a dungeon instance can only be tried, today,
by starting a manager by hand. A portal, a dungeon made for a party, a party
moved together and an instance that falls are what most needs trying, and
they must be tried by the person making the game, in the editor, on one
machine.

## Decision

### 1. Play with players runs the project's `servers.toml`

When the project has a `servers.toml`, **Play with players** starts the
manager (ADR 0220) of the build the editor runs, in a scratch folder under
`.engine/play/`, with its worlds on the machine's own address and its ports
taken from a free range, instead of a single server. Its clients join the
front door, which sends them to the entry world. Stop stops the manager, which
stops every world.

- **Clients have windows; worlds do not.** Each client is a window tiled on
  the screen, as ADR 0106 §5 has it. A world is a headless process, as it is
  on a real server, so the screen is the players'.
- **Each world has a tab** in the Output panel, under its name, its lines as
  they come, with a console field that sends a line to that world (ADR 0218)
  -- `status`, `players`, a game's own commands.
- **The manager's dashboard** (ADR 0220 §8) is a small panel: each world and
  instance, its state, its players, its tick time. An instance appears when
  it is made and goes when it closes.

### 2. What can be tried

- A portal: a client walks into it, the loading screen, the other world.
- An instance: a party asks for one, is moved together, and it closes a
  minute after they leave (the panel shows each step).
- A fall: a world's row has *End* (the process killed, not stopped), so the
  game's handling of `InstanceFailed` and a world's restart are tried as they
  will happen.
- An update: *Restart with this build* rebuilds the scripts and runs ADR 0220
  §7's update over the running worlds, so nobody is dropped -- the editor's
  own hot reload, for a game of several worlds.

### 3. Bounded

At most eight worlds and instances at once and four clients, which a
development machine runs; the panel says when a template's `max_instances` or
this limit refused one.

## As built

- **The manager's editor mode**: `--manager-play=<port>` takes the front door
  at the editor's match port and the worlds on the sixteen after it, on
  127.0.0.1, with no administration and no health; `--manager-root` puts its
  logs, keys and saves in `.engine/play/`. It says its worlds each second, and
  each line a world writes, on its output as messages the editor reads.
- **The ports are the editor's, not probed**: the match port and the sixteen
  after it. A port another program holds fails that world's start, which
  the Console says.
- **The world tabs are the Console's**, which is the editor's output panel: a
  tab for every world and instance beside *All*, and a line typed in a world's
  tab goes to that world's console through the manager.
- **The worlds' table is in the Run view**, under the match's controls, not a
  panel of its own: it is there only while the match runs, which is when
  somebody is looking at the Run view.
- **This game's worlds** is a box in the match controls, shown when the
  project has a `servers.toml`; the dedicated-server box has no meaning while
  it is ticked.
- **Stop asks the manager to stop** and waits up to five seconds before
  ending it, so its worlds are not left running with nobody to end them.

## Consequences

- An online game is tried in the editor, end to end, by its maker.
- Nothing new on the wire or in the engine beyond ADRs 0220 to 0222: the
  editor drives the same manager a server runs.
- Manual: "Online games > Worlds and instances" -- trying them in the
  editor.

## Not decided here

Several worlds in the editor's own process (out of the track's scope).
