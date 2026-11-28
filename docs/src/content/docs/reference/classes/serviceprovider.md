---
title: ServiceProvider
description: Public creator reference for ServiceProvider.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Abstract class that owns registered singleton services. Canonical direct member access and GetService lazily resolve the same service; FindService never constructs.

Kind `Class`. Abstract `True`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [Instance](./instance.md).

## Properties

## Methods

### ServiceProvider.FindService

Returns the named service if it exists, or otherwise nil.

```json
{
  "Params": "{ p(\"serviceName\", t.String) }",
  "Returns": "t.Optional(t.Instance())"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ServiceProvider.GetService

Returns the named service, constructing if it doesn't exist.

```json
{
  "Params": "{ p(\"serviceName\", t.String) }",
  "Returns": "t.Instance()"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Signals

### ServiceProvider.Close

Fired when this service provider is closed.

```json
{}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ServiceProvider.ServiceAdded

Fired when a service is added to this instance

```json
{
  "Args": "{ t.Instance() }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ServiceProvider.ServiceRemoving

Fired when a service is about to be removed from to this instance

```json
{
  "Args": "{ t.Instance() }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
