# Local record and control interfaces

`pl64::runtime` is the common C++23 transport for the instrument services. It is
built here and supplied to the radar repository as a CMake package. It contains
no DSP, device control or MCAP writer. The shared FlatBuffers helper is header
only; callers supply the system FlatBuffers dependency.

## Scientific records

Each producer has one Unix stream listener per record stream. Connections receive
a schema handshake followed by immutable records. The topic, binary FlatBuffers
schema, schema name, effective configuration, host boot ID and Linux executable
path are carried in the handshake. Data is never sent as a large socket payload.

Protocol version 1 uses this 64-byte little-endian header with one `SCM_RIGHTS`
descriptor per message:

| Field | Type |
| --- | --- |
| magic (`0x50363442`), version (`1`) | two uint32 |
| producer stream ID, sequence | two uint64 |
| host monotonic ns, Unix UTC ns | two uint64 |
| byte offset, payload length, mapping capacity | three uint64 |

Sequence zero is the JSON schema handshake and needs no acknowledgement. Data
sequences start at one and increase for the lifetime of a publisher. Capture IDs
inside radar records distinguish repeated captures within one publisher lifetime.
A publisher restart gets a new random stream ID. Native acquisition coordinates
and clock-domain uncertainty remain in the typed payload; header times do not
replace them.

The producer reserves a bounded `SharedBlock`. `MappedBuilder` builds the
FlatBuffer directly in that mapping. It rejects growth beyond the reserved
capacity instead of reallocating and copying a scientific payload. Commit
unmaps producer writes and applies `F_SEAL_WRITE`, `F_SEAL_GROW`, `F_SEAL_SHRINK`
and `F_SEAL_SEAL`. Subscribers check those seals and the file size, then map the
payload read-only. After mapping they close the received descriptor; the mapping
keeps the immutable object alive.

`Frame` is a lease: its destructor unmaps and acknowledges its uint64 sequence
on the connection. Leases may outlive the `Subscription`, be released out of
order, or survive producer exit. No ring slot is overwritten under a reader.

The default budget is 8 outstanding records and 128 MiB **per subscriber**, with
at most 16 subscribers and a 64 MiB maximum record allocation. Navigation uses
64 small-record credits. The preview stream uses 16 credits and a 16 MiB budget.
A full credit/byte budget, socket failure or short header write disconnects only
that subscriber. Producer acquisition never waits for subscriber work. Counters
expose disconnections and outstanding bytes. Consumers must reconnect, report
gaps, invalidate coherent apertures and never interpolate across unexplained
stream/session boundaries.

Linux uses RAM-backed, kernel-sealed memfd objects. The macOS development fallback
uses an unlinked temporary file and passes an independently opened read-only
descriptor after closing its writable descriptor. Linux is the deployed target;
the fallback is not a claim of a macOS kernel sealing or RAM-only guarantee.

## Control and telemetry

`ControlServer` accepts one newline-delimited JSON request per connection and
returns one JSON reply. Requests are bounded to 64 KiB, replies to 1 MiB, and
connections have a two-second lifetime. Replies contain `ok`; failures also
contain `error`. File permissions (0660 sockets, service runtime directory 0770)
and the instrument group determine access. No network listener is opened.

Worker commands return acceptance; status exposes effective state separately.
The common lifecycle names are Idle, Starting, Running, Draining and Fault.
The SAR foundation uses Monitoring to avoid implying that it forms images.

The GUI polls small status replies from a worker thread, sends explicit commands,
and subscribes only to display previews. Rendering never runs a blocking service
request. Docking, font/style scaling and ImPlot axes remain native widgets.

## Checks

`runtime-ipc` exercises control errors, schema handshakes, multi-subscriber
fan-out, nonzero payload offsets, slow-reader disconnection and immutable leases.
`system-recording` checks direct shared FlatBuffers through MCAP and replay.
The `mcap-crc` check compares the accelerated backend against an independent
bit-serial IEEE CRC, including the standard check vector, offsets, empty/null
updates and streamed partitions. Nix uses native zlib-ng; local builds may use
standard zlib. Both preserve MCAP chunk and data CRCs.
The separate end-to-end script checks real profile formation, recording changes
and combined navigation/radar/SAR archives.

`runtime-benchmark` is an optional target. It writes 64 × 16 MiB records once in
the producer and checksums every byte in two subscribers. Its reported rate
measures publication, mapping and reads on that host, not radar/DSP throughput.
