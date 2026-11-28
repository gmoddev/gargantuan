---
title: BasePart
description: Public creator reference for BasePart.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Abstract class for workspace objects that physically interact.

Kind `Class`. Abstract `True`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [Instance](./instance.md).

## Properties

### BasePart.Anchored

Whether to not interact with physics

```json
{
  "Type": "t.Bool",
  "Unmodified": "false",
  "Serializable": true,
  "Replicated": true,
  "Category": "Behavior"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BasePart.AngularDamping

Nonnegative angular damping applied by the rigid solver.

```json
{
  "Type": "t.Float",
  "Unmodified": "0.0f",
  "Serializable": true,
  "Replicated": true,
  "Category": "Behavior",
  "Range": "{ minimum = 0, maximum = 100 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BasePart.CFrame

The world transform of this instance

```json
{
  "Type": "t.CFrame",
  "Unmodified": "gargantuan::CFrame()",
  "Serializable": true,
  "Replicated": true,
  "Category": "Transform",
  "EditorHint": "CFrameComponents"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BasePart.CanCollide

Whether other BaseParts can collide with this instance

```json
{
  "Type": "t.Bool",
  "Unmodified": "true",
  "Serializable": true,
  "Replicated": true,
  "Category": "Collision"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BasePart.CanTouch

Whether this instance can register touched events.

```json
{
  "Type": "t.Bool",
  "Unmodified": "true",
  "Serializable": true,
  "Replicated": true,
  "Category": "Collision"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BasePart.CastShadow

Whether to cast a shadow from this instance

```json
{
  "Type": "t.Bool",
  "Unmodified": "true",
  "Serializable": true,
  "Replicated": true,
  "Category": "Appearance"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BasePart.CollisionGroup

Collision group index from 0 through 15.

```json
{
  "Type": "t.Integer",
  "Unmodified": "0",
  "Serializable": true,
  "Replicated": true,
  "Category": "Collision",
  "Range": "{ minimum = 0, maximum = 15 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BasePart.CollisionMask

Bits for the 16 groups this part accepts; both parts must accept each other.

```json
{
  "Type": "t.Integer",
  "Unmodified": "65535",
  "Serializable": true,
  "Replicated": true,
  "Category": "Collision",
  "Range": "{ minimum = 0, maximum = 65535 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BasePart.Color

The visual color of this instance

```json
{
  "Type": "t.Color3",
  "Unmodified": "gargantuan::Color3(1.0f, 1.0f, 1.0f)",
  "Serializable": true,
  "Replicated": true,
  "Category": "Appearance",
  "EditorHint": "ColorComponents",
  "Range": "{ minimum = 0, maximum = 1 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BasePart.Friction

Rigid contact friction. The solver combines the two surfaces using their geometric mean.

```json
{
  "Type": "t.Float",
  "Unmodified": "0.6f",
  "Serializable": true,
  "Replicated": true,
  "Category": "Collision",
  "Range": "{ minimum = 0, maximum = 4 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BasePart.GravityScale

Per-body multiplier of WorldRoot gravity.

```json
{
  "Type": "t.Float",
  "Unmodified": "1.0f",
  "Serializable": true,
  "Replicated": true,
  "Category": "Behavior",
  "Range": "{ minimum = 0, maximum = 100 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BasePart.Kinematic

Moves from authored velocity without gravity or force response when Anchored is false.

```json
{
  "Type": "t.Bool",
  "Unmodified": "false",
  "Serializable": true,
  "Replicated": true,
  "Category": "Behavior"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BasePart.LinearDamping

Nonnegative linear damping applied by the rigid solver.

```json
{
  "Type": "t.Float",
  "Unmodified": "0.0f",
  "Serializable": true,
  "Replicated": true,
  "Category": "Behavior",
  "Range": "{ minimum = 0, maximum = 100 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BasePart.MovingBodyCCD

Requests an additional swept collision check against moving non-CCD rigid bodies while this part is dynamic and solid.

```json
{
  "Type": "t.Bool",
  "Unmodified": "false",
  "Serializable": true,
  "Replicated": true,
  "Category": "Collision"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BasePart.Position

The world position of this instance

```json
{
  "Type": "t.Vector3",
  "Unmodified": "glm::vec3(0.0f, 0.0f, 0.0f)",
  "OverrideRead": "GetPosition",
  "OverrideWrite": "SetPosition",
  "Category": "Transform",
  "Editable": false
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BasePart.Restitution

Rigid contact bounciness. The greater of the two surfaces' values applies to qualifying impacts.

```json
{
  "Type": "t.Float",
  "Unmodified": "0.0f",
  "Serializable": true,
  "Replicated": true,
  "Category": "Collision",
  "Range": "{ minimum = 0, maximum = 1 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BasePart.Rotation

The world rotation of this instance

```json
{
  "Type": "t.Vector3",
  "Unmodified": "glm::vec3(0.0f, 0.0f, 0.0f)",
  "OverrideRead": "GetRotation",
  "OverrideWrite": "SetRotation",
  "Category": "Transform",
  "Editable": false
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BasePart.Size

The world size of this instance

```json
{
  "Type": "t.Vector3",
  "Unmodified": "glm::vec3(1.0f, 1.0f, 1.0f)",
  "Serializable": true,
  "Replicated": true,
  "Category": "Transform"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BasePart.SleepEnabled

Whether the rigid solver may sleep this body.

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

### BasePart.SpatialRegion

Explicit independent spatial domain for this part and its spatial descendants. An unset value uses the enclosing world.

```json
{
  "Type": "t.Optional(t.Instance(\"SpatialRegion\", true))",
  "Unmodified": "std::nullopt",
  "OverrideRead": "GetSpatialRegion",
  "OverrideWrite": "SetSpatialRegion",
  "MaterializationDependency": "Hard",
  "Serializable": true,
  "Replicated": true,
  "Category": "World"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BasePart.Transparency

The visual transparency of this instance

```json
{
  "Type": "t.Float",
  "Unmodified": "0.0f",
  "Serializable": true,
  "Replicated": true,
  "Category": "Appearance",
  "Range": "{ minimum = 0, maximum = 1 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

### BasePart.ApplyAngularImpulse

Queues an angular impulse for the next fixed step.

```json
{
  "Params": "{ p(\"Impulse\", t.Vector3) }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BasePart.ApplyForce

Queues a force applied during the next fixed step.

```json
{
  "Params": "{ p(\"Force\", t.Vector3) }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BasePart.ApplyImpulse

Applies an impulse onto this instance.

```json
{
  "Params": "{ p(\"impulse\", t.Vector3) }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BasePart.ApplyImpulseAtPosition

Queues an impulse at a world-space point for the next fixed step.

```json
{
  "Params": "{ p(\"Impulse\", t.Vector3), p(\"Position\", t.Vector3) }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BasePart.ApplyTorque

Queues a torque applied during the next fixed step.

```json
{
  "Params": "{ p(\"Torque\", t.Vector3) }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BasePart.GetAngularVelocity

Returns the most recently published angular velocity in radians per second.

```json
{
  "Returns": "t.Vector3"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BasePart.GetAwake

Returns the most recently published awake state.

```json
{
  "Returns": "t.Bool"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BasePart.GetLinearVelocity

Returns the most recently published linear velocity.

```json
{
  "Returns": "t.Vector3"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BasePart.GetLocalInertia

Returns the diagonal of the local rotational inertia tensor.

```json
{
  "Returns": "t.Vector3"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BasePart.GetMass

Returns the solver mass for the current body, or zero before admission.

```json
{
  "Returns": "t.Float"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BasePart.SetAngularVelocity

Sets the world-space angular velocity in radians per second at the next physics safe point.

```json
{
  "Params": "{ p(\"Velocity\", t.Vector3) }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BasePart.SetAwake

Sets awake state at the next physics safe point.

```json
{
  "Params": "{ p(\"Awake\", t.Bool) }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BasePart.SetLinearVelocity

Sets the world-space linear velocity at the next physics safe point.

```json
{
  "Params": "{ p(\"Velocity\", t.Vector3) }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Signals

### BasePart.TouchEnded

Fired when another BasePart has stopped touching this instance.

```json
{
  "Args": "{ t.Instance(\"BasePart\") }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BasePart.Touched

Fired when another BasePart touches this instance.

```json
{
  "Args": "{ t.Instance(\"BasePart\") }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
