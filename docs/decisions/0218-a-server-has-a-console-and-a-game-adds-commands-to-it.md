# 0218 — A server has a console, and a game adds commands to it

- Status: accepted; not built yet
- Date: 2026-10-14
- Builds on: ADR 0167 (a host removes a player), ADR 0105 (server code lives in
  `ServerScriptService`), ADR 0120 (a player is a key), ADR 0111 (saves)

## The question

A dedicated server (`--serve`) runs with no window and no way in. Its operator
cannot list who is on it, remove or ban a player, tell everyone the server is
going down, save, or stop it cleanly; `Ctrl+C` is the only verb. A game has
operations of its own -- give an item, reset a dungeon, open an event -- and
writes a RemoteEvent and an admin check for each, or does without.

## Decision

### 1. The console reads its standard input

A process that serves (`--serve`, or `--host` with no window) reads lines from
its standard input on a thread of its own. **A line is executed between
ticks, never during one**: the reader queues it and the frame takes the queue
where it takes the dev server's commands (architecture.md §4), so a command
that changes the world is an input like a player's, and the tick stays
deterministic. Its answer is written to standard output and to the log, and
to whoever sent it (ADR 0219). A process with no standard input -- a service,
a pipe closed at its far end -- has a console with nobody at it, and is
otherwise the same.

### 2. The built-in commands

| Command | Does |
|---|---|
| `help [command]` | Every command with its usage, or one in full. |
| `status` | Uptime, tick time (mean and worst of the last minute), players and the limit, memory, the scene, the lifecycle state (ADR 0220). |
| `players` | Each player: name, `UserId`, key fingerprint, address, ping, time on. |
| `kick <player> [reason]` | `Player:Kick` (ADR 0167). |
| `ban <player\|key> [minutes] [reason]` | Kicks and refuses them again, **across restarts**: kept in `<saves>/bans.json`, by the player's public key (ADR 0120) -- never by name, which a player picks. No minutes is for good. |
| `unban <key>` | Takes one out. |
| `say <text>` | A line every player is shown (`NetworkService.ServerMessage`, a new event the game draws as it likes; the engine's own banner where the game connects none). |
| `save` | `SaveService` writes what is waiting (ADR 0111), and says when it is written. |
| `stop [minutes]` | Says so to every player at the start and at each minute, then refuses joins, saves, removes every player with a reason, and exits 0. No minutes is now. A second `stop` cancels a countdown. |

**A ban by key can be got round, and the record says so**: a player who
deletes their key file (ADR 0224) comes back as a new player. A ban that
holds is a ban of an account: where a game validates its joiners and names
their account (ADR 0226), `ban` takes the account id, and a new key with the
same account is the same banned account.
**There is no ban by address** by default: a household, a school and a
mobile carrier share one (ADR 0216's per-address limit was learnt that way),
and a ban of one is a ban of all of them.

`<player>` is a name, a `UserId` or the start of a key fingerprint, and one
that names two players is refused with both. Every line of the console's own
is an engine catalog key (R3).

### 3. A game adds its own

```luau
local ServerConsole = game:GetService("ServerConsole")
ServerConsole:Register("give", {
    Description = "admin.give", -- a key of the game's catalog, or text
    Parameters = {
        { Name = "player", Type = "Player" },
        { Name = "item", Type = "string" },
        { Name = "count", Type = "number", Optional = true },
    },
    Permission = 2,
}, function(caller: ConsoleCaller, player: Player, item: string, count: number?)
    ...
    return `gave {count or 1} {item}` -- printed to the caller
end)
```

- **Typed parameters**: `string`, `number`, `boolean`, `Player` (as in §2).
  A word left over is the last `string` parameter's (`say`'s text). A call
  that does not parse is answered with the command's usage, and the function
  is not called.
- **A permission level**, 0 to 3: the local console is 3; a remote admin has
  the level their key was given (ADR 0219). Built-ins: `kick`, `say` 1;
  `ban`, `unban`, `save` 2; `stop` 3.
- The function runs on a new script thread on the server, between ticks; an
  error is the caller's answer, not the server's end. It may yield; the
  console takes the next line meanwhile.
- `ConsoleCaller` says who called: `Local`, or the admin's name and key.
- A name the engine uses is refused; registering a name twice replaces it,
  as a hot reload does.
- `ServerConsole` exists only on the server side (ADR 0138): a client script
  that asks for it is told so.

### 4. A command is said in the log

Every command is written to the log with who ran it -- `local`, or the
admin's name and key fingerprint -- and what it answered. A ban says who
banned and why.

## Consequences

- A dedicated server can be run, watched and stopped by a person or a
  script feeding its input.
- `bans.json` beside the saves; a game that keeps its own bans ignores the
  command, or wraps it in its own.
- New API: `ServerConsole` (`Register`, `Unregister`, `Execute` for a script
  that wants to run a command itself), `ConsoleCaller`,
  `NetworkService.ServerMessage`.
- Manual: "Online games > Administering a server" -- the console, its
  commands, a game's own, and what a ban by key is worth.

## Not decided here

Remote access (ADR 0219); a manager's view of several consoles (ADR 0220).
