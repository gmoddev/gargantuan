---
title: CameraType
description: Public creator reference for CameraType.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Values follow the native G_ENUM C++ enum registration, including explicit assignments and implicit integer increments. Project-defined PreRun extensions have their own schema lifetime.

## Enum.CameraType./// Camera is stationary.
		Fixed

Numeric value `0`.

## Enum.CameraType./// Camera moves with the subject at a fixed offset and will rotate as the subject rotates.
		Attach

Numeric value `1`.

## Enum.CameraType./// Camera is stationary but will rotate to keep the subject in the center of the screen.
		Watch

Numeric value `2`.

## Enum.CameraType./// Camera moves with the subject but does not rotate automatically.
		Track

Numeric value `3`.

## Enum.CameraType./// Camera moves with the subject and rotates to keep the subject in the center.
		Follow

Numeric value `4`.

## Enum.CameraType./// Default mode used by Gargantuan.
		Custom

Numeric value `5`.

## Enum.CameraType./// No default behavior. Used when developers need to script custom behavior.
		Scriptable

Numeric value `6`.

## Enum.CameraType./// The camera has a fixed Y position

Numeric value `7`.

## Enum.CameraType.but can be rotated around the player.
		Orbital

Numeric value `8`.

## Enum.CameraType./// Camera has omnidirectional movement.
		Freecam

Numeric value `9`.

## Enum.CameraType.

Numeric value `10`.

