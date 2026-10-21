# Voice chat

Players in a match talk to each other through `VoiceChatService`. The server
decides who hears whom. Each machine sends its own player's microphone and
plays the voices it is sent. Nothing is recorded: a frame is played and gone.

> Voice needs a codec, and this build carries none yet. Until it does,
> `VoiceChatService.Available` is false, nothing is sent, and a game that
> turns voice on is told so once in the log. Everything on this page is in
> place for when it arrives.

## Turning it on

Voice is off until the game turns it on, and the player's own setting must
allow it too:

```luau
local VoiceChatService = game:GetService("VoiceChatService")

-- On the server and on each client.
VoiceChatService.Enabled = true

-- A settings menu, on the client.
VoiceChatService.PlayerEnabled = playerWantsVoice
```

## Who hears whom

The server's `Mode` decides:

| `Enum.VoiceChatMode` | Who hears a speaker |
|---|---|
| `Global` | Every other player. |
| `Team` | The players whose `Team` attribute is the same as the speaker's. |
| `Proximity` | The players whose character is within `ProximityDistance` of the speaker's, heard from where the speaker stands and fading to nothing at that distance. |
| `Script` | The players the server named with `VoiceChatService:SetCanHear(listener, speaker, true)`, and nobody else. |

```luau
-- On the server: a squad's radio.
VoiceChatService.Mode = Enum.VoiceChatMode.Script
for _, listener in squad do
    for _, speaker in squad do
        if listener ~= speaker then
            VoiceChatService:SetCanHear(listener, speaker, true)
        end
    end
end
```

`Player.VoiceMuted`, set on the server, stops everyone hearing a player. On a
client, `VoiceChatService:SetLocallyMuted(player, true)` stops this machine
hearing them, and no other machine knows. Nobody hears themself.

## Push to talk, or voice activity

With `PushToTalk` (the default), the microphone is sent only while `Talking`
is true. The game sets it from its own input, so a key, a gamepad button and a
button on a phone's screen all work the same:

```luau
-- An action the game already binds to a key, a button or a touch control.
talk.Pressed:Connect(function()
    VoiceChatService.Talking = true
end)
talk.Released:Connect(function()
    VoiceChatService.Talking = false
end)
```

With `PushToTalk = false`, the microphone is sent while the player's voice is
louder than `ActivityThreshold` (in dB below full scale, -40 by default), and
for a moment after, so the pauses between words are not cut out.

## Who is speaking

`SpeakingChanged(player, speaking)` fires on each machine when a player it
hears starts or stops, and for its own player when their microphone starts or
stops being sent. `IsSpeaking(player)` answers the same question at any time.
A speaking icon over a head or on a scoreboard follows them.

## Where a voice is heard from

A voice in `Proximity` mode is heard where the speaker's character is, by the
same ear every sound uses. A top-down game hears better with the ear at the
hero: `AudioService.ListenerMode` (see [Mixing](manual:audio/mixing)).

## On the wire

- A voice is 20 ms frames, sent unreliably on a channel of its own, so it never
  holds up the world's state. A frame that is late is dropped, and the codec
  covers one that is lost.
- Each listener plays a speaker's frames through a jitter buffer. It is 40 ms
  deep and grows to 120 ms on a link that needs it.
- The server takes at most fifty frames and four kilobytes a second from one
  player. It drops the rest and says so once in the log.

## On a phone

An Android game whose scripts name `VoiceChatService` declares the
microphone permission, and the player is asked the first time voice is
turned on. A player who refuses still hears everyone; they are just not
heard. On a phone or a standalone headset the microphone uses the system's
voice preset, which cancels the game's own sound from it. On a desktop, the
game is assumed to be played with headphones.

## Where to look next

- [Mixing](manual:audio/mixing): the listener, and occlusion.
- [Multiplayer](manual:guides/multiplayer).
- [`VoiceChatService`](api:VoiceChatService)
