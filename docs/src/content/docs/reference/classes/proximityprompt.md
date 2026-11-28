---
title: ProximityPrompt
description: Public creator reference for ProximityPrompt.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Declares a bounded world interaction anchored by an ancestor BasePart or Attachment.

Kind `Class`. Abstract `False`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [Instance](./instance.md).

## Properties

### ProximityPrompt.ActionText

Short verb presented for the interaction.

```json
{
  "Type": "t.String",
  "Unmodified": "'\"Interact\"'",
  "OverrideRead": "GetActionText",
  "OverrideWrite": "SetActionText",
  "Serializable": true,
  "Replicated": true,
  "Category": "Interaction"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ProximityPrompt.Enabled

Whether this prompt can become eligible for activation.

```json
{
  "Type": "t.Bool",
  "Unmodified": "true",
  "Serializable": true,
  "Replicated": true,
  "Category": "Interaction"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ProximityPrompt.HoldDuration

Monotonic hold duration in seconds; zero activates on the press edge.

```json
{
  "Type": "t.Float",
  "Unmodified": "0.0f",
  "Serializable": true,
  "Replicated": true,
  "Category": "Interaction",
  "Range": "{ minimum = 0, maximum = 30 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ProximityPrompt.MaxActivationDistance

Maximum world-space distance at which activation is valid.

```json
{
  "Type": "t.Float",
  "Unmodified": "10.0f",
  "Serializable": true,
  "Replicated": true,
  "Category": "Interaction",
  "Range": "{ minimum = 0.01, maximum = 64 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ProximityPrompt.ObjectText

Optional short label identifying the interaction target.

```json
{
  "Type": "t.String",
  "Unmodified": "'\"\"'",
  "OverrideRead": "GetObjectText",
  "OverrideWrite": "SetObjectText",
  "Serializable": true,
  "Replicated": true,
  "Category": "Interaction"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ProximityPrompt.RequiresLineOfSight

Whether rigid world geometry must leave the resolved prompt anchor visible.

```json
{
  "Type": "t.Bool",
  "Unmodified": "false",
  "Serializable": true,
  "Replicated": true,
  "Category": "Interaction"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

## Signals

### ProximityPrompt.Triggered

Fired exactly once after Engine validates a completed activation.

```json
{
  "Args": "{ t.Instance(\"Player\") }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
