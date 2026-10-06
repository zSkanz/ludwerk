# 0178 — Reaching a host behind a NAT: a rendezvous, a punch, and a relay that reads nothing

- Status: accepted
- Date: 2026-10-05
- Decided by: the agent, bringing ADR 0120 §4 forward on the owner's request
  (through the coordinator) that players find and join each other's matches
  over the internet with no VPN, under the standing rule to decide as
  professional engines do and record it
- Builds on: ADR 0120 §4 (the relay and the hole punch, decided in outline),
  ADR 0012 (the transport seam), ADR 0106 (`Join` and `Host` from a script),
  ADR 0167 (`MaxPlayers`, `Kick`)
- Leaves where it is: ADR 0120 §1 to §3 (the sealed transport, a server and a
  player as keys)

## Context

A match hosted from a player's home is behind a NAT, and often behind two:
the home router's and the carrier's. Nobody outside can send it a first
packet. The owner's players run a VPN to meet. ADR 0120 §4 decided the cure in
three lines -- a relay program, hole punching through its rendezvous, the
relay as the fallback, all inside `ITransport` -- and left the rest to the
stage that builds it. This is that stage, brought ahead of the encryption it
was listed with.

What it has to do, from the brief:

- one small program, one UDP port, headless, on Windows and Linux;
- a host registers and is given a short code to hand out;
- a joiner with the code is connected by the best path there is: the same
  network, a punched path, or the relay -- without the game's help -- and the
  game is told which;
- a relay per region that knows no other relay, answers a ping before any
  session exists, and says how much it carries;
- a host's refusal of a player holds through the relay.

## How mature engines and services do it

- **Steam Datagram Relay / GameNetworkingSockets**: rendezvous through a
  signalling service, ICE-style candidate exchange, a path each to the other
  where it opens, relays otherwise; the application sees one connection.
- **Unity Relay** and **Epic's P2P**: a join code allocated by the relay
  service; hosts keep an allocation alive; data relayed, direct where the NAT
  allows (Epic), always relayed (Unity Relay).
- **WebRTC / ICE (RFC 8445), STUN, TURN**: gather candidates (host, server-
  reflexive, relayed), check them in pairs, keep the best. The general
  solution, sized for browsers.
- **ENet-based games** commonly: a small "NAT punch-through server" that
  introduces two endpoints, with a relay or nothing as the fallback.

Everything above is the same three steps: learn each side's public address
from a server both can reach, have both sides send to each other at once,
relay when that fails.

## Decision

### 1. One program, one port, three jobs

`engine-relay` (`tools/relay`: its own executable, the network module and what
that links, no window, no GPU, its messages carried inside it) listens on one
UDP port, 7789 where nobody says another:

- **rendezvous**: a host registers, a joiner looks a code up, each is told the
  other's address;
- **relay**: where no path each to the other opens, it forwards datagrams
  between a joiner and its host;
- **ping and count**: it answers a ping, and the answer says how many matches
  and bytes a second it carries.

It keeps no file and no account, and knows no other relay. The relay itself is
`rendezvous::RelayServer`, told what arrived and when and answering what to
send: no socket and no clock in it, so a test stands it in a network of
routers written down for the purpose, which is the only way to test what a
router does to any of this. `net::RelayService` is that on a socket.

### 2. Not ICE, STUN or TURN

The messages are the engine's own, sixteen fixed layouts under a four-byte
mark. The standard protocols would bring candidate pairing, attributes,
long-term credentials and a TURN allocation per peer for a problem that here
has two parties, one socket each, and a server they both already trust to
introduce them. A managed TURN service was weighed for the owner and declined
by him: the relay is a program he runs.

### 3. A join code is the host's own, not the relay's

A host draws sixteen random bytes once a session, its **host token**. Its
**join code** is the first forty bits of the token's SHA-256, written as eight
characters of an alphabet with no look-alikes (no 0, 1, I or O). The relay
checks that a registration's code is its token's -- so:

- nobody can register someone else's code without the token;
- the relay needs no memory of codes: after a restart the host's next
  keep-alive registers the same code again;
- a code stops resolving thirty seconds after its host's last keep-alive, and
  at once when the host says it has stopped.

A code is not a secret -- it is what a player reads out to a friend -- so
forty bits are for telling matches apart. Two tokens that share a code are
possible to make on purpose; the one registered first keeps it while it lives.

### 4. The path is found on the match's own socket

Every rendezvous message, every knock and every direct packet uses the
transport's one UDP socket, so the mapping a knock opens in a router is the
mapping the match then uses. ENet lets a host see a datagram before it parses
it (`intercept`); the transport takes the marked ones there and sends its own
through ENet's socket. ENet is not patched. (ENet reads the mark's first bytes
as a compressed packet, and the transport compresses nothing: no datagram of
a match begins so.)

A joiner, in order, without the game's help:

1. **Lookup**: asks the relay for the code. No such match, a host that says
   it is full, or a host that is keeping this address away, ends here.
2. **Same network**: when the relay sees both from one public address, it
   gives each the other's own addresses, and both try those too. Two phones
   on one Wi-Fi do not go round by the relay.
3. **Knock**: both sides send to each other ten times a second. A host
   answers a knock only from somebody the relay introduced, and answers to
   where the knock came from -- which behind some routers is not where the
   relay saw the joiner; and a joiner knocks back at wherever the host's
   knock came from, for the same reason the other way. The first answer names
   the path: `lan` from one of the host's own addresses, `direct` otherwise.
