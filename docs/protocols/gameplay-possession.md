# Gameplay possession wire formats

GPIN and GPOS version 1 use the GameSession ready phase. The header is 24
bytes. The first four bytes are the ASCII frame family, followed by a
little-endian unsigned 16-bit version word, an unsigned 8-bit kind,
unsigned 8-bit flags, the authoritative ObjectId slot and generation as two
unsigned 32-bit words, and the corresponding 64-bit epoch. The identity
and epoch are nonzero. The frame length is exact.

| Family | Kind | Bytes | Payload after header | Delivery |
| --- | --- | ---: | --- | --- |
| GPIN | 0 Bind, 1 Unbind | 32 | Server tick as u64 | Server to client, reliable Control |
| GPIN | 2 Input | 52 | Input sequence and client tick as u64, movement X/Y and facing yaw as f32 | Client to server, sequenced RealtimeState |
| GPOS | 0 Publish, 1 Unpublish | 32 | Required structural sequence as u64, zero on Unpublish | Server to client, reliable Control |
| GPOS | 2 Snapshot | 60 | Server tick as u64, position XYZ and quaternion XYZW as f32 | Server to client, sequenced RealtimeState |

Flags are zero except bit 0 on GPIN Input for jump or on GPOS Snapshot for
an initial or discontinuous pose. Integers and IEEE 754 binary32 fields
use little-endian order. Reliable Control has no application order field.
GPIN uses realtime channel 1 in the client-to-server direction and the input
sequence as its order. GPOS uses the part slot as its server-to-client
channel and the server tick as its order. GCHR per-object channels encode
the generation in their upper 32 bits. GPOS shares the transport and resolves
the full ObjectId.

GPIN admits up to four attempts per leased connection per server tick.
The movement vector must be finite with magnitude at most 1.001 and
the finite yaw must be within plus or minus pi. Client intent carries
only the input fields above. Input sequence and client tick advance
monotonically within the lease. Client tick orders that sender's inputs.
The server uses its own receipt clock for admission rate and freshness,
including after independent host scheduling stalls. The most recent sample
expires six ticks after server admission. A release followed by reassignment creates a
new lease epoch. The trusted live relation and its revision are checked
again at consumption.

GPOS accepts finite poses with a unit quaternion. The server installs a
fresh per-recipient epoch after the recipient's structural publish is
accepted by the scheduler. The client waits for the referenced structural
sequence and resolves the full source ObjectId before applying any pose.
Out-of-order, stale-epoch and duplicate samples do not change the current
pose. The client stores at most four samples per stream over fifteen
ticks, with six ticks of presentation delay and no extrapolation.
Structural removal and reliable Unpublish retire the corresponding lifetime.
