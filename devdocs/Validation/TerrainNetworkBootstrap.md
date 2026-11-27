# Terrain Foundation 2 network qualification

The Windows Release client and dedicated server were built from the same
source with GNS `2cb93a06350bb065db53abdb0d87cf297e0bfd34`. The normal
packager produced matching Development packages, and the two packaged
processes connected over loopback. The server's Luau fixture hit its
authoritative terrain. The client reported installed native collision
before acknowledging readiness. The server subsequently admitted gameplay.

The real-transport regression uses a 74,190-byte closure and a receiver
initialized with package pins and zero source bytes. An incompatible peer
is rejected before Player creation. The regression delays actual terrain
frames, checks the accepted client still has no gameplay world, and then
releases the transfer. Decoded scalar state alone leaves the server at
zero ready peers. After native terrain ray hits on both sides, the client
attaches and sends the epoch-bound acknowledgment. An ordinary keyboard
event moves the authoritative Character over the terrain. A reconnect
repeats delivery under a new epoch. The codec fixture also checks failure
on same-length corrupt content and trailing fragment bytes.

Run `ctest -R gargantuan_terrain_session_gns` in a GNS-enabled Release
build. The scope, byte limits, and distribution policy are recorded in
the [terrain session decision](../../docs/adr/TerrainNetworkBootstrap.md).

## Spatial spawn windows

The spawn selection uses the authoritative play actor and indexed lookups
in the pinned terrain directory. An epoch-bound offer identifies the
center of a 3×3×3 source neighborhood. Both peers derive the same canonical
subset manifest. A window may contain at most 1 MiB of chunk artifacts.
The sender references verified immutable package leaves in place.

The initial GNS window trial transferred 24,578 bytes of the 74,190-byte
fixture. It delayed real fragment delivery and confirmed that field
admission alone did not make the peer ready. Native queries hit the local
terrain on both peers and the distant terrain on the server. Normal W
input moved the authoritative Character with a final height of 7.45355.
The Character movement gate rejected a move across the ready coverage
boundary. Matching packaged processes passed the readiness handshake.
The Windows evidence is under
`C:/Sandbox/Codex/Artifacts/TerrainFoundation2/20261127-004317`.

## Rolling windows and live edits

The Windows Release native GNS test used two peers over loopback. It held
the next region's payload and then held the acknowledgment after client
collision cutover. The server continued to enforce its coverage fence.
The next region reused matching encoded leaves. Motion entered its new
coverage after the acknowledgment arrived.

An authorized server Luau fill installed native collision and exact
material ID 17 on the connected client. The second peer joined while
that edit was pending and its original spawn data was held. The server
moved the second player to a distant slab during the delay. Motion
stayed fenced until the new window activated. The distant collider
then admitted movement and the initial window retired. Reconnect
received a new epoch and repeated the collision gate.

The matching native and packaged runs are recorded in
`C:/Sandbox/Codex/Artifacts/TerrainFoundation2/20261127-011326`.
A separate packaged run removed all local client GTCH files and reached
native readiness. Full validation and offline startup rejected the
same incomplete package. Its receipt is under
`C:/Sandbox/Codex/Artifacts/TerrainPackageClient/20261127-010805`.
The full local game session suite and the packaging suite passed. The
Windows packaging replay used a short task-owned temporary directory
to stay within the fixture's path budget.

The pressure trial used a 90,214-byte spawn window and a 64 KiB reliable
queue. It held delivery for 64 simulation ticks. The first run revealed
that GNS pending bytes could exhaust transport capacity even when the
scheduler held only one fragment. The corrected sender waits for both
scheduler capacity and native pending-byte headroom.
The repeat passed fill, paint, excavation, unaffected distant movement,
and renewed movement after reconnect. The native replica test also
proved that paint keeps the provider body and that collider admission
denial preserves the prior visual and physical pair. The pressure log
is under `C:/Sandbox/Codex/Artifacts/TerrainFoundation2/20261127-012202`.

## Bilateral movement release

