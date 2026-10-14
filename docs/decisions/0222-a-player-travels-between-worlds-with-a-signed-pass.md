# 0222 — A player travels between worlds with a signed pass

- Status: accepted; not built yet
- Date: 2026-10-14
- Builds on: ADR 0220 (the manager, the front door, `Redirect`), ADR 0221
  (instances), ADR 0106 (a scene loaded at run time, `GetLoadData`), ADR 0120
  (a player is a key), ADR 0085 (a reconnect is the same player)

## The question

A player walks through a portal in the town and arrives in the fields, or in
a dungeon made for their party. Two processes, so the player's machine must
leave one server and join another -- with no one else able to join the second
by pretending to be them, nothing the first world sent lost on the way, and
the party arriving together.

## Decision

### 1. The call

```luau
NetworkService:Travel(player, "fields", { gate = "north" })
NetworkService:TravelGroup(party, instance, { from = "town" })
```

- A target is a **world's name** from `servers.toml`, or an
  `InstanceHandle` (ADR 0221) -- never an address: the manager knows where a
  world is, and an address in a script is one that is wrong after an update.
- `data` is a table of what `SaveService` can store (ADR 0111), at most
  16 KiB; the target reads it from `NetworkService.TravelArrived(player,
  data, origin)`.
- `TravelGroup` moves its players together: each gets the same target, and
  the target sees them arrive as one `TravelArrived` each, in the order given.
- Server side only. It errors at once outside a manager, and for a target the
  manager does not have.

### 2. What happens

1. The world asks the manager for a pass for each player (ADR 0220 §2).
2. The manager answers with the target's public address, port and the passes.
3. The world sends each player `Redirect` (ADR 0220 §4) with them, and lets
   the connection go once the player says it is leaving -- or after five
   seconds.
4. The player's machine shows the loading screen (§4) and, **at once and at
   the same time**, connects to the target with the pass in its hello and
   starts loading the target's scene from its own content (`LoadSceneAsync`,
   ADR 0106). The hello is never held until the scene has loaded: a phone
   loading a large scene must not outlive its pass. The target takes the
   player and holds them until their machine says the scene is in.
5. The target checks the pass (§3), takes the player, and fires
   `TravelArrived`; the origin fires `PlayerRemoving` with reason `Traveled`.

**A hop that fails is not tried again with the same pass.** The player's
machine goes to the front door, which knows the travel is pending and issues
a fresh pass for it -- the same player, the same target, the same data --
for up to 120 seconds after the first was made. Past that the travel is
over, and the player is sent to the world they left, or to the entry world
if it is gone.

### 3. The pass

A pass is `{version, player key, origin world, target world or instance id,
data hash, issued tick, expiry, nonce}` signed by the manager with its own
key (Ed25519, libsodium). **The target trusts the manager's public key, which
it was started with, and nothing else.**

- **Short**: it expires 30 seconds after it is made.
- **Once**: the target keeps the nonces it has accepted until they would
  have expired; a second use is refused.
- **For one player**: the player's public key (ADR 0120) is in it, and the
  match's handshake proves the joiner holds that key.
- **For one place**: a pass for the fields is refused by the crypt, and one
  for one crypt by another.
- **What it carried**: the data itself travels with the pass, and its hash is
  signed, so the target knows it is what the origin sent.

The manager's signing key is made on its first start and kept as ADR 0219
keeps an administrator's: readable by its owner only, and never in version
control.

A refused pass is `Refused` with a reason the player is told (expired, used,
not for this world) and a line in the target's log with the reason.

### 4. The loading screen

The engine draws one between the two worlds -- the scene's name and a
progress bar -- unless the game sets its own with
`NetworkService:SetTravelScreen(gui)`, a `ScreenGui` it keeps through the hop.

### 5. What a player keeps

What the game sends in `data`, and whatever it saved. The engine carries the
player's identity (their key, so their `UserId` is the same in the target)
and their local players at the same screen (ADR 0205), who travel with them.
Their character does not: the target spawns one.

## Consequences

- Portals, dungeons, a party moved as one, and the update of ADR 0220 §7.
- `Redirect` and a pass in the hello: the online server's one protocol bump, with
  ADR 0220's message and ADR 0224's encryption.
- A forged, expired, reused or misdirected pass is refused, and each is a
  test.
- Manual: "Online games > Worlds and instances" -- Travel, the loading
  screen, and what a player keeps.

## Not decided here

A player moving between machines that do not share a manager.
