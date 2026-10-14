# 24-scenes

**Scenes at run time** (ADR 0106): a menu, a lobby and an arena, three complete
places, and a game that goes between them without restarting -- alone, or with
other players.

- `content/scenes/*.scene.json` light the world differently. A scene carries
  every service's settings, so the arena's noon does not leak into the menu's
  night.
- Each scene has its own code, mounted only while it is loaded:
  - `src/scenes/menu/client/` draws the menu;
  - `src/scenes/lobby/server/` decides who is ready, and
    `src/scenes/lobby/client/` lists them;
  - `src/scenes/arena/server/` builds and spins the arena and gives every
    player a character (`player.Character`), and `src/scenes/arena/client/`
    makes the Move controls, follows your character with the camera and draws
    the HUD.
- `src/shared/Walk.luau` is how a character moves, under
  `GlobalScriptService.Shared`: the server moves every character with it, and a
  client that joined moves its own with it at once, which the server's
  snapshots then correct.
- `src/client/Loading.luau` is the game's own code, under
  `GlobalScriptService.Client`. It is never unloaded. It keeps a loading screen
  with `KeepOnSceneLoad`, and takes the game back to the menu when a match ends.

In the menu:

- **Space** plays alone, straight to the arena. The round travels both ways
  through `LoadScene`'s data and `GetLoadData`.
- **H** hosts (`NetworkService:Host`) and opens the lobby.
- **J** joins a host on this machine (`NetworkService:Join("127.0.0.1")`). The
  host's scene, its lobby, replaces the menu.

In the lobby, **R** says you are ready. The key reaches the server as intent,
and the server writes `player:SetAttribute("Ready", true)`. Attributes
replicate, so every client's list redraws itself, with no `RemoteEvent`. When
everybody is ready, the server loads the arena, and every client goes with it.

In the arena, **WASD** or the left stick walks your character, and **Escape**
goes back to the menu, or leaves the match.

```
run.bat           one window: H in it hosts
run.bat           a second window: J in it joins the first
```
