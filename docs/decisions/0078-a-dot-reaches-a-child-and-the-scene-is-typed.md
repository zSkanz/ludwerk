# 0078 — A dot reaches a child, and the scene's tree is typed

- Status: accepted
- Date: 2026-09-23
- Supersedes: [0061](0061-a-child-is-not-a-member-and-the-refusal-says-so.md)
  and `api-design.md` divergence #26

## Context

ADR 0061 kept `workspace.Baseplate` an error. Its reason was the type system:
typing a child with a dot would need a string indexer on `Instance`, and an
indexer would turn every misspelled property into a silent write. The friction
had been reported twice before that ADR, and this one reverses it.

Two measurements, taken with the pinned `luau-lsp` before anything was
written, shaped what replaced it:

- **The indexer is not available anyway.** This Luau has no `declare class`,
  and an indexer on a `declare extern type` is ignored. `root.Nested` stays a
  type error with one declared. 0061's price was never payable in this
  toolchain.
- **A tree declared per project works, and keeps typo detection.** Declaring
  `workspace` as `Workspace & { Player: Model & { Walker: CharacterBody } }` in
  a second definitions file, loaded after `engine.d.luau`, replaces the plain
  declaration. Then `workspace.Player.Walker` is a `CharacterBody`, while
  `workspace.Playr` and `walker.WalkSped` are still errors. This is what a
  source map gives a language server: the engine declares the classes, and
  only the project knows its tree.

## Decision

1. **At run time, a dot reads a member first and then a child.** A property,
   a method or an event of that name wins, so a part called `Name` never hides
   the property. Otherwise the first child of that name is returned. Otherwise
   it is still `scene.err.unknown_member`. Assigning to a child's name is
   refused with `scene.err.child_not_member`, which now says it is a child and
   that a child is replaced by parenting.
2. **The scene's tree is typed**, in `<project>/.engine/types/scene.d.luau`,
   written by the engine from the scene as the editor holds it: whole, with no
   script run and nothing partitioned away.
   - A child named like a member of its parent is left out, because a dot
     never reaches it.
   - Of two children with one name, the first is declared, because it is the
     one a dot reaches.
   - A name that is not an identifier, or is a keyword, is quoted:
     `["Lantern Post"]`.
   - Streamed-in instances are not the scene's and are left out.
3. **Written in three places.**
   - The editor writes it when it opens a project and on every save.
   - `engine-host <project> --write-types` writes it and exits.
   - `ludwerk setup` runs that host command.

   `ludwerk check` loads it after `engine.d.luau` by default, and the starter
   template's `.vscode` settings list it second.
4. **A child a script makes at run time is not typed.** It has no declared
   name. `FindFirstChild`, `WaitForChild` or a cast reaches it, as before.
5. **The editor follows the runtime.** Completion offers children after a dot,
   after the members. The edit-time lint that underlined a child read now
   underlines only an assignment to a child's name.

## Consequences

- `workspace.Player.Walker` works, is typed in the editor, in VS Code and in
  `ludwerk check`, and a typo in the path is still an error.
- The divergence page and `api-design.md` lose #26. The manual's *Why* page
  becomes *Reaching a child*.
- The typing is only as fresh as the last save or setup. A tree changed by hand
  in the scene file and never opened is typed as it was. The editor rewrites it
  as soon as it opens the project.

## Evidence

- `tests/conformance/instance/dot_access_children.spec.luau`:
  - a child and a grandchild are reached;
  - a member wins over a child of its name;
  - an unknown name is the plain error, and so is a destroyed child;
  - assigning to a child's name is the child error.
- `engine/app/tests/scene_definitions_tests.cpp`: the declared tree, with
  quoting, the first of two names, and a member-named child left out.
- `engine/app/tests/script_complete_tests.cpp`: children after a dot, none after
  a colon, and the lint only on an assignment.
- By hand, on `examples/10-open-world`: `ludwerk setup` wrote the tree, and
  `ludwerk check` accepted `workspace.Player.Walker.WalkSpeed` and rejected
  `workspace.Playr`.
