# 0188 - Platform tools and game integrations are optional

- Status: accepted
- Date: 2026-10-09
- Decided by: the owner in the platform manager discussion and implementation request.

## Decision

Extend the existing exporter. Windows and Linux use the installation's prebuilt
players. Android tooling and Microsoft GDK tooling are opt-in, shared across
projects, and version pinned. Installing tools and enabling an integration in a
project are separate operations. Editing scripts never requires an SDK.

Optional services are declared in the public API schema, including the integration
that owns them. GetService remains usable when disabled: queries return empty
values and actions return an explicit failure. Inactive asynchronous calls finish
immediately. The editor warns on disabled service references, and does not offer
those services as new suggestions. Diagnostics remain available for existing code.
Warnings are once per service per VM, development only. No simulated success.

Native providers are separate dynamic libraries, loaded only for an enabled
integration when used. Default players do not link or ship GDK libraries. GDK
2604.5.7903 is an optional development dependency, never part of the permissive
engine's default dependency graph. SDK files are downloaded from Microsoft/NuGet
and remain outside the public source tree. Restricted console SDKs remain private.

The initial service is XboxService (PC identity and achievements). StoreService
and GooglePlayService are future integrations, not nonfunctional promises exposed
as active platforms. Xbox console is not a Windows export: it remains unavailable
until a console backend, licensed tools and lifecycle requirements are validated.

## Verification

Hold the inactive contract, project settings, disabled-service diagnostics and
module state through tests. Native integration is compiled against the pinned
public SDK. Account authentication, achievements and Store submission require a
configured Microsoft title; record what was actually exercised, not a mock as an
account test.
