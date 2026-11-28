---
title: DataStoreService
description: Public creator reference for DataStoreService.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Submits bounded persistence work to the server's configured provider.

Kind `Service`. Abstract `False`. Service policy `ServerOnly`.

Access through `game:GetService("DataStoreService")` subject to the service policy and runtime context.

Inherited members from [Instance](./instance.md).

## Properties

## Methods

### DataStoreService.CreateArray

Creates a document array that retains its type when empty.

```text
(self: DataStoreService) -> { any }
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "PersistenceAccess"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### DataStoreService.GetDocumentStore

Gets a named collection for structured documents.

```text
(self: DataStoreService, Name: string) -> { GetAsync: (any, string, number?) -> any, PutAsync: (any, string, { [string]: any }, string?, number?) -> any, DeleteAsync: (any, string, string?, number?) -> any, QueryAsync: (any, { [string]: any }, number?) -> any, GetCapabilities: (any) -> { KeyValue: boolean, Documents: boolean, DocumentQueries: boolean } }
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "PersistenceAccess"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### DataStoreService.GetKeyValueStore

Gets a named store for native buffer values.

```text
(self: DataStoreService, Name: string) -> { GetAsync: (any, string, number?) -> any, PutAsync: (any, string, buffer, string?, number?) -> any, DeleteAsync: (any, string, string?, number?) -> any, GetCapabilities: (any) -> { KeyValue: boolean, Documents: boolean, DocumentQueries: boolean } }
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "PersistenceAccess"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### DataStoreService.GetNull

Returns the sentinel for a stored document null.

```text
(self: DataStoreService) -> unknown
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "PersistenceAccess"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
