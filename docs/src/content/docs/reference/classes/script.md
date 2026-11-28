---
title: Script
description: Public creator reference for Script.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Class that runs the source code in an asynchronous thread

Kind `Class`. Abstract `False`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [LuaSourceContainer](./luasourcecontainer.md).

Inherited members from [Instance](./instance.md).

## Properties

### Script.Enabled

Whether this script is actively running

```json
{
  "Type": "t.Bool",
  "Unmodified": "true",
  "OverrideRead": "GetEnabled",
  "OverrideWrite": "SetEnabled",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Script.RunContext

Whether to run this script as the client or server.

```json
{
  "Type": "t.EnumItem(\"RunContext\")",
  "Unmodified": "Enums::RunContext::None",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
