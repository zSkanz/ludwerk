# ADR 0191: Xbox Developer Mode starts with an isolated UWP probe

Date: 2026-10-09
Status: Accepted by the owner's instruction to proceed with the prototype.

The retail Xbox is in Developer Mode. The current Xbox adapter targets Windows
PC GDK; it is not a console runtime. Vendored SDL3 explicitly dropped UWP.
Installing the existing Windows player through Device Portal cannot bridge that
gap, and public GDK does not provide console GDKX access.

First validate a native CoreWindow UWP host with Direct3D 11, Windows.Gaming.Input,
packaged assets, app-local storage and suspension/resumption. Reuse the pinned
Luau interpreter with its sandbox and the engine catalog reader. Keep all WinRT
types inside `platforms/uwp`. This target is opt-in, requires WindowsStore and
MSVC, and does not enter shipping builds or claim to implement the engine RHI,
InputService, generic platform services or Hordewake.

Build outputs and local development signing keys stay outside the repository.
SDKs are external installations; this adds no vendor download or runtime SDK
dependency to default builds. Test the resulting APPX on desktop where possible,
then on the retail console through Device Portal. Console rendering, physical
input and lifecycle checks are a required hardware checkpoint before extending
this into a real player port. GDKX remains a separate future target requiring
authorized console tooling.
