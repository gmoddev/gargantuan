---
title: EntitlementService
description: Public creator reference for EntitlementService.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Checks provider-neutral semantic entitlements for runtime-owned Player identities.

Kind `Service`. Abstract `False`. Service policy `Default`.

Access through `game:GetService("EntitlementService")` subject to the service policy and runtime context.

Inherited members from [Instance](./instance.md).

## Properties

## Methods

### EntitlementService.CheckAsync

Checks one semantic entitlement for a Player without accepting caller-supplied identity.

```text
(self: EntitlementService, player: Player, entitlementId: string) -> { Status: "Granted" | "Denied" | "Unavailable", EntitlementId: string, Identity: { Provider: string, Subject: string }, ExpiresUnixMilliseconds: number? }
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### EntitlementService.CheckManyAsync

Checks up to 32 semantic entitlements for a Player.

```text
(self: EntitlementService, player: Player, entitlementIds: { string }) -> { { Status: "Granted" | "Denied" | "Unavailable", EntitlementId: string, Identity: { Provider: string, Subject: string }, ExpiresUnixMilliseconds: number? } }
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
