---
title: SessionService lifecycle candidate
description: Candidate-only reference for SessionService.
---

Source `61f8520d782ea2810f168fa38d3b28027aff7a89`. Qualified unreleased lifecycle candidate.

Controls the gameplay session owned by a standalone application.

[Lifecycle usage guide](../../guides/standalone-application.mdx)

## SessionService.FindServers

Lists current sessions supplied by the native launcher for the selected source.

```json
{
  "Name": "FindServers",
  "AsLuau": true,
  "Description": "Lists current sessions supplied by the native launcher for the selected source.",
  "LuauSignature": "(self: SessionService, Source: \"LAN\" | \"Node\", Cursor: string?) -> { Await: (self: any) -> ({ Ok: true, Value: {Items: {{Id: string, Name: string}}, NextCursor: string?} } | { Ok: false, Code: string, Message: string }), GetResult: (self: any) -> ({ Ok: true, Value: {Items: {{Id: string, Name: string}}, NextCursor: string?} } | { Ok: false, Code: string, Message: string })?, GetState: (self: any) -> string, GetProgress: (self: any) -> {Phase: string}?, ProgressChanged: Signal, Cancel: (self: any) -> boolean, Completed: Signal }"
}
```

## SessionService.GetSession

Returns a session snapshot.

```json
{
  "Name": "GetSession",
  "AsLuau": true,
  "Description": "Returns a session snapshot.",
  "LuauSignature": "(self: SessionService) -> { Generation: string, State: \"Idle\" | \"Starting\" | \"Connecting\" | \"Active\" | \"Leaving\", World: \"Absent\" | \"Preparing\" | \"Ready\" | \"Retiring\", Mode: (\"Offline\" | \"PrivateHost\" | \"MultiplayerHost\" | \"Remote\")?, SaveId: string?, WorldId: string?, LastError: {Ok: false, Code: string, Message: string, RecoveryId: string?}? }"
}
```

## SessionService.Host

Opens an enrolled multiplayer Server and joins it as a player.

```json
{
  "Name": "Host",
  "AsLuau": true,
  "Description": "Opens an enrolled multiplayer Server and joins it as a player.",
  "LuauSignature": "(self: SessionService, Options: {SaveId: string?, MaxPlayers: number?, Discovery: (\"LAN\" | \"Node\")?}?) -> { Await: (self: any) -> ({ Ok: true, Value: { Generation: string, State: \"Idle\" | \"Starting\" | \"Connecting\" | \"Active\" | \"Leaving\", World: \"Absent\" | \"Preparing\" | \"Ready\" | \"Retiring\", Mode: (\"Offline\" | \"PrivateHost\" | \"MultiplayerHost\" | \"Remote\")?, SaveId: string?, WorldId: string?, LastError: {Ok: false, Code: string, Message: string, RecoveryId: string?}? } } | { Ok: false, Code: string, Message: string }), GetResult: (self: any) -> ({ Ok: true, Value: { Generation: string, State: \"Idle\" | \"Starting\" | \"Connecting\" | \"Active\" | \"Leaving\", World: \"Absent\" | \"Preparing\" | \"Ready\" | \"Retiring\", Mode: (\"Offline\" | \"PrivateHost\" | \"MultiplayerHost\" | \"Remote\")?, SaveId: string?, WorldId: string?, LastError: {Ok: false, Code: string, Message: string, RecoveryId: string?}? } } | { Ok: false, Code: string, Message: string })?, GetState: (self: any) -> string, GetProgress: (self: any) -> {Phase: string}?, ProgressChanged: Signal, Cancel: (self: any) -> boolean, Completed: Signal }"
}
```

## SessionService.Join

Connects to an admitted direct target.

```json
{
  "Name": "Join",
  "AsLuau": true,
  "Description": "Connects to an admitted direct target.",
  "LuauSignature": "(self: SessionService, Target: ({Kind: \"Direct\", Address: string} | {Kind: \"Listing\", Id: string})) -> { Await: (self: any) -> ({ Ok: true, Value: { Generation: string, State: \"Idle\" | \"Starting\" | \"Connecting\" | \"Active\" | \"Leaving\", World: \"Absent\" | \"Preparing\" | \"Ready\" | \"Retiring\", Mode: (\"Offline\" | \"PrivateHost\" | \"MultiplayerHost\" | \"Remote\")?, SaveId: string?, WorldId: string?, LastError: {Ok: false, Code: string, Message: string, RecoveryId: string?}? } } | { Ok: false, Code: string, Message: string }), GetResult: (self: any) -> ({ Ok: true, Value: { Generation: string, State: \"Idle\" | \"Starting\" | \"Connecting\" | \"Active\" | \"Leaving\", World: \"Absent\" | \"Preparing\" | \"Ready\" | \"Retiring\", Mode: (\"Offline\" | \"PrivateHost\" | \"MultiplayerHost\" | \"Remote\")?, SaveId: string?, WorldId: string?, LastError: {Ok: false, Code: string, Message: string, RecoveryId: string?}? } } | { Ok: false, Code: string, Message: string })?, GetState: (self: any) -> string, GetProgress: (self: any) -> {Phase: string}?, ProgressChanged: Signal, Cancel: (self: any) -> boolean, Completed: Signal }"
}
```

