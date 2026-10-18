# 0238 — A player's data lives in the manager's database

- Status: accepted
- Date: 2026-10-17
- Builds on: ADR 0111 (`SaveService`), ADR 0220 (the manager), ADR 0222
  (travel), ADR 0230 (saves shared by channels), ADR 0237 (accounts)
- New dependency: SQLite (public domain), see §6

## The question

An online game keeps a player's progress -- level, inventory, money -- across
worlds, channels and restarts, and some of it changes hands: a trade, an
auction. `SaveService` (ADR 0111) is slots of files on the machine that runs
the game, written whole; ADR 0230 made several channels share them under a
lock, with the last save winning. That is right for a single-player save and
wrong for an inventory: two worlds that both hold a player's slot lose one
world's changes, and a trade written as two saves duplicates or loses an item
when a process dies between them.

## Decision

### 1. One database, the manager's

The manager keeps one SQLite database for the game (`data/<game id>.db`, WAL
journal, `synchronous=FULL`) and is the only process that opens it. Worlds ask
it over the pipes they already speak to it on (ADR 0220 §2). One writer means
SQLite's own locking is never contended across processes and a transaction is
one SQLite transaction. A server run without a manager (`--serve` alone)
opens the same database itself; the API is the same.

### 2. `DataService`: a player's record

On the server, `DataService:LoadAsync(player)` returns the player's record --
a table of the same values `SaveService` keeps (numbers, strings, booleans,
`Vector3`, `Color3`, `CFrame` and tables of them, ADR 0111) -- and holds it
for that world:

```luau
--!strict
local DataService = game:GetService("DataService")

local record = DataService:LoadAsync(player)
record:Set("Level", (record:Get("Level") :: number? or 1) + 1)
-- written behind: every 30 seconds, when the player leaves or travels, and when the world stops
```

**A record is held by one world at a time.** A second world that asks for it
waits for the first to write and let it go, up to ten seconds, then is
refused with `DataBusy`; travel (ADR 0222) writes and lets go before the hop,
so the next world loads what the last one wrote. A world that dies holding
records loses at most the 30 seconds not yet written; the manager lets them
go when it sees the world is gone.

`DataService.Version` and `OnMigrate(record, fromVersion)` work as
`SaveService`'s do: a record written by an older version of the game is
migrated where it is loaded.

### 3. Items and money move in transactions, never in records

What changes hands is not part of a record. **An item** is a row: a unique
id the database makes (64-bit, never reused), its kind, its owner, and its
fields. **Money** is a balance per account and currency. They move only by
`DataService:TransactAsync`, which applies all of its moves or none:

```luau
local ok, reason = DataService:TransactAsync({
    { Move = swordId, From = seller, To = buyer },
    { Pay = 250, Currency = "Gold", From = buyer, To = seller },
})
```

A move whose item is not where it says it is, or a payment that would make a
balance negative, refuses the whole transaction. **Every move is a line in the
move log** (when, which transaction, which world, what moved, from whom, to
whom), kept for 90 days by default, so a lost or duplicated item can be traced
and a console command can undo a transaction. `TransactAsync` is synchronous
with the database -- it is never written behind -- so a world that dies after
it returns cannot lose it, and one that dies before it returns did not do it.

`CreateItemAsync(owner, kind, fields)`, `DestroyItemAsync(id)`,
`GetItemsAsync(owner)` and `GetBalanceAsync(owner, currency)` complete it.

### 4. Shared values, with versions

For what belongs to no player -- an auction house's listings, a guild bank, a
world event's progress -- `DataService:GetAsync(scope, key)` returns a value
and its version, and `SetAsync(scope, key, value, version)` writes only if the
version is still that one. `UpdateAsync(scope, key, function(old) ... end)`
does that loop for a script, calling the function again when another world
won. No lock is held across a world's script.

### 5. A game's own backend

`[data] backend = "https://..."` sends the same operations, as JSON over
HTTPS, to the game's own service instead: load and write a record, a
transaction, get and set a shared value. The contract is in the manual, with
a reference implementation in the realms example. The engine does not check
what a backend does with a transaction; it holds the record lock and the
write-behind exactly as it does for its own database.

### 6. SQLite

The amalgamation (`sqlite3.c`, `sqlite3.h`), a pinned release, vendored under
R13, compiled with `SQLITE_OMIT_LOAD_EXTENSION` and `SQLITE_DQS=0`. It is in
the public domain, which R6 allows; it has no dependencies; and it is the
storage most servers of this size use. It is linked into the server's binary
only: a client build (`ENG_ENABLE_REPLICATION` with no server) does not carry
it.

### 7. What can be lost, and the backup

**Said plainly**: a record's fields changed in the last 30 seconds are lost
when the world holding it crashes; a transaction is never lost -- it was
written before `TransactAsync` returned, or it did not happen.

**An online backup**: the console's `data backup [path]` copies the whole
database while the game runs, through SQLite's backup API, a few pages at a
time between the manager's other work, so no world waits on it; the copy is
consistent as of the moment it finished. `[data] backup_every_hours` (0, the
default, for never) and `backup_keep` (7) schedule it into `data/backups/`.
The manual puts it beside the front door's key backup (ADR 0220), because
the two are what a server's operator must keep.

## Proof

- **A trade with the server killed in the middle**: two players trade a
  sword for gold 1,000 times while the world is killed at a random moment in
  each run; afterwards, the move log and the items' owners agree and no item exists
  twice or nowhere. Killing the manager instead: the same.
- A record held by one channel is refused to another until the first lets it
  go; travel carries a change made the tick before the hop.
- A world killed holding records loses at most what it had not written in 30
  seconds, and the records are free again.
- `UpdateAsync` from three worlds at once, 1,000 increments each: 3,000.
- A backup taken while 1,000 trades run opens as a database whose move log
  and owners agree.
- The same tests against the reference backend.

## Consequences

- New API: `DataService` (server only): `LoadAsync`, `Version`,
  `OnMigrate`, `TransactAsync`, `CreateItemAsync`, `DestroyItemAsync`,
  `GetItemsAsync`, `GetBalanceAsync`, `GetAsync`, `SetAsync`, `UpdateAsync`;
  `DataRecord` (`Get`, `Set`, `Save`, `Release`); `[data]` in
  `servers.toml` (`backend`, `backup_every_hours`, `backup_keep`); console
  `data`, `data backup` and `moves` commands.
- `SaveService` stays what it is: a game's own saves on its own machine.
- One database and one manager are one machine: the scale is the scale ADR
  0220 already accepted.

## Not decided here

Several machines sharing one database (server meshing is out of scope on
purpose), and copying backups off the machine, which is the operator's.
