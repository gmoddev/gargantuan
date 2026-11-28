---
title: ProcessService
description: Public creator reference for ProcessService.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Provides access to the system process.

Kind `Service`. Abstract `False`. Service policy `Default`.

Access through `game:GetService("ProcessService")` subject to the service policy and runtime context.

Inherited members from [Instance](./instance.md).

## Properties

## Methods

### ProcessService.ExitAsync

Exits the system process with the specified exit code. Yields indefinitely.

```text
(self: ProcessService, exitCode: number) -> never
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ProcessService.FlushStdout

Flushes the standard output.

```json
{}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ProcessService.WriteToStdout

Writes to the standard output without a new line.

```text
(self: ProcessService, ...string) -> ()
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
