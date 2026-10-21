# 0256 — Players talk to each other

- Status: accepted and built: the ear (§5), the capture, the gate, the
  wire, the server's routing and cap, the jitter buffer, the playback, the
  service and the phone's permission, behind a codec interface that libopus
  1.6.1 fills. A build without libopus has no codec, and there
  `VoiceChatService.Available` is false and nothing is sent.
- Date: 2026-10-20
- Approved: libopus (BSD-3) as its codec, by the project's maintainer on
  2026-10-21: the latest release, pinned, the codec alone, the copy the
  video player takes too.
- Builds on: ADR 0009 (miniaudio is the seam), ADR 0122 (libopus, approved
  as a dependency), ADR 0205 (an ear a view), ADR 0241 (what a peer may send
  is bounded and said), ADR 0255 (a sound behind a wall is muffled)
- Protocol: 66

## The question

A multiplayer game made with the engine has no voice. A game that wants one
has nothing to build it from: no microphone, no codec, no way to send audio
that is not a message a script made. Every game that wants players to talk
-- a co-op raid, a squad, a party in a lobby -- needs the same thing, and it
is the engine's to give.

And the ear a voice is heard by is the same ear every sound is: the camera.
A top-down game's camera is high above its players, so a voice next to the
player's character is heard from far off, and through a tree's crown.

## Decision

### 1. Who hears whom is the server's

A voice goes from the speaker's machine to the server and from the server to
the players who hear it. The server decides who those are, every frame,
from `VoiceChatService.Mode`:

- `Global`: every player hears every other.
- `Team`: a player hears the players of their own team (`Player.Team`).
- `Proximity`: a player hears those within `ProximityDistance` of their
  character, the voice placed at the speaker's character as a sound is.
