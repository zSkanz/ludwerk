# ADR 0193: The native UWP player uses an optional D3D12 backend

Date: 2026-10-09
Status: Accepted by the owner's instruction to finish the Xbox player port.

The isolated UWP probes in ADR 0191 proved CoreWindow, controller input, local
storage and the existing shipping DXIL shaders on the owner's Series S. The
vendored SDL3 cannot host UWP, so exporting the desktop executable is insufficient.

Add an opt-in `ENG_BUILD_UWP_PLAYER` build graph using the same `engineHostMain`,
world, scripts, renderer, input actions, audio, physics and multiplayer modules.
Only the platform boundary changes: native CoreApplication/CoreWindow,
Windows.Gaming.Input, application-local storage and Windows.Web.Http. Unsupported
desktop tooling reports unavailable. Windows types stay private to the native
implementation and the narrow opaque `windows_interop` presentation boundary.

An opt-in `ENG_RHI_D3D12` backend implements the existing IDevice/ICmdList seam;
no second renderer, game loop or shader language is introduced. DXIL comes from
the host shader build. Descriptor snapshots and copied uniform uploads remain
alive until submitted GPU work finishes. Initial resource retirement deliberately
waits for idle; future frame-fenced arenas must retain that ownership guarantee.

Default desktop/Android/Linux backends remain unchanged. UWP excludes SDL and
unused native Luau CodeGen, retains the interpreter/compiler and multiplayer,
and resolves all SDK/build tools externally. APPX packaging consumes an existing
sealed game export and host content, signs with a local non-exportable development
key, and stages outside the repository. This target is Developer Mode UWP, not
GDKX or a claim of console Store certification.

Hardware menu rendering alone is insufficient acceptance: scene transitions,
physical controller gameplay, suspension/resumption, saves and measured Game-mode
performance must be recorded before declaring the player finished.
