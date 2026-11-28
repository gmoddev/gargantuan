---
title: Tags
description: Public creator reference for Tags.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Provides bounded, DataModel-scoped Instance tag membership and indexed queries.

Kind `Service`. Abstract `False`. Service policy `Default`.

Access through `game:GetService("Tags")` subject to the service policy and runtime context.

Inherited members from [Instance](./instance.md).

## Properties

## Methods

### Tags.Add

Adds a tag through authoritative mutation.

```text
(self: Tags, instance: Instance, tag: string) -> ()
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Tags.GetTagged

Returns live tagged Instances in ObjectId order.

```text
(self: Tags, tag: string) -> { Instance }
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Tags.GetTaggedAll

Returns the intersection of explicit tags in ObjectId order.

```text
(self: Tags, tags: { string }) -> { Instance }
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Tags.GetTags

Returns an Instance's tags in deterministic name order.

```text
(self: Tags, instance: Instance) -> { string }
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Tags.Has

Returns whether an Instance has a tag.

```text
(self: Tags, instance: Instance, tag: string) -> boolean
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Tags.Remove

Removes a tag through authoritative mutation.

```text
(self: Tags, instance: Instance, tag: string) -> ()
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
