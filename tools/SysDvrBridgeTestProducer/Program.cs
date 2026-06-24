using System.Buffers.Binary;
using System.IO.Pipes;
using System.Text;

const ushort Version = 1;
const int HelloSize = 32;
const int HeaderSize = 48;
const uint MaxPayloadSize = 16 * 1024 * 1024;

var pipeName = ValueAfter("--pipe-name") ?? "SysDVR-Upscaler.Video";
var mode = ValueAfter("--case") ?? "valid";
var payloadPath = ValueAfter("--payload");
var payload = payloadPath is not null ? File.ReadAllBytes(payloadPath) : SamplePayload();

using var pipe = new NamedPipeClientStream(".", NormalizePipeName(pipeName), PipeDirection.Out, PipeOptions.Asynchronous);
await pipe.ConnectAsync(30_000);

switch (mode)
{
    case "bad-magic":
        await pipe.WriteAsync(CreateHello("BADH", Version));
        break;
    case "unsupported-version":
        await pipe.WriteAsync(CreateHello("SUBH", 99));
        break;
    case "oversized":
        await pipe.WriteAsync(CreateHello());
        await pipe.WriteAsync(CreateHeader(1, 0, MaxPayloadSize + 1u, 0, 0, 0));
        break;
    case "fragmented-header":
        await pipe.WriteAsync(CreateHello());
        foreach (var b in CreateHeader(1, 0, (uint)payload.Length, 0, 0, 0))
            await pipe.WriteAsync(new[] { b });
        await pipe.WriteAsync(payload);
        break;
    case "fragmented-payload":
        await pipe.WriteAsync(CreateHello());
        await pipe.WriteAsync(CreateHeader(1, 0, (uint)payload.Length, 0, 0, 0));
        foreach (var b in payload)
            await pipe.WriteAsync(new[] { b });
        break;
    case "coalesced":
        await pipe.WriteAsync(CreateHello());
        var first = Message(1, 0, payload, 0);
        var second = Message(1, 0, payload, 1);
        var combined = new byte[first.Length + second.Length];
        Buffer.BlockCopy(first, 0, combined, 0, first.Length);
        Buffer.BlockCopy(second, 0, combined, first.Length, second.Length);
        await pipe.WriteAsync(combined);
        break;
    case "discontinuity":
        await pipe.WriteAsync(CreateHello());
        await pipe.WriteAsync(CreateHeader(2, 1, 0, 0, 0, 0));
        await pipe.WriteAsync(Message(1, 0, payload, 1));
        break;
    case "disconnect":
        await pipe.WriteAsync(CreateHello());
        break;
    case "valid":
    default:
        await pipe.WriteAsync(CreateHello());
        await pipe.WriteAsync(Message(1, 0, payload, 0));
        break;
}

string? ValueAfter(string name)
{
    for (var i = 0; i + 1 < args.Length; ++i)
        if (string.Equals(args[i], name, StringComparison.OrdinalIgnoreCase))
            return args[i + 1];
    return null;
}

static string NormalizePipeName(string name)
{
    const string prefix = @"\\.\pipe\";
    return name.StartsWith(prefix, StringComparison.OrdinalIgnoreCase) ? name[prefix.Length..] : name;
}

static byte[] CreateHello(string magic = "SUBH", ushort version = Version)
{
    var bytes = new byte[HelloSize];
    Encoding.ASCII.GetBytes(magic).CopyTo(bytes, 0);
    BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(4, 2), version);
    BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(6, 2), HelloSize);
    BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(8, 4), 0x1f);
    BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(12, 4), unchecked((uint)Environment.ProcessId));
    BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(16, 8), 10_000_000);
    return bytes;
}

static byte[] CreateHeader(uint type, uint flags, uint payloadSize, ulong sequence, ulong sourceTimestamp, ulong bridgeTimestamp)
{
    var bytes = new byte[HeaderSize];
    Encoding.ASCII.GetBytes("SUBP").CopyTo(bytes, 0);
    BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(4, 2), Version);
    BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(6, 2), HeaderSize);
    BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(8, 4), type);
    BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(12, 4), flags);
    BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(16, 4), payloadSize);
    BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(24, 8), sequence);
    BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(32, 8), sourceTimestamp);
    BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(40, 8), bridgeTimestamp);
    return bytes;
}

static byte[] Message(uint type, uint flags, byte[] payload, ulong sequence)
{
    var header = CreateHeader(type, flags, checked((uint)payload.Length), sequence, sequence * 16_666, sequence * 16_666);
    var message = new byte[header.Length + payload.Length];
    Buffer.BlockCopy(header, 0, message, 0, header.Length);
    Buffer.BlockCopy(payload, 0, message, header.Length, payload.Length);
    return message;
}

static byte[] SamplePayload() => new byte[]
{
    0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x0C, 0x20, 0xAC, 0x2B, 0x40, 0x28, 0x02, 0xDD, 0x35, 0x01,
    0x0D, 0x01, 0xE0, 0x80, 0x00, 0x00, 0x00, 0x01, 0x68, 0xEE, 0x3C, 0xB0,
    0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x00
};