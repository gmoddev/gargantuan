---
title: LuaSourceContainer
description: Public creator reference for LuaSourceContainer.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Abstract class for compiling Lua bytecode

Kind `Class`. Abstract `True`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [Instance](./instance.md).

## Properties

### LuaSourceContainer.GuiTemplateSourceDigest

Engine-owned digest of the trusted StarterGui source bytes bound to this clone.

```json
{
  "Type": "t.String",
  "Unmodified": "std::string()",
  "OverrideRead": "GetGuiTemplateSourceDigest",
  "OverrideWrite": "SetGuiTemplateSourceDigest",
  "Replicated": true,
  "Editable": false,
  "ReadLevel": "Engine",
  "WriteLevel": "Engine"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### LuaSourceContainer.GuiTemplateSourceId

Engine-owned provenance for a PlayerGui clone of a packaged StarterGui source.

```json
{
  "Type": "t.String",
  "Unmodified": "std::string()",
  "OverrideRead": "GetGuiTemplateSourceId",
  "OverrideWrite": "SetGuiTemplateSourceId",
  "Replicated": true,
  "Editable": false,
  "ReadLevel": "Engine",
  "WriteLevel": "Engine"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### LuaSourceContainer.GuiTemplateSourceVersion

Engine-owned SourceVersion for the trusted StarterGui source bound to this clone.

```json
{
  "Type": "t.Integer",
  "Unmodified": "0",
  "OverrideRead": "GetGuiTemplateSourceVersion",
  "OverrideWrite": "SetGuiTemplateSourceVersion",
  "Replicated": true,
  "Editable": false,
  "ReadLevel": "Engine",
  "WriteLevel": "Engine"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### LuaSourceContainer.Source

Authoritative Luau source code

```json
{
  "Type": "t.String",
  "Unmodified": "std::string()",
  "OverrideRead": "GetSource",
  "OverrideWrite": "SetSource",
  "Serializable": true,
  "Editable": false,
  "ReadLevel": "Engine",
  "WriteLevel": "Engine"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### LuaSourceContainer.SourceId

Engine-owned persistent Script resource identity; not a live ObjectId or authority token

```json
{
  "Type": "t.String",
  "Unmodified": "std::string()",
  "OverrideRead": "GetSourceId",
  "OverrideWrite": "SetSourceId",
  "Serializable": true,
  "Editable": false,
  "ReadLevel": "Engine",
  "WriteLevel": "Engine"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### LuaSourceContainer.SourceVersion

Transient source invalidation token

```json
{
  "Type": "t.Integer",
  "Unmodified": "1",
  "OverrideRead": "GetSourceVersion",
  "OverrideWrite": "SetSourceVersion",
  "Replicated": true,
  "Editable": false,
  "WriteLevel": "Engine"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
