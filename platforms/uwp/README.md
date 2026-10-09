# Xbox Developer Mode platform probe

This is a platform feasibility app, not the Ludwerk player or Hordewake.
It uses CoreWindow, Direct3D 11/Direct2D, the pinned sandboxed Luau interpreter,
packaged script/catalog, Windows.Gaming.Input and application-local save data.
No SDL3 or GDK integration is included. Console execution must be verified on
hardware; desktop validation alone does not establish Xbox compatibility.

## Build

On Windows with Visual Studio C++ UWP tools and Windows SDK 10.0.26100.0:

```powershell
./scripts/package-xbox-uwp-probe.ps1
```

Outputs default to the external build root under `xbox-uwp-probe`. Override with
`-Out <external-folder>`. The script packages an x64 APPX, signs it with a local
development certificate, and supplies its public CER and the installed SDK's
x64 UWP VCLibs runtime. The private key stays non-exportable in CurrentUser/My.
No trust roots or Developer Mode settings are changed. These are local test
artifacts, not a Store release or permission to redistribute SDK contents.
Use `-PackageVersion 0.1.0.5` (then increase it for later changes) when updating
an already installed probe. Changing payloads while reusing a package version
caused stale installation/activation failures on the test console.

## Install on the retail Xbox in Developer Mode

1. Open the URL from Dev Home > Remote Access in a PC browser on the same LAN.
2. In Device Portal > Home > My games & apps, choose **Add**.
3. Select `engine-uwp-probe-x64.appx`. At the dependency step, select
   `Microsoft.VCLibs.x64.14.00.appx` if the console does not already have it.
4. Follow the portal's installation steps. Xbox package deployment does not
   require uploading the CER; it is supplied for desktop development validation.
   Keep the private key on the build machine. Record any exact deployment error.
5. Launch the probe from Dev Home or the application's portal action menu.

Expected: dark screen with status text and a green circle. Stick/D-pad moves it;
A highlights it; B resets its position. Disconnect/reconnect the controller.
Suspend and resume, then restart: position should persist. The app-local
`probe.log` records asset/VM readiness, first presentation, first physical
gamepad input, suspension, resume and graphics device recreation/errors.
Use Device Portal file access to collect app-local logs when available.

On desktop, arrows move and Space resets. Launch argument
`--self-test` exits after 120 rendered frames and emits a smoke-test marker;
it does not simulate or certify physical gamepad input.

## Hardware checkpoint (2026-10-09, package 0.1.0.4)

The owner's Xbox rendered the probe and ran its packaged sandboxed Luau script.
The owner confirmed movement, A highlight, corrected B reset without closing,
and controller disconnect/reconnect. Suspension saved a nondefault position,
which was restored on cold relaunch and checked in console screenshots.
Live in-process resumption and forced GPU device-loss recovery remain unverified.
The portal's explicit resume command failed; activating the app again cold-launched
successfully. PC registration was blocked by local sideload policy; no PC policy
or certificate trust setting was changed.

## Remaining work after the hardware checkpoint

Port the actual platform and RHI interfaces, InputService binding, audio,
filesystem/network/lifecycle integration, then validate the full player and
Hordewake. Native Xbox GDKX is a different target with separate SDK/access
requirements. Existing PC GDK services do not become console services here.
