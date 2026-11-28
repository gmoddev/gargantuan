---
title: UserInputService
description: Public creator reference for UserInputService.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Provides user input events, including keyboard, mouse, touch, and gamepads

Kind `Service`. Abstract `False`. Service policy `Default`.

Access through `game:GetService("UserInputService")` subject to the service policy and runtime context.

Inherited members from [Instance](./instance.md).

## Properties

### UserInputService.AccelerometerEnabled

Whether device accelerometer reading is active

```json
{
  "Type": "t.Bool",
  "Unmodified": "false"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.GamepadEnabled

Whether gamepad input processing is active

```json
{
  "Type": "t.Bool",
  "Unmodified": "false"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.GyroscopeEnabled

Whether device gyroscope reading is active

```json
{
  "Type": "t.Bool",
  "Unmodified": "false"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.KeyboardEnabled

Whether a physical keyboard is available

```json
{
  "Type": "t.Bool",
  "Unmodified": "true"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.MouseBehavior

Behavior mode of the mouse cursor

```json
{
  "Type": "t.EnumItem(\"MouseBehavior\")",
  "Unmodified": "Enums::MouseBehavior::Default"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.MouseIcon

Content identifier for the mouse icon asset

```json
{
  "Type": "t.StringView",
  "Unmodified": "'\"\"'"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.MouseIconContent

Raw content for the mouse icon

```json
{
  "Type": "t.StringView",
  "Unmodified": "'\"\"'"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.MouseIconEnabled

Whether the mouse icon is visible

```json
{
  "Type": "t.Bool",
  "Unmodified": "false"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.OnScreenKeyboardPosition

Screen position of the on-screen keyboard

```json
{
  "Type": "t.Vector2",
  "Unmodified": "gargantuan::Vector2(0.0f, 0.0f)"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.OnScreenKeyboardSize

Dimensions of the on-screen keyboard

```json
{
  "Type": "t.Vector2",
  "Unmodified": "gargantuan::Vector2(0.0f, 0.0f)"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.OnScreenKeyboardVisible

Whether the touch on-screen keyboard is visible

```json
{
  "Type": "t.Bool",
  "Unmodified": "false"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.TouchEnabled

Whether touch input events are enabled

```json
{
  "Type": "t.Bool",
  "Unmodified": "false"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.TouchScreenEnabled

Whether a touchscreen interface is detected

```json
{
  "Type": "t.Bool",
  "Unmodified": "false"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

### UserInputService.GetKeysPressed

Returns an array of input objects for all currently pressed keyboard keys

```json
{
  "Returns": "t.Array(t.Instance(\"InputObject\"))"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.GetLastInputType

Returns the type of the last input event received

```json
{
  "Returns": "t.EnumItem(\"UserInputType\")"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.GetMouseButtonsPressed

Returns an array of input objects for all currently pressed mouse buttons

```json
{
  "Returns": "t.Array(t.Instance(\"InputObject\"))"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.GetMouseDelta

Returns the mouse movement delta vector since the last frame

```json
{
  "Returns": "t.Vector2"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.GetMouseLocation

Returns the current position of the mouse cursor

```json
{
  "Returns": "t.Vector2"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.IsKeyDown

Returns whether a keyboard key is currently pressed

```json
{
  "Params": "{ p(\"keyCode\", t.EnumItem(\"KeyCode\")) }",
  "Returns": "t.Bool"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.IsMouseButtonPressed

Returns whether a specific mouse button is currently pressed

```json
{
  "Params": "{ p(\"mouseType\", t.EnumItem(\"UserInputType\")) }",
  "Returns": "t.Bool"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Signals

### UserInputService.DeviceAccelerationChanged

Fired when the physical acceleration of the device changes

```json
{
  "Args": "{ t.Instance(\"InputObject\") }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.DeviceGravityChanged

Fired when the gravity vector of the device changes

```json
{
  "Args": "{ t.Instance(\"InputObject\") }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.DeviceRotationChanged

Fired when the device orientation changes

```json
{
  "Args": "{ t.Instance(\"InputObject\"), t.CFrame }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.GamepadConnected

Fired when a gamepad is connected

```json
{
  "Args": "{ t.Instance(\"InputObject\") }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.GamepadDisconnected

Fired when a gamepad is disconnected

```json
{
  "Args": "{ t.Instance(\"InputObject\") }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.InputBegan

Fired when a user input starts

```json
{
  "Args": "{ t.Instance(\"InputObject\"), t.Bool }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.InputChanged

Fired when an active user input changes state

```json
{
  "Args": "{ t.Instance(\"InputObject\"), t.Bool }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.InputEnded

Fired when a user input ends

```json
{
  "Args": "{ t.Instance(\"InputObject\"), t.Bool }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.JumpRequest

Fired when a jump action is triggered by the user

```json
{}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.LastInputTypeChanged

Fired when the most recent input device type changes

```json
{
  "Args": "{ t.EnumItem(\"UserInputType\") }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.PointerAction

Fired for general pointer/mouse actions

```json
{
  "Args": "{ t.Float, t.Vector2, t.Float, t.Bool }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.TouchDrag

Fired during a touch dragging gesture

```json
{
  "Args": "{ t.EnumItem(\"SwipeDirection\"), t.Integer, t.Bool }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.TouchEnded

Fired when a touch gesture ends

```json
{
  "Args": "{ t.Instance(\"InputObject\"), t.Bool }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.TouchLongPress

Fired when a touch long press gesture is registered

```json
{
  "Args": "{ t.Array(t.Vector2), t.EnumItem(\"UserInputState\"), t.Bool }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.TouchMoved

Fired when an active touch point moves

```json
{
  "Args": "{ t.Instance(\"InputObject\"), t.Bool }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.TouchPan

Fired during a touch panning gesture

```json
{
  "Args": "{ t.Array(t.Vector2), t.Vector2, t.Vector2, t.EnumItem(\"UserInputState\"), t.Bool }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.TouchPinch

Fired during a touch pinch gesture

```json
{
  "Args": "{ t.Array(t.Vector2), t.Float, t.Float, t.EnumItem(\"UserInputState\"), t.Bool }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.TouchRotate

Fired during a touch rotation gesture

```json
{
  "Args": "{ t.Array(t.Vector2), t.Float, t.Float, t.EnumItem(\"UserInputState\"), t.Bool }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.TouchStarted

Fired when a touch gesture begins

```json
{
  "Args": "{ t.Instance(\"InputObject\"), t.Bool }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.TouchTap

Fired when a touch tap occurs

```json
{
  "Args": "{ t.Array(t.Vector2), t.Bool }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.TouchTapInWorld

Fired when a tap occurs in 3D world space

```json
{
  "Args": "{ t.Vector2, t.Bool }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.WindowFocusReleased

Fired when the application window loses focus

```json
{}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### UserInputService.WindowFocused

Fired when the application window gains focus

```json
{}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
