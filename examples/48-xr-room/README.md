# 48-xr-room

**A room for a headset** (ADRs 0244 to 0246): crates and balls on a table to
pick up and throw, a teleport and a snap turn, and a panel of buttons.

## Starting it in a headset

With the headset connected to the PC -- a Quest through Link or Air Link,
or any headset SteamVR runs -- start the engine from the package folder with
this example and `--xr`:

```
engine-host.exe <path to the repository>\examples\48-xr-room --xr
```

`run.bat --xr` does the same from a build. The window shows what the left
eye sees. With no headset, the room is drawn on the screen and says how to
start it in one.

## In the room

- **Pick up**: hold a controller's grip near a crate or a ball. **Throw**:
  let go while the hand is moving; it flies on at the hand's speed.
- **Teleport**: push the left stick forward, aim the arc at the floor, and
  let the stick go. **Turn**: push the right stick to a side.
- **The panel**: point the right controller at a button and pull the
  trigger, or, with your hands tracked, press it with a fingertip.
  *Put everything back* returns what was thrown; *Turning* switches between
  turning in steps and smoothly. *Passthrough* waits for the passthrough
  work.

## What to read

- `src/client/init.luau`: the room, the picking up and throwing, the panel,
  and `@engine/xrcomfort` started in one call.
