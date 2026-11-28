---
title: Attachment
description: Public creator reference for Attachment.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Defines a point and orientation relative to an ancestor BasePart, optional canonical skeleton joint, or another Attachment.

Kind `Class`. Abstract `False`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [Instance](./instance.md).

## Properties

### Attachment.CFrame

Relative transform of this attachment.

```json
{
  "Type": "t.CFrame",
  "Unmodified": "gargantuan::CFrame()",
  "Serializable": true,
  "Replicated": true,
  "Category": "Transform"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Attachment.JointPath

Optional canonical skeleton joint path used by the semantic spatial runtime.

```json
{
  "Type": "t.String",
  "Unmodified": "'\"\"'",
  "OverrideRead": "GetJointPath",
  "OverrideWrite": "SetJointPath",
  "Serializable": true,
  "Replicated": true,
  "Category": "Transform"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Attachment.WorldCFrame

Current read-only world transform, including a resolved animated joint pose when bound.

```json
{
  "Type": "t.CFrame",
  "Unmodified": "gargantuan::CFrame()",
  "OverrideRead": "GetWorldCFrame",
  "Writable": false,
  "Editable": false,
  "Category": "Transform"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
