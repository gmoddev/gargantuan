---
title: RunService lifecycle candidate
description: Candidate-only reference for RunService.
---

Source `61f8520d782ea2810f168fa38d3b28027aff7a89`. Qualified unreleased lifecycle candidate.

Provides access to the runtime.

[Lifecycle usage guide](../../guides/standalone-application.mdx)

## RunService.IsGameplayPaused

Returns the committed gameplay pause state.

```json
{
  "Name": "IsGameplayPaused",
  "AsLuau": true,
  "Description": "Returns the committed gameplay pause state.",
  "LuauSignature": "(self: RunService) -> boolean"
}
```

## RunService.PauseGameplay

Requests a pause at the next simulation boundary.

```json
{
  "Name": "PauseGameplay",
  "AsLuau": true,
  "Description": "Requests a pause at the next simulation boundary.",
  "LuauSignature": "(self: RunService) -> { Await: (self: any) -> ({ Ok: true, Value: boolean } | { Ok: false, Code: string, Message: string }), GetResult: (self: any) -> ({ Ok: true, Value: boolean } | { Ok: false, Code: string, Message: string })?, GetState: (self: any) -> string, GetProgress: (self: any) -> {Phase: string}?, ProgressChanged: Signal, Cancel: (self: any) -> boolean, Completed: Signal }"
}
```

## RunService.ResumeGameplay

Releases the caller's gameplay pause.

```json
{
  "Name": "ResumeGameplay",
  "AsLuau": true,
  "Description": "Releases the caller's gameplay pause.",
  "LuauSignature": "(self: RunService) -> { Await: (self: any) -> ({ Ok: true, Value: boolean } | { Ok: false, Code: string, Message: string }), GetResult: (self: any) -> ({ Ok: true, Value: boolean } | { Ok: false, Code: string, Message: string })?, GetState: (self: any) -> string, GetProgress: (self: any) -> {Phase: string}?, ProgressChanged: Signal, Cancel: (self: any) -> boolean, Completed: Signal }"
}
```

## RunService.GameplayPauseChanged

Fired in presentation when the effective gameplay hold changes.

```json
{
  "Name": "GameplayPauseChanged",
  "Description": "Fired in presentation when the effective gameplay hold changes.",
  "Args": "{ t.Bool }"
}
```

## RunService.PostSimulation

Fired after simulating WorldRoots. Listeners receive the delta time

```json
{
  "Name": "PostSimulation",
  "Description": "Fired after simulating WorldRoots. Listeners receive the delta time",
  "Args": "{ t.Float }"
}
```

## RunService.PreRender

Fired before rendering the scene. Listeners receive the delta time.

```json
{
  "Name": "PreRender",
  "Description": "Fired before rendering the scene. Listeners receive the delta time.",
  "Args": "{ t.Float }"
}
```

## RunService.PreSimulation

Fired before simulating WorldRoots. Listeners receive the delta time

```json
{
  "Name": "PreSimulation",
  "Description": "Fired before simulating WorldRoots. Listeners receive the delta time",
  "Args": "{ t.Float }"
}
```

