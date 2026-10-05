# 0177 — A script writes the clipboard, and never reads it

- Status: accepted
- Date: 2026-10-04
- Decided by: the agent, on a request from the owner's review of a game's
  menus (ludwerk-08), under the standing rule to decide as professional
  engines do and record it
- Builds on: ADR 0029 (the input system), the text input's own copy and paste

## Context

A game shows a room's code and its player wants to click it and have it
copied. A script had no way to put text on the clipboard: a read-only
`TextInput` lets a player select and press Ctrl+C, which is two gestures on a
desk and a long press and a menu on a phone.

## How mature engines do it

- **Unity**: `GUIUtility.systemCopyBuffer`, read and write.
- **Godot**: `DisplayServer.clipboard_set` and `clipboard_get`.
- **Unreal**: `FPlatformApplicationMisc::ClipboardCopy` and `ClipboardPaste`.
- **The web**, which is the platform that had to think about strangers'
  code: writing is allowed on a user's gesture; reading asks permission.

Engines for trusted code give both. A platform that runs code its player did
not write gives the write freely and guards the read.

## Decision

1. **`InputService:SetClipboard(text: string)`**. It puts `text` on the
   clipboard of the machine the script runs on.

2. **There is no way to read it.** What a player copied somewhere else -- a
   password, a message -- is theirs, and nothing a game does needs it: a
   `TextInput` already pastes where the player chooses to. The sandbox (R4)
   is about what a game's code may reach, and this is on the far side of it.

3. **The host applies it**, after the tick, on a machine with a window. The
   script module never reaches the window system: it leaves the text for the
   host (`takeClipboardText`). On a server and in a headless run there is no
   clipboard and nothing happens -- it is a client script's call.

4. **At most 64 KiB**, cut at a character's boundary. The last call of a frame
   is the one the clipboard keeps.

5. Not gated on a gesture. A game is not a web page: it is installed and it
   is the foreground, and a rule that a copy must follow a click would be a
   rule a HUD's "copied!" button satisfies anyway.

## Consequences

- Windows, Linux, macOS and Android, through the same call the text input's
  own copy uses.
- One method more in the API; the protocol is unchanged (40).
