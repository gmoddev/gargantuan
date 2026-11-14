# Luau examples

These are standalone script bodies for Gargantuan's runtime `Script` objects. Author the Script and its Source through Studio, or place a recognized `.luau`, `.client.luau`, or `.server.luau` file in a trusted FileLink source directory for startup-time import. FileLink is not a general live sync or write API. Set `RunContext` as noted in each file. These examples are parser-checked, not self-starting tests. Each checks missing setup or recoverable API errors and uses `[System:SubSystem]` diagnostics.

| File | Setup |
| --- | --- |
| `part-and-tags.luau` | Server Script in a running DataModel. |
| `math-filter-signal.luau` | Server Script with `Workspace`; demonstrates VM-local `RaycastParams`, `Vector3`, and `Signal`. |
| `world-query.luau` | Server Script with `Workspace`; run after the example part or other collidable geometry exists. |
| `remote-server.luau` and `remote-client.luau` | A real client/server session, with a pre-authored `RemoteEvent` named `Ping` under `ReplicatedStorage`; place the scripts in Server and Client contexts respectively. Local Studio Play alone has no peer topology. |

The [API reference](../src/content/docs/api/index.mdx) explains authority, failure, and lifecycle limits. Copies of an `Instance` or `Remote` in a local variable remain valid only during the owning DataModel/session lifetime.
