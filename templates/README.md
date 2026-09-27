# templates/ — `ludwerk new` Project Templates

Three templates, per `docs/api-design.md` §4 and §8:

- `starter/` — the minimal project: `project.toml`, a scene, and
  `src/client/Main.luau`, which greets you when you press Play (ADR 0105:
  code in `src/client/` runs where a player sits, `src/server/` on the
  authority). The Spinner's own script turns it, inside the Spinner (ADR 0092),
  and a `ModuleScript` in `ReplicatedStorage` holds the settings it requires.
- `obby/` — the idiom teacher: tags + `TagService` signals, `CharacterBody`
  respawn, IAS jump action, tweened platforms, `Signal.new`, a HUD, localized
  strings, a `.prefab.luau`.
- `openworld-demo/` — the flagship project (roadmap M8), streaming + camera
  rig + day/night + hot-reload workflow; also serves as `examples/10-open-world`.

Templates are analyzed in CI under the pinned luau-lsp (all `--!strict`).
Populated starting at M3 (`starter`), M6 (`obby`), M8 (`openworld-demo`).
