---
title: LayerCollector
description: Public creator reference for LayerCollector.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Abstract root for an ordered two-dimensional GUI layer

Kind `Class`. Abstract `True`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [GuiBase2d](./guibase2d.md).

Inherited members from [GuiBase](./guibase.md).

Inherited members from [Instance](./instance.md).

## Properties

### LayerCollector.Enabled

Whether this GUI layer participates in layout, rendering, and input

```json
{
  "Type": "t.Bool",
  "Unmodified": "true",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### LayerCollector.ResetOnSpawn

Whether player runtime policy may reset this layer on respawn

```json
{
  "Type": "t.Bool",
  "Unmodified": "false",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### LayerCollector.ZIndexBehavior

How descendant ZIndex values are interpreted

```json
{
  "Type": "t.EnumItem(\"ZIndexBehavior\")",
  "Unmodified": "Enums::ZIndexBehavior::Global",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