## SessionService.Leave

Retires the current gameplay session.

```json
{
  "Name": "Leave",
  "AsLuau": true,
  "Description": "Retires the current gameplay session.",
  "LuauSignature": "(self: SessionService) -> { Await: (self: any) -> ({ Ok: true, Value: { Generation: string, State: \"Idle\" | \"Starting\" | \"Connecting\" | \"Active\" | \"Leaving\", World: \"Absent\" | \"Preparing\" | \"Ready\" | \"Retiring\", Mode: (\"Offline\" | \"PrivateHost\" | \"MultiplayerHost\" | \"Remote\")?, SaveId: string?, WorldId: string?, LastError: {Ok: false, Code: string, Message: string, RecoveryId: string?}? } } | { Ok: false, Code: string, Message: string }), GetResult: (self: any) -> ({ Ok: true, Value: { Generation: string, State: \"Idle\" | \"Starting\" | \"Connecting\" | \"Active\" | \"Leaving\", World: \"Absent\" | \"Preparing\" | \"Ready\" | \"Retiring\", Mode: (\"Offline\" | \"PrivateHost\" | \"MultiplayerHost\" | \"Remote\")?, SaveId: string?, WorldId: string?, LastError: {Ok: false, Code: string, Message: string, RecoveryId: string?}? } } | { Ok: false, Code: string, Message: string })?, GetState: (self: any) -> string, GetProgress: (self: any) -> {Phase: string}?, ProgressChanged: Signal, Cancel: (self: any) -> boolean, Completed: Signal }"
}
```

## SessionService.Reconnect

Starts a fresh connection to the last remote session after leaving or disconnecting.

```json
{
  "Name": "Reconnect",
  "AsLuau": true,
  "Description": "Starts a fresh connection to the last remote session after leaving or disconnecting.",
  "LuauSignature": "(self: SessionService) -> { Await: (self: any) -> ({ Ok: true, Value: { Generation: string, State: \"Idle\" | \"Starting\" | \"Connecting\" | \"Active\" | \"Leaving\", World: \"Absent\" | \"Preparing\" | \"Ready\" | \"Retiring\", Mode: (\"Offline\" | \"PrivateHost\" | \"MultiplayerHost\" | \"Remote\")?, SaveId: string?, WorldId: string?, LastError: {Ok: false, Code: string, Message: string, RecoveryId: string?}? } } | { Ok: false, Code: string, Message: string }), GetResult: (self: any) -> ({ Ok: true, Value: { Generation: string, State: \"Idle\" | \"Starting\" | \"Connecting\" | \"Active\" | \"Leaving\", World: \"Absent\" | \"Preparing\" | \"Ready\" | \"Retiring\", Mode: (\"Offline\" | \"PrivateHost\" | \"MultiplayerHost\" | \"Remote\")?, SaveId: string?, WorldId: string?, LastError: {Ok: false, Code: string, Message: string, RecoveryId: string?}? } } | { Ok: false, Code: string, Message: string })?, GetState: (self: any) -> string, GetProgress: (self: any) -> {Phase: string}?, ProgressChanged: Signal, Cancel: (self: any) -> boolean, Completed: Signal }"
}
```

## SessionService.StartLocal

Starts an offline world or its provisioned private Server.

```json
{
  "Name": "StartLocal",
  "AsLuau": true,
  "Description": "Starts an offline world or its provisioned private Server.",
  "LuauSignature": "(self: SessionService, Options: { SaveId: string?, PrivateServer: boolean? }?) -> { Await: (self: any) -> ({ Ok: true, Value: { Generation: string, State: \"Idle\" | \"Starting\" | \"Connecting\" | \"Active\" | \"Leaving\", World: \"Absent\" | \"Preparing\" | \"Ready\" | \"Retiring\", Mode: (\"Offline\" | \"PrivateHost\" | \"MultiplayerHost\" | \"Remote\")?, SaveId: string?, WorldId: string?, LastError: {Ok: false, Code: string, Message: string, RecoveryId: string?}? } } | { Ok: false, Code: string, Message: string }), GetResult: (self: any) -> ({ Ok: true, Value: { Generation: string, State: \"Idle\" | \"Starting\" | \"Connecting\" | \"Active\" | \"Leaving\", World: \"Absent\" | \"Preparing\" | \"Ready\" | \"Retiring\", Mode: (\"Offline\" | \"PrivateHost\" | \"MultiplayerHost\" | \"Remote\")?, SaveId: string?, WorldId: string?, LastError: {Ok: false, Code: string, Message: string, RecoveryId: string?}? } } | { Ok: false, Code: string, Message: string })?, GetState: (self: any) -> string, GetProgress: (self: any) -> {Phase: string}?, ProgressChanged: Signal, Cancel: (self: any) -> boolean, Completed: Signal }"
}
```

## SessionService.Changed

Fires with an immutable snapshot at each committed session transition.

```json
{
  "Name": "Changed",
  "Description": "Fires with an immutable snapshot at each committed session transition.",
  "Args": "{{kind = \"c\", cType = \"gargantuan::SessionSnapshot\", luauType = Info, cIncludes = {'\"gargantuan/runtime/SessionSnapshot.hpp\"'}}}"
}
```

