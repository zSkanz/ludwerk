# 0132 — Players chat through `TextChatService`

- Status: **withdrawn by the owner on 2026-10-03**: a game's chat is the game's own code, built on `RemoteEvent` (ADR 0077) and the transport's encryption; the engine ships nothing chat-specific. The text below is kept as the record of what was decided and undone.
- Date: 2026-09-27
- Decided by: the owner, on 2026-09-27, approving text chat from a survey of
  public documentation (R7).
- Builds on: [0077](0077-game-messages-cross-the-wire-through-a-remote-event.md)
  (messages across the wire), [0099](0099-teams-and-network-ownership.md) (teams),
  [0120](0120-the-transport-is-encrypted-and-a-player-is-a-key.md) (a player is a
  key; the transport is encrypted).

## Context

Every multiplayer game needs players to talk. Today a game would build a chat
from `RemoteEvent`s, a UI, a rate limit and bubbles over heads by itself.

## Decision

1. **`TextChatService`** (replicated): `ChatWindowEnabled`, `BubbleChatEnabled`,
   `ChatInputBarEnabled`, and the callbacks `OnIncomingMessage(message)` (change
   how a message looks before it is shown) and `ShouldDeliver(message, player)`
   (on the authority: whether a player receives it). `FilterCallback(text,
   player)` on the authority lets a game filter text; **the engine ships no
   moderation service of its own** and says so in the manual.
2. **`TextChannel`**: `SendAsync(text)`, `MessageReceived(message)`,
   `AddUserAsync(player)`, `RemoveUser(player)`. Default channels are made by the
   engine when `CreateDefaultChannels` is true: `General`, `System`, one per team,
   and whispers (`/w name`).
3. **`TextChatMessage`** (a value): `Text`, `PrefixText`, `TextSource` (the
   player), `TextChannel`, `Timestamp`, `Status`, `Metadata`.
4. **`TextChatCommand`**: `PrimaryAlias`, `SecondaryAlias`, `Triggered(player,
   text)`; built-in `/team`, `/w`, `/clear`.
5. **Through the authority**: a message goes to the authority, which checks
   length (`MaxMessageLength`, default 200), rate (`[chat] messages_per_second`),
   channel membership, the filter and `ShouldDeliver`, then delivers it. Over
   ADR 0120's encrypted transport.
6. **The default UI**: a chat window and an input bar (`/` or `Enter` focuses it;
   the phone's keyboard on touch), and bubbles over each speaker's character, all
   themed, i18n'd, and configurable through `ChatWindowConfiguration` and
   `BubbleChatConfiguration`; a game can turn them off and draw its own.
7. Solo play has a working local channel, so the UI can be built and tested
   without a server.

## Consequences

- A multiplayer game has chat by setting nothing.

## Not decided here

- Voice chat.
- A moderation service.
