# Hordewake local cooperative console port

Status: first integrated co-op Release exported and installed on Xbox as
0.8.49.35. Two/four-player windowless PC scenarios passed. Console startup
verified; physical multi-controller validation is pending owner feedback.
This is not a declaration that the complete console port is finished.

Owner requirements:
- Complete console interaction, not only movement/menu adaptation.
- Up to four controllers on one PC, Xbox or Android device.
- One independent hero per local player, shared screen.
- Higher shared camera, smooth fit/zoom according to player separation.
- Independent simultaneous card hands displayed in separate player areas;
  pause the simulation until every outstanding choice is confirmed.
- Keep solo, online multiplayer, current graphics and prior optimizations.

Current constraints: PC tests remain windowless, without pointer capture. Phone
access remains withdrawn. Owner re-enabled Xbox access for export/deployment.
Do not claim physical multi-controller success until the owner confirms it.

Initial audit gaps (historical; implementation checkpoint below supersedes these):
- InputObject payload currently carries no gamepad instance id.
- Controls.new creates unfiltered InputActions, currently aggregated gamepads.
- PlayerController owns one control set, one LocalPlayer hero and third-person
  camera. Server hero/run systems already support multiple network Players.
- NetworkService.LocalPlayer is singular; local seats need explicit ownership,
  safe authoritative routing and lifecycle, not forged network player ids.

Implementation order:
1. Finish/validate current terrain/loading reuse changes separately.
2. Extend existing input snapshots, InputObjects and action filtering for
   per-device reads; preserve replay, UI consumption and disconnected states.
3. Add local participant ownership/routing compatible with existing multiplayer
   architecture; reject unauthorized remote impersonation and cap four seats.
4. Lobby join/leave, per-seat hero and ready state, reconnect/reservation.
5. Independent movement/actions plus camera framing/limits and player HUD.
6. Simultaneous card panels and independent focus/confirmation, common pause
   ownership, end/retry/leave flows, per-seat death/revive and persistence.
7. Automated two/four-controller scenarios, regressions for solo and network,
   shared camera aspect ratios and choices; Release PC artifact. Console and
   Android device validation deferred until owner makes devices available.

Do not describe this checklist as completed. This is a large gameplay/input
change, not a generic camera toggle. User selected simultaneous independent
card choices in the follow-up reply on this date.

## Handoff: engine foundation and first gameplay integration

Implemented in the working tree (after the separate loading10 Release export):
- Per-controller snapshots, InputObject.GamepadId, GetGamepads and per-controller
  button polling; InputContext.GamepadId/Player; independent sinks and tap latches.
- NetworkService local guest inventory/add/remove for offline authority only.
  Primary identity preserved, remote-peer impersonation refused, hosting/joining
  refused while local guests exist. Hybrid local+online parties are not supported.
- RemoteEvent/UnreliableRemoteEvent FireServerFor and directed LocalClientReceived.
- RunService BindToIntent optional local Player ownership.
- UIService.GamepadId filters shared menu navigation; zero remains aggregate.
  A guest binding does not reserve the primary controller's menu controls.
- Windows input: 49 cases / 1004 assertions; script full suite: 192 / 1617;
  UI full suite: 158 / 71636; app navigation: 9 / 90, passed.
- Linux input and script passed the same counts before the latest UI filter;
  rebuild/retest the latest engine changes on Linux still required.

Game source now has LocalSeats (pure ownership/reservation), LocalParty module,
per-device Controls options, independent guest intent writers and Hello messages,
local lobby explicit Menu join / hero selection / ready / leave, shared camera
frustum fitting and shared offline pause. Strict source analysis passed before the
latest pause/lobby updates; rerun after completing choices.

Pure math tests: party-camera.test.luau 207364 checks (2-4 players, 9:16,
4:3, 16:9 and 21:9, movement/spawn and zoom-in). local-seats.test.luau passed
ownership, cap, disconnect, explicit reclaim and removal cases.

Outstanding before claiming a complete console port:
- Simultaneous per-player cards with independent focus/serial/confirmation.
- Guest interactions (chests/merchant), health/weapon HUD and directed Me/End.
- Reconnect must reclaim only an existing seat during a run (no late new seats).
- Menu return must remove local guests before online host/join; lifecycle/retry.
- Shared-view separation limits, terrain occlusion, downed-player behavior.
- Scene-transition persistence and end-to-end two/four-device replay scenarios.
- Solo/network regression, visual captures, Release packaging, console/mobile
  validation when devices become available. No interactive PC launches/capture.

