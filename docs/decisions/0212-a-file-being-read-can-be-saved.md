# 0212 — A file the engine is reading can be saved

- Status: accepted; the first half is built, the second is its own item
- Date: 2026-10-13
- Builds on: ADR 0021 (vendoring: a local change is a patch and a manifest
  note), ADR 0024 (hot reload)

## The question

The engine reloads a file when it changes. So it reads a sound, a model or a
picture at exactly the moment the tool that makes it is most likely to save it
again, and on Windows the reader decides what anybody else may do with a file
until it closes it. The platform library opened a file it only reads shared
with other readers and with nobody else. For the length of a read, then, a
tool's save was refused: "file in use", from the artist's own tool, with
nothing to say why.

It was found as a test's second write of a sound, refused in silence while the
frame's own read of the first still had the file.

## The decision

### 1. A file opened to be read is shared with a writer, a replace and a delete

On Windows, every read the engine opens for content shares write and delete as
well as read. It is patch 0006 on the platform library, in the two places a
read-only open is made: the one every whole-file read and the thread-pool
asynchronous read go through, and the Windows 11 asynchronous read. A file
opened for writing is shared as it was.

What this allows, each held by a test with the file open as the engine has it:

- a save in place (the file opened again and written);
- a save beside the file and a replace (`ReplaceFile`);
- a delete.

What it cannot allow: a plain rename of another file over one that is open.
Windows refuses that whatever the reader shares, and a tool that saves that
way deletes first.

Packs are not changed. A pack is mapped, a mapped file cannot be made shorter
whatever it is shared for, and a pack is an export's, not something a tool
saves while a game runs.

### 2. What a read sees of a file written under it

Whatever the disk held as it read. That is not new: a tool that writes in
place while sharing the file for reading could always be read half way
through, by the reload its first change notice starts. What makes it right in
the end is the same in both cases, and is the rule: **the tool's last write
brings a notice, and the reload that notice starts reads again.**

### 3. A read that fails leaves what was loaded (to be built)

Between the half-written read and the last notice, a loader has a file that is
short or does not parse. Every cache of content reloads by forgetting first
and reading on the next use -- meshes and their textures, materials, sounds,
the mouths' tracks, sequences, animation graphs, skeletons -- so none has the
old asset to fall back on: a sound saved mid-read plays the placeholder tone,
with a warning, until the next notice.

The rule to build, as its own item and beginning with sounds and meshes: a
reload whose read is short or whose parse fails **keeps what was loaded
before, and says nothing**, until a notice brings a read that succeeds. Each
kind gets a test that writes half a file, reloads, and finds the old asset in
use and the log quiet. One thing is decided when it is built and not here:
quiet must not hide a file that was saved whole and is wrong, so a failed read
that no further notice follows within a moment is said, once.

## Consequences

- A tool's save is no longer refused because the engine was reading the file.
- The patch is carried until a pinned release of the platform library shares
  the same way; the manifest's note says so.
- Other systems open files as the engine did before: nothing changes there,
  where a reader never refused a writer.
