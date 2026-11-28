---
title: ModuleScript
description: Public creator reference for ModuleScript.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Class that contains a Lua module which can be required at runtime

Kind `Class`. Abstract `False`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [LuaSourceContainer](./luasourcecontainer.md).

Inherited members from [Instance](./instance.md).

## Properties

### ModuleScript.ReplicatedSource

Engine-only projection of source explicitly stored in a replicated service

```json
{
  "Type": "t.String",
  "Unmodified": "std::string()",
  "OverrideRead": "GetReplicatedSource",
  "OverrideWrite": "SetReplicatedSource",
  "Replicated": true,
  "Editable": false,
  "ReadLevel": "Engine",
  "WriteLevel": "Engine"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