Version 4 requires the server's exact regional confirmation before the
client widens movement coverage. The Windows native GNS regression held the
client's initial receipt, committed a server edit in the installed region,
then held the server confirmation. The client installed the new collision
while local movement stayed locked. A same-revision rolling handoff retained
the old and new coverage intersection on both peers through the delay.
After reconnect the regression held a later confirmation through the client
deadline and confirmed that client terrain queries failed closed.

The regression passed with both a 65,536-byte and a 16,412-byte negotiated
reliable queue. The smaller queue carried at most 8,146 terrain payload bytes
per frame and transferred the same 90,214-byte startup window. The independent
review also checked that ordinary reliable scheduling can leave a separate
control opportunity and that an edit before offer admission is coalesced
under the existing deadline.

The full Windows native session suite and matching headless Development
player and server packages passed. The packaged client logged its
server-confirmed movement release. The receipt and native logs are under
`C:/Sandbox/Codex/Artifacts/TerrainFoundation2/20261127-020217`.

Ubuntu 24.04 with GCC 13 and the same pinned GNS source passed the
version-4 native runtime test and both queue profiles. Both runs
transferred 90,214 startup bytes and recorded a final ground Y of
7.45355. The runner used the matching network source and test hashes
inside the isolated Linux worker. Its hash manifest and raw output are
under `C:/Sandbox/Codex/Artifacts/TerrainEngineLinuxGns/20261127-015113`.

## Actor envelope at half spacing

Each region keeps a four-sample inset from its source-window edges for
contours, normals, and conditioned collision. The upper edge is exclusive.
Windows centered at X=0 and X=−1 at half spacing cover X=[−14,30) and
X=[−30,14), respectively. Their common coverage admits the full
conservative radius-9 bounds of the tested 12×12×6 anchored part.

Real Windows and Linux GNS ran the public Luau possession input and
authoritative pose route. With the client's acknowledgment held, its
western collider was already installed and the actor stopped at X=−5.
The actor reached X=−8.5 after confirmation and its pose replicated. An
actor resized beyond a radius of 22 could no longer move within a single
half-spacing window. Native motion fails closed when the actor's full
bounds cannot fit in the admitted region.

The raw logs are under
`C:/Sandbox/Codex/Artifacts/TerrainFoundation2/Envelope-Windows` and
`C:/Sandbox/Codex/Artifacts/TerrainEngineLinuxGns/20261127-015113/terrain-envelope-matrix.log`.

## Repeated edits in established regions

Run `gargantuan_game_session_tests --terrain-gns-churn 120` in a
GNS-enabled build. The optional workload keeps two established sessions
and their Characters on supported terrain. It alternates authoritative
fill, paint, and excavation over 120 cycles. For every revision it checks
the nearby movement fence, the released replica, a 27-sample exact
stencil, and native terrain ray parity. The distant Character moves on
the server and on its own client through each edit.

| Result | Windows Release | Linux Release |
| --- | ---: | ---: |
| Committed and converged edits | 360 | 360 |
| Exact signed density/material samples | 9,720 | 9,720 |
| Edit loop wall time | 25.82 s | 27.07 s |
| Total terrain fragment payload | 2,564,180 B | 2,564,180 B |
| Maximum fragment payload for one edit | 7,864 B | 7,864 B |
| Distant peer terrain payload | 0 B | 0 B |
| Peak near replica pages, meshes, colliders | 4, 2, 2 | 4, 2, 2 |
| Peak near replica field and render bytes | 119,488 B and 74,080 B | 119,488 B and 74,080 B |

The run advanced authoritative revisions 9 through 368. Its limits
were 64 KiB of terrain fragment payload per operation and 10 MiB
for the full workload. These are payload and owned-resource figures.
Transport framing, process working set and operating-system caches
have separate costs. The fixture edits one recurring region while
the other peer remains distant.

Raw logs are under
`C:/Sandbox/Codex/Artifacts/TerrainFoundation2/Churn-Windows/churn-120.log`
and
`C:/Sandbox/Codex/Artifacts/TerrainEngineLinuxGns/20261127-015113/churn/cycles120-gns-churn.log`.
