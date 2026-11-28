---
title: InteractionService
description: Public creator reference for InteractionService.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Owns bounded proximity discovery, activation validation, hold state, and presentation state.

Kind `Service`. Abstract `False`. Service policy `Default`.

Access through `game:GetService("InteractionService")` subject to the service policy and runtime context.

Inherited members from [Instance](./instance.md).

## Properties

### InteractionService.ActionText

Bounded presentation snapshot of the current prompt action text.

```json
{
  "Type": "t.String",
  "Unmodified": "'\"\"'",
  "OverrideRead": "GetActionText",
  "Writable": false
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### InteractionService.ActivePrompt

Current deterministic prompt candidate for the local Player.

```json
{
  "Type": "t.Optional(t.Instance(\"ProximityPrompt\"))",
  "Unmodified": "std::nullopt",
  "OverrideRead": "GetActivePrompt",
  "Writable": false
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### InteractionService.Available

Whether the local Player currently has a valid interaction candidate.

```json
{
  "Type": "t.Bool",
  "Unmodified": "false",
  "OverrideRead": "GetAvailable",
  "Writable": false
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### InteractionService.DefaultPresentationEnabled

Whether this runtime owns the shipped screen-space prompt presentation.

```json
{
  "Type": "t.Bool",
  "Unmodified": "false",
  "OverrideRead": "GetDefaultPresentationEnabled",
  "Writable": false
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### InteractionService.HoldDuration

Presentation snapshot of the current prompt hold duration.

```json
{
  "Type": "t.Float",
  "Unmodified": "0.0f",
  "OverrideRead": "GetHoldDuration",
  "Writable": false
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### InteractionService.HoldProgress

Current local hold progress from zero through one.

```json
{
  "Type": "t.Float",
  "Unmodified": "0.0f",
  "OverrideRead": "GetHoldProgress",
  "Writable": false,
  "Range": "{ minimum = 0, maximum = 1 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### InteractionService.InputHint

Short default-platform input hint for presentation only.

```json
{
  "Type": "t.String",
  "Unmodified": "'\"E / A\"'",
  "OverrideRead": "GetInputHint",
  "Writable": false
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### InteractionService.ObjectText

Bounded presentation snapshot of the current prompt object text.

```json
{
  "Type": "t.String",
  "Unmodified": "'\"\"'",
  "OverrideRead": "GetObjectText",
  "Writable": false
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

### InteractionService.BeginActivation

Begins the local presentation activation path; Engine still validates the active prompt.

```json
{}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### InteractionService.EndActivation

Ends the local presentation activation path and cancels an incomplete hold.

```json
{}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Signals

### InteractionService.PresentationChanged

Fired at a bounded semantic cadence when the local presentation snapshot changes.

```json
{}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
