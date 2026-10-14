# 0216 — Dependencies brought up to date, 2026-10-14

- Status: accepted; built
- Date: 2026-10-14
- Amends: ADR 0002 (the Luau pin), ADR 0036 (fastgltf and simdjson), ADR 0086
  (what a replica is sent), the Android target of ADR 0104
- Builds on: ADR 0083 (the deterministic `pow` patch), ADR 0085 (a player who
  comes back), ADR 0093 (the editor's checker)

## The question

Every pin had fallen behind its upstream by one or more releases, the Play
store had raised the API level it requires, and two GitHub Actions majors the
workflows used were being retired. The maintainer approved upgrading all of
them together on 2026-10-14, each pinned to an exact version. This record lists
what moved, what each upgrade needed, and what the checks found. It also covers
three defects that were fixed in the same batch, because they share the batch's
single gate.

## Decision

### The pins

| Dependency | From | To | Commit |
|---|---|---|---|
| Luau | 0.734 | 0.742 | `8387cfb461b38ce09e37c1e4c2811538ef1bf5cb` |
| luau-lsp (rokit) | 1.69.0 | 1.70.1 | |
| StyLua (rokit) | 2.5.2 | 2.6.0 | |
| SDL | 3.4.14 | 3.4.18 | `829a65d769d935c4852f8159e964312c0957260a` |
| fastgltf | 0.9.0 | 0.9.1 | `705564d4d54500ae88befb0db9e361e447923655` |
| simdjson | 3.12.3 | 4.6.11 | `f5de14f09256982933af2849beb43778bd421ca7` |
| meshoptimizer | 1.2 | 1.3 | `9e1f07b159d3cb777f1c67ed31fc11fd117986f4` |
| xxHash | 0.8.3 | 0.8.4 | `c87183a77d67f7d37e3d2d1b7eaac5e7c695e4f0` |
| Android `compileSdk` and `targetSdk` | 35 | 36, with build-tools 36.0.0 | |
| Android Gradle plugin | 8.7.3 | 8.9.3 | |
| `actions/checkout` | v4 | v7 | |
| `actions/cache`, `actions/cache/restore` | v4 | v6 | |
| `actions/upload-artifact` | v4 | v7 | |
| `actions/download-artifact` | v4 | v8 | |
| `actions/setup-java` | v4 | v6 | |

**What each upgrade needed:**

- **Luau.** The `pow` patch (ADR 0083) is kept. 0.742 still sends `^` and its
  constant folding to the C runtime's `pow`, so the patch was rebuilt against
  the new sources, and it still does the same thing.
- **simdjson** moves only because fastgltf 0.9.1's CMake targets 4.6.11. That
  is the rule its row has always followed.
- **SDL 3.4.18** adds `SDL_GAMEPAD_TYPE_STEAM`. A Steam controller now names
  the Xbox layout, which is the layout its face buttons have. All six of our
  patches still apply as written.
- **Android API 36.** It needs Gradle plugin 8.9.1 or later. 8.9.3 is a patch
  release of the first line that builds API 36, so it is the smallest move. A game
  still runs from API 21 up. The platformer was exported as an APK with
  `targetSdk` 36 and run on the emulator.
- **meshoptimizer and xxHash** are pin changes only. Nothing that calls them
  changed.
- **The nightly sanitizer job** ran out of time on run 38544278457 and was
  ended before it finished. A suite that stops early reports nothing about the
  tests it never ran. Its time limit is now 140 minutes, with 120 for the
  step.

**Not in this batch.** FidelityFX SDK 2.x, GameNetworkingSockets and simdjson 5.
Each is a change of API and needs its own record.

### fastgltf 0.9.1

- **Its validation now catches an accessor that reads past its buffer view.**
  The importer's own bounds check stays in place behind it. A file like that is
  now refused as an invalid document, which is a refusal either way.
- **A node's morph weights override its mesh's.** The importer took only the
  mesh's weights, so a model whose node said otherwise started from the wrong
  shape. A node's weights are now where its targets start. The test is
  `gltf: a node's morph weights are where its targets start`.

### Bytecode across the upgrade

Luau 0.742 still writes bytecode version 9, as 0.734 did. Its VM reads
versions 3 to 14, where 0.734 read 3 to 13. **A game exported by the previous
build still runs**, and so does one exported by this build on the previous
build's VM. Nothing is refused.

### A Luau upgrade's checklist

Every Luau upgrade from now on is held to this list, and this one was:

1. Rebuild the `pow` patch, or drop it if upstream no longer needs it. Then
   regenerate the type definitions, and run the determinism traces on every
   tier. They are the gate's.
2. Compare `LBC_VERSION_MIN`, `MAX` and `TARGET` with the previous pin, and
   say in the CHANGELOG whether an older export still loads.
3. Run `--check-scripts` on every example, every template and the starter.
   This time: 38 projects, no errors and no warnings.
4. Run the editor's script editor, language service and script document tests,
   and the scripted session in a tab: type a member and get suggestions, see an
   error appear and go away. Saving and hot reload are covered by
   `script_files_tests` and the hot-reload tests.
5. Make sure no experimental Luau flag is on in the engine or in luau-lsp.
   The editor's checker turns on Luau's checker flags except the ones Luau
   lists as experimental. `ludwerk check` and VS Code now get the parser's
   flags from the engine (ADR 0217).
6. Look for the language features this Luau carries behind a flag. Offer them
   by name in Project Settings > Luau (ADR 0217), and take out the ones it
   made permanent.
7. Report what changed in the diagnostics. This time, in the engine's own
   checker, nothing: every example, template and the starter gave the same
   result under 0.742 as under 0.734. In luau-lsp 1.70.1 -- which carries
   Luau 0.740, not 0.742 -- seven new errors in the repository's own tools,
   in three patterns, all about a narrowed value: one checked against `nil`
   and then used (`'(string?) & ~nil'` refused where a `string` is wanted),
   a table checked against `nil` and then indexed, and a function written
   inside an `if` condition whose `string?` return it took for the enclosing
   function's `number`. **Measured against the engine's own Luau**: the
   vendored 0.742's `luau-analyze`, with the same solver, flags and Lute
   definitions, reports none of them; 1.70.0 (Luau 0.739) reports all seven;
   1.69.0 (0.729) reports none, but does not know `if local` or the other
   flags 0.742's parser has, so pinning it would make VS Code refuse what a
   project's `[luau.flags]` turns on (ADR 0217). **1.70.1 is kept**, each site
   is written another way -- a cast, a named function -- with nothing turned
   off, and the three patterns are in the CHANGELOG and the manual's
   `ludwerk check` page, so a game's developer recognises them. When a
   luau-lsp built on 0.742 or later is released, it replaces this one and the
   casts can go. One more, `..=` on a name narrowed by `~= ""`, is reported
   by 0.742 and 1.69.0 alike, so it is the language's and was simply written
   another way.
8. When a luau-lsp built on Luau 0.742 or later is released, move to it,
   take the casts and named functions of item 7 out, and take the three
   patterns out of the CHANGELOG's note and the manual.

### Players of one address: 64, and floods told by their rate

**The defect.** An authority took at most eight connections from one network
address, and refused the ninth without saying why. That ninth person could be
in a household behind one router, a LAN house, a school behind one NAT, or on a
mobile carrier, where thousands of phones share an address. ENet dropped the
connection, so the player saw "connection lost". A load test with headless
players stopped at 128 for the same reason.

**Now:**

- **The limit is a project setting.** `[network] max_connections_per_address`
  and the flag `--max-per-address=N` set it. The default is 64, and 0 means no
  limit. 64 covers a LAN house or a school without letting one machine take a
  whole server.
- **A player over the limit is told why.** The authority counts the players it
  has welcomed from each address. When one more arrives, it sends `Refused`
  with reason 4, `RefusedAddressLimit`. The player gets a `JoinFailed` message
  in their own language, and the server writes one line to its log.
- **ENet's own per-address cap sits 16 above the limit.** So the connection
  that is one too many stays open long enough to be told.
- **A flood is told apart from a crowd by its rate, not its count.** More than
  20 connections begun from one address within 60 receives, or 16 that have
  not yet said hello, are dropped without a reply. A machine that dials in a
  loop is not owed one.

The test joins twelve players from one address three times. With the default,
all twelve are welcomed. With a limit of ten, ten are welcomed and two are told
why. With 0, all twelve are welcomed.

### What a replica is sent: measured to a part's edge

**The defect.** Interest was measured from the player's focus to each part's
centre. A 4 km floor centred on the origin was never sent to a player standing
on it 1 km out. On the client the floor was missing, the predicted character
fell through it, and the authority corrected it every tick: more than 3,500
corrections in 30 seconds per client. Any large part does this: a road, a city
wall, a long bridge.

**Now** the distance is measured to the part's oriented box: how far the focus
is past each face, along the part's own axes. A sprite counts as the circle
around its rectangle. The streaming radius and its hysteresis are unchanged.

**Why not the size classes that authored chunks use.** A part already is its
box, so the exact distance costs three dot products where the centre's cost
one, per part and per focus. A class would round a long road up to a sphere
around its middle. That sphere is sent to everyone beside the road, kilometres
from where they stand. The test puts a player on a 4 km floor 1 km from its
centre. Measured to the centre, the floor never arrives and there are 227
corrections. Measured to the edge, the floor arrives and there are none.

### A script that begins with a byte-order mark

Windows Notepad and Windows PowerShell 5 write a UTF-8 byte-order mark at the
start of a file. The game ran such a script, because its compiler skipped the
mark. The editor's tab and `--check-scripts` did not skip it, and reported
"Expected identifier ... got Unicode character U+feff" and a misplaced
`--!strict`. Now every reader skips the mark: the world's file reads, the
script document, the language service, the export's compiler, and the hash
that hot reload compares. All of them use one rule,
`core/utf8_mark.h`. A file saved with the mark and the same text saved
without it count as one text.

## Consequences

- One gate covered all of it, and one commit records it.
- A project with more than 64 players behind one address must raise the limit,
  and the refusal tells its players so.
- A server that has used a lot of large parts sends them to more players than
  before. That is correct: those players were standing on them.