- `Script`: the pairs the server's `VoiceChatService:SetCanHear(listener,
  speaker, hears)` named. A declaration rather than a callback: the server
  reads it while it routes a frame, in the middle of taking in the network,
  where calling into a script is a re-entry nothing else does.

`Player.VoiceMuted` (server, written by the game) stops a player being heard
by anyone. `VoiceChatService:SetLocallyMuted(player, muted)` (client) stops
this machine hearing one player, and no other machine knows. Nothing is
heard by the speaker themself.

### 2. What is sent, and how much

- **Opus**, at 48 kHz mono, 20 ms frames, about 24 kbit/s with in-band
  forward error correction: the codec every voice service uses, BSD-3, and
  the one ADR 0122 approved -- this is the first use to vendor it. Built
  with its codec only, no tools or examples.
- **On an unreliable, sequenced channel**, as a `VoiceFrame` message: a
  frame that is late is worth nothing, and Opus covers a lost one.
- **A cap a speaker** (ADR 0241): a server takes at most 50 frames a second
  and 4 KB a second from one player's microphone, and what is over is
  dropped and said once in the log, as any other peer's excess is.
- **Only the server's own codec**: a server relays frames in its build's
  codec and drops any other. A build with none relays nothing, so a client
  cannot make a server pass on raw samples. A raw 16-bit frame is 1920
  bytes, over a frame's 512-byte bound, and is never sent at all; that
  codec exists only in the tests.
- **A jitter buffer on the listener**, 40 ms deep and growing to 120 ms on a
  link that needs it, with Opus's loss concealment for a frame that does
  not arrive.

### 3. The microphone

miniaudio's capture device, opened only while voice is on and the player
allows it. **Push to talk** is the default (`VoiceChatService.PushToTalk`):
the microphone is sent while `Talking` is true, which the game sets from its
own input -- a key, a gamepad button and a phone's on-screen button alike.
With `PushToTalk` false it is sent while the level is above
`ActivityThreshold`, and for 300 ms after, so the pauses inside a sentence
are not cut.

**Echo**: on Android and on a standalone headset the capture is opened with
the system's voice-communication preset (AAudio's `VoiceCommunication`),
which cancels the speaker's sound from the microphone -- a headset's speakers
are a hand's width from it. On Windows, Linux and macOS miniaudio does not
ask the system for its echo cancellation, and the game is assumed to be
played with headphones; it is said in the guide. Cancelling echo in the
engine itself (WebRTC's audio processing, BSD) is a later decision about a
dependency, not taken here.

The permission is asked when voice is first turned on: Android's
`RECORD_AUDIO`, at run time, in the APK of a game that uses voice; macOS's
`NSMicrophoneUsageDescription`.

### 4. Off unless the game and the player turn it on

`VoiceChatService.Enabled` is `false` until the game sets it, and a player's
own setting (`VoiceChatService.PlayerEnabled`, kept with their settings)
must allow it too. The engine records nothing and keeps nothing: a frame is
decoded, played and gone.

### 5. The ear

`AudioService.ListenerMode` chooses where every sound -- and every voice --
is heard from, on each machine:

- `Camera` (the default, as it always was): the camera.
- `Character`: the local player's character, facing the way the camera
  looks, so left is still the screen's left. `ListenerBlend`, 0 to 1 and 1
  by default, moves the ear from the camera (0) to the character (1): a
  top-down game puts it a little above its hero with 0.8.
- `Custom`: `AudioService.ListenerCFrame`, which the game sets.

With several players at one screen (ADR 0205), each view's ear is chosen the
same way. Occlusion (ADR 0255) casts from the ear chosen here.

### 6. A sound that is never muffled

`Sound.Occluded` (default `true`): set `false`, the sound is heard open
whatever stands between it and the ear, with `AudioService.OcclusionEnabled`
on -- an announcer, a radio the player carries. It travels with the sound,
which is why it waited for this protocol.

## Script surface

- `VoiceChatService`: `Enabled`, `PlayerEnabled`, `Available` (read only),
  `Mode` (`Enum.VoiceChatMode`), `ProximityDistance`, `PushToTalk`,
  `Talking`, `ActivityThreshold`, `SetLocallyMuted(player, muted)`,
  `IsLocallyMuted(player)`, `IsSpeaking(player)`, `SetCanHear(listener,
  speaker, hears)` (server), `SpeakingChanged(player, speaking)`.
- `Player.VoiceMuted` (server).
- `AudioService.ListenerMode` (`Enum.ListenerMode`), `ListenerBlend`,
  `ListenerCFrame`.
- `Sound.Occluded`.

## Proof

- Opus round trip: a tone and a recorded phrase come back with their
  energy and pitch.
- Two clients and a server over the link conditioner at 10% loss: a test
  file stands in for one client's microphone, and the other client hears it,
  late by at most the jitter buffer and the round trip, correlated with what
  was sent.
- Each mode: who hears whom with four players, muted, locally muted, a team
  changed, a player walking out of `ProximityDistance`.
- The cap: a client that sends twice the frames is held to the cap, and the
  log says so once.
- The ear: each mode's position and facing, and occlusion cast from it.
- Measured: bytes a second a speaker, and the time from the microphone's
  buffer to the other machine's speakers, on loopback and over the
  conditioner.

Measured, on one machine with 25 ms each way added to each joiner's link
(`netcode_voice`): a frame of speech is about 60 bytes, 3 KB a second. Over
the clean link nothing was concealed, and what each joiner heard had a
spectral similarity of 0.93 and 0.96 to the phrase the other spoke, against
0.20 and 0.24 to its own. With a tenth lost both ways on one link and 10 ms
of jitter, 9 and 11% of the frames were concealed, and the similarity was
0.91 against 0.29 and 0.25. From the speaker's microphone to the listener's
ear took 109 to 111 ms over the clean link and 123 to 127 ms over the lossy
one: a captured frame, the 50 ms of round trip, and the jitter buffer.

## Not decided here

- **Echo cancellation in the engine**, for speakers on a desktop.
- **Voice on the web**, which takes the browser's microphone and its own
  echo cancellation, with the web category.
- **A console's party chat**, which a platform provider (ADR 0188) gives.
- **Noise suppression** beyond Opus's own.
