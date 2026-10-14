# 0227 — The editor watches the project's files

- Status: accepted; built
- Date: 2026-10-14
- Builds on: ADR 0057 (where Ctrl+S sends a script), ADR 0058 (Play starts
  the scripts and compiles `Source`), ADR 0105 (code under the script
  services is a file), ADR 0062 (a changed asset reloads itself)

## The question

The editor can be open while something else edits the project: VS Code, an
AI agent, a sync tool. That is how the maintainer works. Before this record
the editor noticed none of it.

- **Scripts.** A script changed on disk kept its old text in its tab and in
  the world's `Source`. Play compiles `Source` (ADR 0058), so Play ran the old
  code until the project was opened again.
- **Scenes, stamps and materials.** One changed on disk could be written over
  by the editor's next save, with no word. Only a script's Ctrl+S had a guard.

`ludwerk dev` had a watcher. The editor, which `ludwerk edit` runs without the
dev server, had none.

## Decision

### 1. A look at the disk, twice a second

`ProjectWatcher` walks `src/`, `content/` and `i18n/` and compares each
file's write time and size with the previous walk. It does this every half
second, and only in the editor. The engine's own folder, hidden files and
editors' temporary files are left alone.

- **Why walk instead of using change notifications:** a walk behaves the same
  on every platform and on a network folder. **Measured, not assumed**: the
  first version took each file's path relative to the root with
  `std::filesystem::relative`, which canonicalises both sides with several
  system calls a file. A pass cost 31 ms for one game on this machine (358
  files) and 85 ms for another (1,203). Built lexically from the root, a pass
  costs 1.5 ms and 3.7 ms. That is still more than an editor frame should pay,
  so **the walk runs on a thread of its own** (`ProjectWatcherThread`), and
  the frame takes what has settled since it last asked, which is a lock and a
  swap.
- **A file is reported once it has stopped changing:** it must differ from
  what was reported last, and be unchanged between the last two walks. A file
  still being written by a save, a copy or a tool that saves many times a
  second is therefore read whole, and only once.
- **A removal is reported only after two walks in a row,** so a save that
  replaces a file is not seen as a removal.

### 2. What each change does, in edit mode

Every handler compares a file's content, not its time. A file saved without
changes does nothing, and neither does the editor's own save, which comes
back as news.

**Scripts** (`applyExternalScriptChanges`):

- **Not edited here** (its `Source` still hashes to what was last read or
  written): it takes the disk's text. That covers `Source`, its tab if one is
  open (the caret stays where it was, as nearly as the new text allows) and
  the mount table's record. The next Play compiles the new text.
- **Edited here as well:** the tab says *This file changed on disk while you
  were editing it* and offers **Keep mine**, which writes the tab's text over
  the file, and **Take the disk's**, which replaces it. Until one is chosen,
  Ctrl+S does what it always has: it keeps the disk and puts the editor's text
  in `.engine/conflicts/`.
- **A new file** under one of the five folders (ADR 0105) is mounted where the
  project's open would have mounted it, with the same rules for folder and
  file name, and appears in the Explorer. A file belonging to another scene's
  folder is left alone.
- **A file that is gone** takes its script with it, and its tab if one is
  open. If the script was edited here, it stays and is reported, and the next
  save writes it back.

**The open scene, stamp or material:**

- The editor records a hash of what it last read or wrote of each one.
- A file that still matches that hash is not news.
- A file that differs is read again if it was not edited here. If it was
  edited here, the editor asks: **Reload from disk** drops the edits, and
  **Keep mine** marks the disk's current state as read, so the next save
  writes over it knowingly.
- **A save never writes over a file that changed outside.** Every scene,
  stamp and material save goes through one guard: when the file differs from
  what the editor last read or wrote, the save keeps the disk, puts the
  editor's version in `.engine/conflicts/` and says so.

**Other content** (models, textures, sounds, graphs, baked light) is
forgotten through the same function the dev server's `asset-changed`
message uses, and is read again when it is next asked for. Catalogs under
`i18n/` were already read again in a development run.

### 3. During Play

A change seen while the game runs is held. The status bar says how many
changes are waiting, until Stop. At Stop they are taken in as in edit mode,
so the next Play runs them. **A running game's code is never changed while it
runs.** `ludwerk dev`'s reload rebuilds the world from the scene saved on
disk. In the editor that would replace the game being played with the saved
scene, dropping unsaved scene edits, and the snapshot that Stop restores would
then put the old `Source` back. This is the same reason a tab's Ctrl+S never
rebuilds the world.

- **Preferences > Performance > A script file changed during Play** chooses
  what happens: **Apply it at Stop** (the default), or **Restart Play with it**,
  which stops, takes the changes in and plays again from the editor's world.
- Clicking the status bar's count restarts Play the same way, once.

## Consequences

- Another program can edit scripts while the editor is open, and Play runs what is
  on disk.
- No version of a file is lost silently: scripts, scenes, stamps and materials
  all keep their copy in `.engine/conflicts/` when two versions meet.
- Tests: the watcher reports a file once it has settled, an added file, a
  removal after two walks, and nothing for the editor's own write. Also: a
  script and its tab take the disk's text, and a touch does nothing; a script
  edited on both sides asks, and both answers keep what they say; a new file
  is mounted under the right service, and another scene's file is not; a
  removed file takes its unedited script, and an edited one stays; the next
  Play runs the disk's text; a scene changed outside is never written over,
  is asked about once, and is written over only after **Keep mine**.
