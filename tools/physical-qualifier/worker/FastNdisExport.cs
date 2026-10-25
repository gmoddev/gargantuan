using System;
using System.Diagnostics.Eventing.Reader;
using System.IO;

namespace GargantuanQualification {
public static class FastNdisExport {
    private const string Provider = "Microsoft-Windows-NDIS-PacketCapture";
    private static readonly long UnixEpochTicks =
        new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc).Ticks;

    public static long Export(string Source, string Destination, uint InterfaceIndex) {
        long Packets = 0;
        var Query = new EventLogQuery(Source, PathType.FilePath, "*");
        Query.ReverseDirection = false;
        using (var Reader = new EventLogReader(Query))
        using (var Stream = new FileStream(Destination, FileMode.CreateNew,
            FileAccess.Write, FileShare.None, 1 << 20))
        using (var Writer = new BinaryWriter(Stream)) {
            WriteSection(Writer);
            WriteInterface(Writer);
            EventRecord Record;
            while ((Record = Reader.ReadEvent()) != null) {
                using (Record) {
                    if (Record.Id != 1001 || Record.ProviderName != Provider)
                        continue;
                    var Properties = Record.Properties;
                    if (Properties.Count < 4 ||
                        Convert.ToUInt32(Properties[0].Value) != InterfaceIndex ||
                        Convert.ToUInt32(Properties[1].Value) != InterfaceIndex)
                        continue;
                    var Frame = Properties[3].Value as byte[];
                    if (Frame == null || Frame.Length < 14 ||
                        Frame.Length != Convert.ToUInt32(Properties[2].Value))
                        throw new InvalidDataException("NDIS packet event is truncated or malformed.");
                    var Ticks = Record.TimeCreated.Value.ToUniversalTime().Ticks - UnixEpochTicks;
                    long Microseconds = Convert.ToInt64((double)Ticks / 10.0);
                    WritePacket(Writer, Frame, Microseconds);
                    Packets++;
                }
            }
        }
        if (Packets == 0)
            throw new InvalidDataException(
                "NDIS trace contains no complete frames from the worker fiber miniport.");
        return Packets;
    }

    private static void WriteSection(BinaryWriter Writer) {
        Writer.Write(0x0A0D0D0Au);
        Writer.Write(28u);
        Writer.Write(0x1A2B3C4Du);
        Writer.Write((ushort)1);
        Writer.Write((ushort)0);
        Writer.Write(-1L);
        Writer.Write(28u);
    }

    private static void WriteInterface(BinaryWriter Writer) {
        Writer.Write(1u);
        Writer.Write(20u);
        Writer.Write((ushort)1);
        Writer.Write((ushort)0);
        Writer.Write(65535u);
        Writer.Write(20u);
    }

    private static void WritePacket(BinaryWriter Writer, byte[] Frame, long Microseconds) {
        int Padding = (4 - (Frame.Length & 3)) & 3;
        uint Length = (uint)(32 + Frame.Length + Padding);
        Writer.Write(6u);
        Writer.Write(Length);
        Writer.Write(0u);
        Writer.Write((uint)((ulong)Microseconds >> 32));
        Writer.Write((uint)Microseconds);
        Writer.Write((uint)Frame.Length);
        Writer.Write((uint)Frame.Length);
        Writer.Write(Frame);
        if (Padding != 0) Writer.Write(new byte[Padding]);
        Writer.Write(Length);
    }
}
}
