# Xbox Developer Mode prototype

Owner authorization: proceed with the minimal UWP prototype before Hordewake.
Decision: [ADR 0191](../decisions/0191-xbox-developer-mode-starts-with-an-isolated-uwp-probe.md).

## Current checkpoint

The owner has opened Xbox Device Portal successfully on the local network.
No credentials have been requested or saved. The external SDK selection UI is
deferred by the owner. Existing platform services target Windows PC GDK, and
existing Windows/Linux/Android players remain separate from this prototype.

The opt-in WindowsStore x64 target compiles with MSVC 14.51 and SDK 10.0.26100.0.
MakeAppx validation and development signing passed. Device Portal accepted the
APPX and UWP x64 VCLibs dependency and reported installation success. The probe
has rendered on the owner's retail Xbox: a 1920x1080 portal screenshot was
inspected, and app-local logs confirm the real pinned VM, packaged script/catalog,
graphics and physical gamepad input. The owner confirmed movement and A.

The initial STA initialization failed with RPC_E_CHANGED_MODE; using the proper
MTA entry thread fixed it. Reusing a changed package at the same version caused
stale activation/install failures; the packaging command now accepts an explicit
four-component version. Current artifact: **0.1.0.4**. The owner reported B might
close the app: UWP maps it to Escape, which the initial desktop shortcut used to
quit. Removed that shortcut, consume system Back, and prevent duplicated native
gamepad/emulated keyboard processing. The corrected artifact is installed and
running; the owner confirmed that B now resets without closing the app and that
controller disconnect/reconnect works.

Desktop registration was attempted but blocked by local sideload policy
(`0x80073CFF`). No PC Developer Mode or certificate trust settings were changed.
Actual Xbox execution supplies the runtime evidence instead. Default Windows
configure/build and the docs gate passed after this addition. Linux CTest passed
145/145 (475.34 s), conformance passed 1642/1642 and hot reload passed 3/3.
The complete Linux gate passed in 536.5 s. No commit or push was performed.

## Completion checklist

- [x] Compile the opt-in WindowsStore x64 target using the installed Windows SDK.
- [x] Validate and sign a development APPX outside the source tree.
- [x] Supply the x64 UWP VCLibs dependency and public development certificate.
- [x] Check package contents; desktop blocked by policy, Xbox runtime checked.
- [x] Install through Device Portal; confirm presentation and physical controller.
- [x] Confirm physical controller disconnect/reconnect (owner).
- [x] Confirm suspension saves position and cold relaunch restores it.
- [ ] Confirm live in-process resumption; the portal suspend test cold-relaunched.
- [x] Record concrete logs, artifact paths and remaining player-port work here.

## Evidence and continuation

- Build: `%LOCALAPPDATA%/Ludwerk/build/uwp-x64-probe`.
- APPX/CER/dependency/instructions: `%LOCALAPPDATA%/Ludwerk/build/xbox-uwp-probe`.
- Delivered copy: `%USERPROFILE%/Downloads/Ludwerk-Xbox-UWP-Probe`.
- APPX 0.1.0.4 SHA256: `7f940e3ebfaed211ff61b68a54e12269b86c1b7b59099300b47757dc47e5f911`.
- Build/package log: `%TEMP%/ludwerk-uwp-package.log`.
- APPX inventory contains only the probe executable, script, catalog, logos,
  engine/Luau licenses and packaging metadata. No SDK DLL, SDL, GDK or private
  signing key. CMS cryptographic signature validation passed without changing
  root trust. The single reusable private key is non-exportable in CurrentUser/My.
- i18n lint passed, 3848 references/2650 catalog keys; probe StyLua and strict
  Luau LSP analysis passed. C++ formatted with pinned clang-format 18.
- Default Windows log: `%TEMP%/ludwerk-uwp-default-windows.log`.
- Linux/docs logs: `%TEMP%/ludwerk-uwp-{linux,docs}.log`.
- Docs gate passed (178.2 s). Complete Linux gate passed (536.5 s): CTest 145/145,
  conformance 1642/1642, hot reload 3/3, no skipped tests.
- App-local `probe.log` includes `UWP_HOST_INITIALIZED`, `UWP_WINDOW_READY`,
  `UWP_LOAD_BEGIN`, `UWP_LUAU_ASSET_READY`, `UWP_PRESENT_READY`,
  `UWP_GAMEPAD_INPUT_READY`, and a suspension observed after the owner's test.
- Portal API file paths require a leading slash, e.g. encoded `\LocalState`.
  Installation acceptance is asynchronous; poll completion before launching.
- Owner moved the circle off center before suspension: `position.txt` saved
  `0.258321 0.494464`. Before/after cold-relaunch screenshots both locate its
  center at `(495.5, 533.5)` on a 1920x1080 frame, within one pixel of the saved
  normalized coordinates. Evidence is under the artifact folder's `evidence/`.
- `UWP_SUSPENDED` and subsequent fresh initialization were observed. The portal's
  explicit resume request returned Element not found (`0x8002802b`); launching
  again succeeded and restored position. There is no `UWP_RESUMED` proof yet;
  do not report live resumption or forced GPU device-loss recovery as validated.

Use [the probe instructions](../../platforms/uwp/README.md) for rebuild/install.
Follow the official [Device Portal REST reference](https://learn.microsoft.com/en-us/windows/uwp/debug-test-perf/device-portal-api-core)
and [gamepad key mapping](https://learn.microsoft.com/en-us/windows/uwp/ui-input/gamepad-and-remote-interactions).
No account identifiers, credentials, console logs unrelated to this app, or
private signing keys belong in the repository or redistributable test folder.

The probe uses the pinned sandboxed Luau VM, real packaged script/catalog,
Direct3D/Direct2D, Windows.Gaming.Input and app-local save data. It does not load
SDL3, GDK DLLs, JIT or simulated engine services. A successful probe is evidence
for the platform host only; engine RHI, audio, filesystem, network, service/input
bindings and full game compatibility still require a proper port.

## Next implementation boundary

Keep this probe as the known-good platform baseline. Audit the actual player
dependency graph before introducing a UWP exporter: SDL3/CoreWindow bootstrap,
native window ownership in the RHI, InputService event bridge, app-local/package
filesystem paths, audio/network API eligibility and manifest capabilities,
suspension and graphics recovery. Preserve the current deferred input/service
semantics and deterministic simulation; do not copy this small demonstration
host into the player as though it already satisfied those contracts.

PC GDK provider binaries must not enter an Xbox UWP export. Unsupported generic
services must keep their explicit unavailable behavior until a compatible real
provider exists. Only add a selectable export target after a real player can load
an engine scene and accept input on hardware; validate Hordewake after that.
External SDK discovery UI remains deferred by the owner.
