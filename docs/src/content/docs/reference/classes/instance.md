---
title: Instance
description: Public creator reference for Instance.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Abstract class for all instances which can be part of the DataModel

Kind `Class`. Abstract `True`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

## Properties

### Instance.Archivable

Whether this instance can be archived through clones and saves

```json
{
  "Type": "t.Bool",
  "Unmodified": "false",
  "Category": "Data",
  "Editable": false
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Instance.ClassName

The class this instance belongs to

```json
{
  "Type": "t.String",
  "Unmodified": "CLASS_DEFINITION.ClassName",
  "OverrideRead": "GetClassName",
  "Category": "Identity"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Instance.Destroyed

Whether this instance has been destroyed

```json
{
  "Type": "t.Bool",
  "Unmodified": "false",
  "WriteLevel": "Never",
  "Editable": false,
  "Writable": false,
  "Category": "State"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Instance.Name

Non-unique identifier of the instance

```json
{
  "Type": "t.String",
  "Unmodified": "CLASS_DEFINITION.ClassName",
  "Replicated": true,
  "Category": "Identity"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Instance.Parent

Hierarchical parent of the instance, instances are unparented on destruction

```json
{
  "Type": "t.Optional(t.Instance())",
  "Unmodified": "std::nullopt",
  "OverrideRead": "GetParent",
  "OverrideWrite": "SetParent",
  "Category": "Hierarchy",
  "Editable": false
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

### Instance.ClearAllChildren

Destroys the direct children present at the start of the call, including their subtrees

```json
{
  "RequiredCapability": "MutateDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Instance.Clone

Returns an independent detached clone of this Instance and its descendants

```json
{
  "Returns": "t.Instance()"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Instance.Destroy

Destroys this instance

```json
{
  "RequiredCapability": "MutateDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Instance.FindFirstAncestor

Returns the first ancestor that matches the specified name

```json
{
  "Params": "{ p(\"name\", t.String) }",
  "Returns": "t.Instance()"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Instance.FindFirstAncestorOfClass

Returns the first ancestor that exactly matches the specified class

```json
{
  "Params": "{ p(\"className\", t.String) }",
  "Returns": "t.Instance()"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Instance.FindFirstAncestorWhichIsA

Returns the first ancestor that derives from the specified class

```json
{
  "Params": "{ p(\"className\", t.String) }",
  "Returns": "t.Instance()"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Instance.FindFirstChild

Returns the first direct child that matches the specified name

```json
{
  "Params": "{ p(\"name\", t.String), p(\"recursive\", t.Optional(t.Bool)) }",
  "Returns": "t.Instance()"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Instance.FindFirstChildOfClass

Returns the first direct child that exactly matches the specified class

```json
{
  "Params": "{ p(\"className\", t.String), p(\"recursive\", t.Optional(t.Bool)) }",
  "Returns": "t.Instance()"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Instance.FindFirstChildWhichIsA

Returns the first direct child that derives from the specified class

```json
{
  "Params": "{ p(\"className\", t.String), p(\"recursive\", t.Optional(t.Bool)) }",
  "Returns": "t.Instance()"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Instance.FindFirstDescendant

Returns the first descendant that matches the specified name

```json
{
  "Params": "{ p(\"name\", t.String) }",
  "Returns": "t.Instance()"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Instance.FindFirstDescendantOfClass

Returns the first descendant that exactly matches the specified class

```json
{
  "Params": "{ p(\"className\", t.String) }",
  "Returns": "t.Instance()"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Instance.FindFirstDescendantWhichIsA

Returns the first descendant that derives from the specified class

```json
{
  "Params": "{ p(\"className\", t.String) }",
  "Returns": "t.Instance()"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Instance.GetAncestors

Returns a snapshot of the current parent chain, nearest parent first

```json
{
  "Returns": "t.Array(t.Instance())"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Instance.GetAttribute

Returns a bounded runtime attribute or nil

```text
(self: Instance, name: string) -> AttributeValue?
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Instance.GetAttributeChangedSignal

Returns a signal fired once for each committed change to the named attribute

```text
(self: Instance, name: string) -> Signal<>
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Instance.GetAttributes

Returns a copy of all bounded runtime attributes

```text
(self: Instance) -> { [string]: AttributeValue }
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Instance.GetChildren

Returns all direct children of this instance

```json
{
  "Returns": "t.Array(t.Instance())"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Instance.GetDescendants

Returns all descendants below this instance

```json
{
  "Returns": "t.Array(t.Instance())"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Instance.GetExtensionProperty

Reads a schema-defined extension property by canonical extension and property name

```text
(self: Instance, extension: string, property: string) -> boolean | number | string
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Instance.GetFullName

Returns the instance's ancestry as a period-separated path

```json
{
  "Returns": "t.String"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Instance.GetPropertyChangedSignal

Returns a signal that is fired when the specified property is changed

```json
{
  "Params": "{ p(\"propertyName\", t.String) }",
  "Returns": "t.Signal()"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Instance.IsA

Whether this instance derives from the specified class

```json
{
  "Params": "{ p(\"className\", t.String) }",
  "Returns": "t.Bool"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Instance.IsAncestorOf

Tests whether descendant currently belongs below this instance. Self is excluded.

```json
{
  "Params": "{ p(\"descendant\", t.Instance()) }",
  "Returns": "t.Bool"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Instance.IsDescendantOf

Tests whether ancestor occurs on this instance's current parent chain. Self is excluded.

```json
{
  "Params": "{ p(\"ancestor\", t.Instance()) }",
  "Returns": "t.Bool"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Instance.IsPropertyModified

Whether the named property has been modified

```json
{
  "Params": "{ p(\"propertyName\", t.String) }",
  "Returns": "t.Bool"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Instance.ResetPropertyToDefault

Resets the named property to the base unmodified value

```json
{
  "Params": "{ p(\"propertyName\", t.String) }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Instance.SetAttribute

Sets or removes a bounded runtime attribute through authoritative mutation

```text
(self: Instance, name: string, value: AttributeValue?) -> ()
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Instance.SetExtensionProperty

Writes a schema-defined extension property through authoritative mutation

```text
(self: Instance, extension: string, property: string, value: boolean | number | string) -> ()
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Signals

### Instance.AncestryChanged

Fired after the Parent property of this or ancestor instances is changed

```json
{
  "Args": "{ t.Instance(), t.Instance() }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Instance.ChildAdded

Fired after an instance is parented to this instance

```json
{
  "Args": "{ t.Instance() }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Instance.ChildRemoved

Fired after an instance is unparented from this instance

```json
{
  "Args": "{ t.Instance() }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Instance.DescendantAdded

Fired after a descendant instance is added to this instance

```json
{
  "Args": "{ t.Instance() }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Instance.DescendantRemoved

Fired after a descendant instance is removed from this instance

```json
{
  "Args": "{ t.Instance() }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Instance.Destroying

Fired once when the instance is destroyed

```json
{
  "Args": "{}"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
