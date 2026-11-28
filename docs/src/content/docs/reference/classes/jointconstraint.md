---
title: JointConstraint
description: Public creator reference for JointConstraint.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Connects two attachment frames as a ball socket, hinge, slider, or distance joint.

Kind `Class`. Abstract `False`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [Constraint](./constraint.md).

Inherited members from [Instance](./instance.md).

## Properties

### JointConstraint.CollideConnected

Whether attached parts may collide.

```json
{
  "Type": "t.Bool",
  "Unmodified": "false",
  "Serializable": true,
  "Replicated": true,
  "Category": "Constraint"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### JointConstraint.EnableLimit

Enable hinge angle, slider translation, or distance limits.

```json
{
  "Type": "t.Bool",
  "Unmodified": "false",
  "Serializable": true,
  "Replicated": true,
  "Category": "Constraint"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### JointConstraint.EnableMotor

Enable the hinge or slider motor.

```json
{
  "Type": "t.Bool",
  "Unmodified": "false",
  "Serializable": true,
  "Replicated": true,
  "Category": "Constraint"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### JointConstraint.JointType

BallSocket, Hinge, Slider, or Distance.

```json
{
  "Type": "t.String",
  "Unmodified": "'\"BallSocket\"'",
  "OverrideRead": "GetJointType",
  "OverrideWrite": "SetJointType",
  "Serializable": true,
  "Replicated": true,
  "Category": "Constraint"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### JointConstraint.LowerLimit

Lower limit in radians for a hinge, or world units for a slider or distance joint.

```json
{
  "Type": "t.Float",
  "Unmodified": "0.0f",
  "Serializable": true,
  "Replicated": true,
  "Category": "Constraint"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### JointConstraint.MaximumMotorForce

Maximum hinge torque or slider force.

```json
{
  "Type": "t.Float",
  "Unmodified": "0.0f",
  "Serializable": true,
  "Replicated": true,
  "Category": "Constraint",
  "Range": "{ minimum = 0 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### JointConstraint.MotorSpeed

Hinge motor radians per second or slider motor world units per second.

```json
{
  "Type": "t.Float",
  "Unmodified": "0.0f",
  "Serializable": true,
  "Replicated": true,
  "Category": "Constraint"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### JointConstraint.RestLength

Unstretched distance between attachment points.

```json
{
  "Type": "t.Float",
  "Unmodified": "1.0f",
  "Serializable": true,
  "Replicated": true,
  "Category": "Constraint",
  "Range": "{ minimum = 0.001 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### JointConstraint.SpringDamping

Distance spring damping ratio.

```json
{
  "Type": "t.Float",
  "Unmodified": "0.7f",
  "Serializable": true,
  "Replicated": true,
  "Category": "Constraint",
  "Range": "{ minimum = 0, maximum = 2 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### JointConstraint.SpringFrequency

Distance spring frequency in hertz; zero means a rigid distance joint.

```json
{
  "Type": "t.Float",
  "Unmodified": "0.0f",
  "Serializable": true,
  "Replicated": true,
  "Category": "Constraint",
  "Range": "{ minimum = 0, maximum = 15 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### JointConstraint.UpperLimit

Upper limit in radians for a hinge, or world units for a slider or distance joint.

```json
{
  "Type": "t.Float",
  "Unmodified": "0.0f",
  "Serializable": true,
  "Replicated": true,
  "Category": "Constraint"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
