---
title: ApplicationService lifecycle candidate
description: Candidate-only reference for ApplicationService.
---

Source `61f8520d782ea2810f168fa38d3b28027aff7a89`. Qualified unreleased lifecycle candidate.

Controls the enclosing standalone application.

[Lifecycle usage guide](../../guides/standalone-application.mdx)

## ApplicationService.GetState

Returns the state of the local presentation, when available.

```json
{
  "Name": "GetState",
  "AsLuau": true,
  "Description": "Returns the state of the local presentation, when available.",
  "LuauSignature": "(self: ApplicationService) -> (\"Starting\" | \"Running\" | \"Closing\")?"
}
```

## ApplicationService.Quit

Requests application shutdown after the current callback.

```json
{
  "Name": "Quit",
  "AsLuau": true,
  "Description": "Requests application shutdown after the current callback.",
  "LuauSignature": "(self: ApplicationService) -> { Ok: true, Value: boolean } | { Ok: false, Code: string, Message: string }"
}
```

## ApplicationService.Closing

Fires once when the local host commits to closing the application.

```json
{
  "Name": "Closing",
  "Description": "Fires once when the local host commits to closing the application.",
  "Args": "{t.Bool}"
}
```

