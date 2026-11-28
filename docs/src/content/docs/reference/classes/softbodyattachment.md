---
title: SoftBodyAttachment
description: Public creator reference for SoftBodyAttachment.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Pins one stable deformable vertex to a world-space position when parented beneath a DeformableBody.

Kind `Class`. Abstract `False`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [Instance](./instance.md).

## Properties

### SoftBodyAttachment.Enabled

Whether this pin participates in simulation.

```json
{
  "Type": "t.Bool",
  "Unmodified": "true",
  "Serializable": true,
  "Replicated": true,
  "Category": "Behavior"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### SoftBodyAttachment.Position

World-space target position for the pinned vertex.

```json
{
  "Type": "t.Vector3",
  "Unmodified": "glm::vec3(0.0f, 0.0f, 0.0f)",
  "Serializable": true,
  "Replicated": true,
  "Category": "Attachment"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### SoftBodyAttachment.VertexIndex

Zero-based stable topology vertex index pinned by this attachment.

```json
{
  "Type": "t.Integer",
  "Unmodified": "0",
  "Serializable": true,
  "Replicated": true,
  "Category": "Attachment",
  "Range": "{ minimum = 0, maximum = 65535 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
