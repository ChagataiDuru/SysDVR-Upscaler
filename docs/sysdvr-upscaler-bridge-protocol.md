# SysDVR-Upscaler Bridge Protocol

This document describes protocol version 1 used between `SysDVR-UpscalerBridge` and `SysDVR-Upscaler`.

## Transport

The parent application creates one local Windows byte-stream named pipe and accepts one client. The bridge connects as a named-pipe client to `\\.\pipe\<pipe-name>`. Message boundaries are defined only by the binary headers below; Windows message-mode pipe boundaries are not used.

All integers are little-endian. Native C# or C++ structs are never serialized directly.

## Hello Message

The bridge writes one hello header immediately after connecting.

| Offset | Size | Field |
| --- | ---: | --- |
| 0 | 4 | Magic ASCII `SUBH` |
| 4 | 2 | Protocol version, currently `1` |
| 6 | 2 | Header size, currently `32` |
| 8 | 4 | Capability flags |
| 12 | 4 | Bridge process ID |
| 16 | 8 | Bridge monotonic timestamp frequency |
| 24 | 8 | Reserved, zero |

Capabilities currently defined:

| Bit | Name |
| ---: | --- |
| 0 | Video |
| 1 | SourceTimestamp |
| 2 | BridgeMonotonicTimestamp |
| 3 | DiscontinuityMessages |
| 4 | StatusMessages |

Unknown capability bits must be ignored.

## Stream Message

Every message after hello starts with a stream header.

| Offset | Size | Field |
| --- | ---: | --- |
| 0 | 4 | Magic ASCII `SUBP` |
| 4 | 2 | Protocol version, currently `1` |
| 6 | 2 | Header size, currently `48` |
| 8 | 4 | Message type |
| 12 | 4 | Flags |
| 16 | 4 | Payload size |
| 20 | 4 | Reserved, zero |
| 24 | 8 | Sequence number |
| 32 | 8 | Raw SysDVR source timestamp |
| 40 | 8 | Bridge monotonic timestamp |

Message types:

| Value | Name |
| ---: | --- |
| 1 | VideoPayload |
| 2 | Discontinuity |
| 3 | EndOfStream |
| 4 | Status |

Flags:

| Bit | Name |
| ---: | --- |
| 0 | PipeQueueRecovered |
| 1 | SysDvrSourceReconnect |
| 2 | BridgeShuttingDown |

Maximum payload size is 16 MiB. Larger declarations are rejected before allocation.

## Validation

The parent rejects unknown magic, unsupported protocol versions, headers smaller than the minimum, headers larger than the configured maximum, oversized payload declarations, truncated headers, truncated payloads, integer-overflow attempts, unknown message types, and unexpected message ordering.

Sequence gaps are logged and counted as transport discontinuities by higher-level telemetry. A discontinuity message means the parent should flush live parser/decoder state before accepting new decodable frames.

## Lifecycle

1. Parent creates the pipe server.
2. Bridge connects and writes `SUBH`.
3. Bridge writes zero or more `SUBP` messages.
4. `VideoPayload` messages carry raw H.264 payload bytes.
5. `Discontinuity` messages carry no required payload.
6. `EndOfStream` or pipe closure terminates the stream.

If the parent closes the pipe, the bridge stops accepting new queue items, releases all queued buffers, stops streaming, and exits headless mode.

## Compatibility

Protocol version 1 is strict. Future versions should increase the header size only when older readers can reject them safely. Unknown capability and flag bits are ignored unless a future version explicitly requires them.