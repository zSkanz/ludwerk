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

## As built, the records (2026-10-18)

- **SQLite 3.53.4**, of 2026-07-24, the newest release at least two weeks old
  when it was pinned. It is vendored by a new kind of manifest row, an
  **archive**: the release's zip, pinned by its SHA-256 and by each file's,
  because the amalgamation is in no git tree. `vendor.luau sync` checks the
  archive's hash before anything is unpacked, and `status` re-hashes the
  three files every run. Built with `SQLITE_OMIT_LOAD_EXTENSION` and
  `SQLITE_DQS=0`, in every build that can be a server (`ENG_ENABLE_DATA`): not
  the player profile, a phone, the web or the Xbox app. "Public Domain" is
  on R6's list by this ADR.
- **The records** (§2): `DataService:LoadAsync` and `DataRecord` (`Get`,
  `Set`, `Update`, `Remove`, `GetKeys`, `Save`, `Release`, `Released`). A record is
  written in a save slot's format. One process opens the database: the
  manager, whose worlds ask over their pipes (`data-load`, `data-write`,
  `data-record`), or a server with no manager, which keeps `data.db` in its
  saves folder. Each run of a world holds records under its own name, so a
  world started again holds none of what the run before it held; a world
  that stops writes everything it holds as it goes, and the manager reads
  those lines before it lets go of the rest.
- **Whose record**: the account a ticket named; else the key the player's
  copy of the game joined with; else, for the players at a hosting machine,
  their number there -- that machine's key exists only once it listens.
- Proved by the broker's tests (a second world waits and is given the record
  when the first lets it go, is refused after ten seconds, and a world that
  died has its records let go and its late write refused), the database's
  (a record read back after the file is closed), and `data_record`: a hosting
  machine run twice with the same saves, the second run finding the level
  the first wrote.
- **Not yet**: `Version` and `OnMigrate`, items, money and transactions (§3),
  shared values (§4), a game's own backend (§5), the backup and the console's
  `data` commands (§7).

## As built, items, money and transactions (2026-10-18)

- `TransactAsync`, `CreateItemAsync`, `DestroyItemAsync`, `GetItemsAsync`
  and `GetBalanceAsync` (§3). The database's schema is version 2: `items`
  (an id SQLite never gives twice, a kind, an owner, the fields as a saved
  value's text), `balances`, `transactions` and `moves`. A database of
  version 1 is brought up to 2 where it is opened.
- **A transaction is one SQLite transaction**: every move, then its line in
  the move log with the transaction and the world, then the commit -- or a
  rollback that leaves nothing. An item moves only from whoever the move says
  holds it, and money only from a balance that holds it; a refusal says which
  (`ItemNotThere`, `NotEnough`, `Invalid`). Making and destroying an item are
  transactions too, logged as moves from and to the game.
- **An owner** is a player's account or a name the game gives, such as a
  guild's bank. For money, nil is the game itself: it pays from nowhere and is
  paid into nothing, which is how money enters and leaves.
- Each operation is one request and one answer in JSON: over a managed
  world's pipe (`data-op`, `data-result`), and run in the process by a server
  that keeps its own database. The same code answers both.
- Proved by `data_trade_killed`: a server trades a sword for gold back and
  forth and is killed (the process ended, with no warning) at a different
  moment in each of eight runs. Each run first counts five swords and a
  thousand gold, and so does a last run that is not killed. And by the
  stores' tests: a transaction that fails part way changes nothing, an id
  destroyed is not given again, and what was traded is there after the file
  is closed.
- **Not yet**: killing the manager in the middle of a trade (the same
  database and the same transaction, not yet a test of its own), a console
  command that undoes a transaction, and the move log's 90 days.

## As built, versions and shared values (2026-10-18)

- `DataService.Version` and `OnMigrate` (§2), as `SaveService`'s: a record
  written at an older version, with something in it, is passed to
  `OnMigrate` once, in a thread of its own, before `LoadAsync` returns it,
  and is then written at the current version. `data_migrate` writes a
  record with version 1 of a game and loads it with version 2, which
  migrates it once; a second run of version 2 does not.
- `GetAsync`, `SetAsync` and `UpdateAsync` (§4). The schema is version 3,
  with `shared`: a value per scope and key, and its version. A write reads
  and writes in one SQLite transaction, at the version the world read or
  not at all. `UpdateAsync` is that loop: the value is read, the function
  makes the next one, and it is written at the version read. Where another
  world wrote first, the answer already holds what it wrote, so the function
  runs again on it with no second read. No lock is held across the script.
  After 32 losses in a row it raises an error.
- `data_shared`: three threads add one to the same value 200 times each, at
  once, and it ends at 600, version 600. Threads of one world interleave
  their reads and writes across frames as three worlds would, and lose to
  each other as often.
