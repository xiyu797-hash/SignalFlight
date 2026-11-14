# SFP/1 — SignalFlight Packet Protocol

This is a versioned, intentionally small application protocol over IPv4 UDP. **All header integer fields are in network/big-endian order**. The payload contains interleaved `I,Q` float32 IEEE-754 values in little-endian order (`cf32_le`). Each datagram has exactly 56 bytes of header and 8..1200 bytes of nonempty payload, divisible by 8.

| Byte offset | Size | Field | Notes |
|---:|---:|---|---|
| 0 | 4 | Magic | ASCII `SFP1` (`0x53465031`) |
| 4 | 2 | Version | 1 |
| 6 | 2 | Header length | 56 |
| 8 | 4 | Source ID | independent source |
| 12 | 4 | Stream ID | independent logical stream |
| 16 | 8 | Sequence | increasing **per source+stream**, including intentionally omitted packets |
| 24 | 8 | Frame ID | increasing within stream |
| 32 | 2 | Chunk index | 0..chunk_count-1 |
| 34 | 2 | Chunk count | 1..128 |
| 36 | 2 | Format | 1 = cf32_le |
| 38 | 2 | Flags | reserved, sender currently sets 0 |
| 40 | 8 | Source timestamp (ns) | sender monotonic reading; not comparable across hosts without clock sync |
| 48 | 2 | Payload bytes | actual data byte count |
| 50 | 2 | Reserved | 0 |
| 52 | 4 | CRC32 | IEEE reflected CRC32 over all header bytes (CRC field zeroed) + payload |
| 56 | variable | Payload | cf32_le samples |

A 128-IQ-sample chunk occupies 1024 bytes; the UDP application payload totals 1080 bytes, below standard Ethernet MTU after IP/UDP overhead. A 1024-sample frame occupies eight chunks. No retransmission, transport-layer backpressure, FEC or on-wire fragmentation recovery is performed.

Receiver behavior:

1. Check length, magic, version, chunk indices, type and CRC32.
2. Maintain sliding sequence tracking per `(source_id, stream_id)`; detect duplicates and out-of-order arrival.
3. Track missing sequence IDs until a configurable reorder timeout; report `missing_final` for gaps surviving that timeout.
4. Reassemble frames by `(source_id, stream_id, frame_id)` with fixed chunk count and source timestamp.
5. Reject inconsistent frame definitions and process only complete frames of the expected IQ length.
6. Time out incomplete frames. Frames arriving again within the recent-completed window are ignored.

**Caveat:** A missing sequence ID is not proof that loss occurred in the network. It can also indicate a sender-side skipped packet, packet corruption, kernel overflow, or other unobservable loss. Timeout-classified loss is relative to a configurable patience period, not a promise that the packet can never arrive late.


## SFR0002 black-box recording format (v1.3)

SFR0002 is the capture container and is separate from the UDP SFP1 protocol. Each `.bin` file contains:

- 8-byte ASCII magic `SFR0002\n`
- Zero or more records: LE uint64 timestamp offset (ns), LE uint32 packet length, LE uint32 per-record CRC32, then the exact wire datagram bytes
- Mandatory 24-byte EOF footer: LE uint64 `UINT64_MAX` marker, LE uint64 number of records, LE uint64 rolling FNV-1a 64-bit digest of each record header and body

The digest detects accidental modification and aligned truncation. FNV is **not a cryptographic MAC or authentication mechanism**; untrusted or adversarial inputs still require separate authorization/integrity controls.

Existing `SFR0001\n` recordings remain readable but have no reliable aligned-truncation detection. `sf_replay` warns on them. New captures are preflight-validated before analysis or result-file creation.
