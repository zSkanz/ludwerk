# 33 — Options

The engine's own options screen, opened by one call, in two languages.

```
run.bat
```

- **Options** (or Escape, or Start on a gamepad) opens the screen:
  `require("@engine/settings").open()`. Step the quality level and watch the
  columns' shadows and the lamps' glow change behind it; change the window on
  the Display page and press Apply.
- **Language** goes round the locales the project has a catalog for. The two
  buttons are this project's words (`i18n/en.json`, `i18n/pt-BR.json`); the
  options screen's are the engine's, and `i18n/pt-BR.json` says five of the
  engine's own keys to show how a game re-words it.
- What Apply keeps is in `.engine/settings.json` while the project is run from
  here, and in the player's own folder once the game is exported.

The script is `src/client/init.luau`. The manual: *Graphics and display
settings* and *Localization*.