Do not confuse loading10 Downloads artifacts with these unshipped co-op changes.

## Release/export checkpoint: 0.8.49.35

Implemented since the preceding handoff:
- Simultaneous per-seat card panels, independent controller focus, grace period,
  serial/request validation, reroll/skip and authoritative confirmation. World
  remains paused until all outstanding local choices are resolved.
- Guest directed Me/End data, weapon/health HUD, chest/portal/challenge/merchant
  actions, guest personal FX without duplicating shared world effects.
- Run reconnect reclaims reserved seats only; disconnected seats pause offline
  play. Menu return clears guests. Camera fit includes downed heroes and terrain
  clearance; movement tether keeps the shared view bounded.
- Scene transition retains each local native Player/hero owner. Lobby hero/ready
  and shared pause tested in isolated two/four-player fixtures.

Validation:
- Windows and Linux: input 49 cases/1004 assertions; script 192/1617;
  UI 158/71636; navigation 9/97, all passed. Windows and UWP Release builds passed.
- Shipping src strict Luau analysis passed after final changes (source-analysis.log).
- LocalSeats, PartyMovement, LocalChoices pure tests passed; PartyCamera 207364
  checks across 2-4 players and four aspect ratios passed.
- Two/four-player PC headless fixtures passed ownership, independent movement,
  pause/resume and simultaneous choices including first confirmation holding
  the world until the last seat confirms. Screenshots reviewed. These use a
  fixture-only synthetic device inventory, not physical controllers. The two
  fixture inherited some log labels saying four; actual roster assertions are two.
- Full CLI check still sees pre-existing marketing capture unused-RunService
  errors; do not report the entire repository check as passing.

Artifact: C:/Users/juanr/Downloads/Hordewake-0.8.49-coop-xbox/
APPX: Ludwerk.Hordewake.Dev-0.8.49.35-x64.appx (91,296,487 bytes).
SHA256: f24f5989a6332ca23a7e7876364da66072a1fd638ea4cea5aababca4c5527e11
Packaged from build/hordewake-coop-export-v1, current native UWP Release player;
local development signing succeeded. Updated the existing normal Hordewake
identity, preserving its local storage. Benchmark/profile packages untouched.
Device Portal upload/install success and launch HTTP 200. player-entry.txt reports
memoryBudgetBytes=5368709120 (5120 MiB/Game). Subsequent menu samples roughly
59.9-60.3 FPS, no error/fatal entries in the captured startup log. Cold startup
still logged 3.46/4.27-second stalls, 13.2-second loading and a loading-readiness
warning: do not describe loading performance as solved or these samples as a
co-op gameplay benchmark.
Evidence: C:/Users/juanr/Downloads/Hordewake-Local-Coop-results/ (logs/captures).

Remaining work / honest limits:
- Physical 2-4 controller Xbox validation requested; await owner feedback.
- Offline co-op only; mixed local+online intentionally rejected by engine guards.
- Guest North/Back currently toggles the primary stats sheet; add explicit sheet
  ownership before declaring every console interaction complete.
- Party cards keyboard/mouse reroll/skip affordances and two-player panel spacing
  can be improved. End/retry/revive, disconnect/replacement and merchant flows
  still need physical multi-controller QA; no Android validation this batch.
- Investigate cold console loading stalls/readiness warning separately, without
  removing quality or hiding stalls behind an FPS average.


### Xbox controller ownership labels: 0.8.49.36
Owner confirmed independent hero movement on Xbox with two controllers. This
confirmation does not establish physical card/reconnect/four-controller QA.
Owner reported missing lobby selection ownership indicators; corrected:
- Local figure billboards say localized Player/Jogador 1-4, including primary
  before an extra controller joins. Online nicknames remain unchanged.
- Portraits show localized P1/J1 etc., including multiple owners of one hero.
- Local readiness uses the actual Ready attribute, not host status. Reserved
  disconnected controllers show reconnect status.
Shipping Luau source strict analysis passed. Four-seat windowless lobby capture
exit 0, visually reviewed: shared portrait P1/P2, separate P3/P4, ready/choosing
states match fixture setup. Evidence: lobby-label-capture.png/log and
lobby-label-analysis.log in Downloads/Hordewake-Local-Coop-results.
Re-exported and signed normal Xbox APPX 0.8.49.36 in the same Downloads folder.
Deployment 0.8.49.36 completed successfully; launch HTTP 200 and engineHostMain entry confirmed, Game budget 5368709120 bytes. APPX SHA256: 637f2091e809a8347f615855776a0b5b2bfe8b77d5b5f1159be95bac17f4ed0a.