4. **Relay**: with no answer in two and a half seconds, the joiner asks for
   the relay and ENet connects to the relay's own address.

What that reaches, as the tests have it: two machines the internet reaches,
two home routers of the ordinary kinds, and a router that opens a port per
destination on one side where the other side's lets anyone in to an open
port -- each to the other. Such a router against one that lets in only whom
it wrote to, and two such routers: the relay.

### 5. The relay forwards by where a datagram came from, and adds nothing

A relayed joiner sends its ordinary ENet datagrams to the relay's port; the
relay knows it by its source address. For the way back, the host opens **one
more UDP socket per relayed joiner** and says its slot from it, so the relay
knows that socket's public address as that joiner's; in the host's process
the socket is a pipe to and from ENet's own, from a loopback address of its
own (127.a.b.1, the slot in a and b), so ENet sees one peer per joiner as it
would across the internet.

So a relayed datagram is the datagram: no header, no byte added, nothing for
the relay to parse. A sealed packet (ADR 0120 §1) will pass through it as it
is. The alternative -- a header naming the joiner on every packet -- needs
ENet's send path rewritten for relayed peers, which is a patch to carry, and
bytes on every packet for ever.

The host's socket says its slot again every three seconds, with the joiner's
address the relay first gave it. A relay that was restarted answers that it
knows no such match; the host registers again at once, and the slot is the
relay's again from the next saying -- so a relay's restart is three or four
seconds of silence to a carried player, inside ENet's timeout, and not the
end of the match.

### 6. What a host refuses stays refused

When a relayed player goes -- it left, the host let it go, it timed out -- the
host tells the relay to close the slot, with for how long that address may not
look the code up again (`ITransport::ban`; told when the player is gone, since
the host's goodbye to it crosses the relay too). A host with no room says so
in its registration, and the relay refuses lookups for it without knocking on
it. The relay cannot be used to reach a match its host does not want reached.

### 7. Bounded

A relay caps its matches, the carried players of a match and of one address
in a match, and the bytes a second each carried player sends each way; it
limits lookups and registrations by source address and introductions by
host; and it never answers with more bytes than it was sent -- a lookup and a
ping are padded to their answers' size, and one cut short is not answered --
so it amplifies nothing.

Forwarding one datagram costs the relay's own code about twenty nanoseconds
(measured: four thousand carried players of a thousand matches, 47 million
datagrams a second on one core of the development machine with the sockets
left out). What a relay carries a second is therefore what its sockets carry:
two system calls a datagram.

### 8. The script's side

- `NetworkService:Host(port?, { Relay = "host:port" })`, with `[network]
  relay` in `project.toml` and `--relay=` on the command line as the relay a
  call names none over. Hosting works on the local network, and by address,
  whether or not the relay answers.
- `NetworkService.JoinCode` -- empty until the relay has registered the host.
- `NetworkService.RelayState` (`Enum.RelayState`: `None`, `Connecting`,
  `Ready`, `Unreachable`) and `RelayStateChanged`.
- `NetworkService:Join(code, { Relay = "host:port", Direct = false })`: where
  a relay is known and the text is a code -- as a player types one, either
  case, a dash or spaces between -- it is a join by code; otherwise an
  address, as before. `Join("relay://host:port/CODE")` says both in one
  string. A host whose name could be read as a code is written with its port.
- `GetStats().Path`: `"lan"`, `"direct"` or `"relayed"`; on a host, the
  longest way any of its players came.
- `JoinFailed`'s reason gains: the relay did not answer, no match has that
  code, the host is keeping this machine away, the relay is at its limit, no
  path opened in time.

Nothing here is on the wire of a match: the protocol's version is unchanged,
and the rendezvous has a version of its own.

**Amended 2026-10-05 (D566): a carried player's allowance holds eight seconds
of its rate.** It held one, and the first relay in service dropped a third of
every join: a small scene's world is a megabyte and a half sent in a second,
against 512 KB held and 512 KB earned. The rate over any long stretch is the
rate it was; `--burst` says another number of seconds.

**Amended 2026-10-05 (D562): a relay's name is looked up until it is found.**
The first relay installed for a game had a name made minutes before, and a
host whose resolver did not know it yet was `Unreachable` for its whole
session: the name was asked for once. It is kept and looked up again every
half minute, on a thread of its own, and the host registers when it answers.

## What it does not do

- Encryption and identity: ADR 0120 §1 to §3, where they were. A relay sees
  what a match sends, as every router between two players does.
- IPv6. ENet's addresses are IPv4 and so are these.
- A room list, names, passwords: a directory's, which is a game's service.
- UPnP or NAT-PMP port mapping.
- A call for a script to ping a relay before it joins: the relay answers a
  ping and says what it carries, and nothing in the script API asks yet. A
  game with relays in several regions needs it to choose one.
- The link conditioner (`--net-delay`) on a join by code.

## Consequences

- One UDP port open on the machine that runs the relay.
- A carried player costs the relay what the match sends that player, both
  ways, and nothing more: at the game measured, about fifty kilobytes a
  second for a two-player match.
- A host opens one more UDP socket, and one thread for all of them, when the
  first player the relay must carry arrives; none before.
- On macOS a host's pipes are all 127.0.0.1 -- the system has no other
  loopback address -- so a host's own limit of connections an address counts
  every carried player as one address there; the relay's limit of carried
  players an address is what holds.
