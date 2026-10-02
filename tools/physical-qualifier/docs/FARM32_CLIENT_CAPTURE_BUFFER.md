# Farm32 client capture buffer candidate

The client adapter requests a fixed 64 MiB Npcap capture buffer with `-B 64`.
Its marker records `RequestedBufferMiB: 64`; campaign sealing and offline
acceptance require that value. This is requested configuration, not proof of
the driver's actual allocation or of zero-loss capacity. The existing helper
hash pin identifies the revised adapter. Historical markers remain historical
and do not satisfy this candidate's marker requirement.

Synthetic run `845edec4-c1f7-4f6e-96c2-3747ae9caba1` recorded 6,113,310 captured
packets and 1,022 `pcap` drops, with zero `dumpcap`, `flushed`, and `ps_ifdrop`
drops. The retained endpoint DATA ledgers passed. This is capture loss; the
diagnostic's rounded `100.0%` does not make it lossless. Driver-buffer overflow
is attributable, but its triggering reader delay (scheduling, storage, or
another cause) was not measured. Application memory pressure is not established.

The inspected binary is Wireshark dumpcap 4.6.8, SHA-256
`71f765535d59d9f2c1e2fb0000e0babd881c381447bde5ddeafc4683f1b598c4`.
In the [pinned capture utility](https://github.com/wireshark/wireshark/blob/v4.6.8/capture/capture-pcap-util.c#L1466-L1487),
the requested MiB value is converted to bytes before capture activation.
The [pinned Windows dispatch path](https://github.com/wireshark/wireshark/blob/v4.6.8/dumpcap.c#L3566-L3577)
writes on the capture thread with the current single-interface configuration.
The [Npcap counter definition](https://npcap.com/guide/wpcap/pcap_stats.html)
attributes `ps_drop` to capture-buffer exhaustion when packets are not read
quickly enough. It does not establish application UDP loss.

The [dumpcap manual](https://www.wireshark.org/docs/man-pages/dumpcap.html)
documents a default requested buffer of 2 MiB and warns that the effective
driver allocation can differ. At the intended combined 40,000 packets/s with
1,442-byte Ethernet frames, 2 MiB represents about 36 ms of frame bytes and
64 MiB about 1.16 s, before capture-record overhead. These are reservoir
estimates, not a guaranteed stall tolerance or service constant. The 64 MiB
request is a bounded operational candidate requiring fresh synthetic validation.

No threading, ring buffer, overwrite, snapshot-length reduction, filter change,
or timing/retention relaxation accompanies it. The 600-second capture,
630-second hard wrapper bound, 16 GiB autostop, strict 15 GiB completeness
threshold, 18 GiB disk reserve, and zero-drop acceptance remain unchanged.
In particular, `-t` is not added: its separate queue has its own bounded
capacity and drop counter and would require independent qualification.

Offline checks cover the exact argument array, fixed marker pin, and rejection
of the retained 1,022-drop diagnostic despite zero dumpcap drops. Before claiming
capacity qualification, a fresh bounded synthetic run must reach the intended
rate using the corrected pacing clock, retain exact endpoint DATA ledgers,
report zero capture drops at both ends, and satisfy the existing full-packet,
direction, size, export, and cleanup gates. Actual allocation and physical
zero-loss results for this buffer candidate are **NOT MEASURED** here.
