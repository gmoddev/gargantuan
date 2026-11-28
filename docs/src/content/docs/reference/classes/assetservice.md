---
title: AssetService
description: Public creator reference for AssetService.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Resolves stable project and built-in asset references through one bounded catalog.

Kind `Service`. Abstract `False`. Service policy `Default`.

Access through `game:GetService("AssetService")` subject to the service policy and runtime context.

Inherited members from [Instance](./instance.md).

## Properties

## Methods

### AssetService.GetAssetMetadata

Returns bounded semantic metadata for a known asset, or nil.

```text
(self: AssetService, reference: string) -> { [string]: any }?
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### AssetService.IsAssetAvailable

Returns whether a reference has a valid last-known-good canonical asset.

```text
(self: AssetService, reference: string) -> boolean
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### AssetService.ResolveAsset

Returns the canonical available asset reference, or nil.

```text
(self: AssetService, reference: string) -> string?
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
