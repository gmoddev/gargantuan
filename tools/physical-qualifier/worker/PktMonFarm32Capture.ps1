param(
    [Parameter(Mandatory=$true)][string]$EvidenceDir,
    [ValidateSet('Start','Stop','Finalize')][string]$Action = 'Start',
    [ValidateSet(39450,39452)][int]$CapturePort = 39450
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$Marker = Join-Path $EvidenceDir 'farm32-netsh-owner.json'
$Etl = Join-Path $EvidenceDir 'farm32-worker-capture.etl'
$Pcap = Join-Path $EvidenceDir 'farm32-worker-capture.pcapng'
# One noncircular ETL file. Reaching the reserve below the hard storage cap
# invalidates evidence rather than permitting silent truncation or wraparound.
$CaptureProfile = 'Farm32Capture16GiB-v2'
$StopPolicy = 'ExactOwnedControlTraceW-v1'
$TraceMaximumMiB = 16384
$NoWrapThresholdMiB = 15360
$UnixEpochTicks = [datetime]::new(1970, 1, 1, 0, 0, 0, [DateTimeKind]::Utc).Ticks
$EvidenceDir = [IO.Path]::GetFullPath($EvidenceDir)
$Etl = [IO.Path]::GetFullPath($Etl)
$Pcap = [IO.Path]::GetFullPath($Pcap)
$Marker = [IO.Path]::GetFullPath($Marker)
$NameHash = [Security.Cryptography.SHA256]::Create()
try { $NameBytes = $NameHash.ComputeHash([Text.Encoding]::UTF8.GetBytes($Etl.ToUpperInvariant())) }
finally { $NameHash.Dispose() }
$TraceSessionName = 'GargantuanFarm32-' + ([BitConverter]::ToString($NameBytes).Replace('-', '').ToLowerInvariant())

function InitializeFarm32Etw {
    if ('GargantuanQualification.Farm32EtwV1' -as [type]) { return }
    $EtwDefinition = @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
namespace GargantuanQualification {
    public sealed class Farm32EtwSession {
        public string Name, FileName, Guid;
        public ulong Handle;
        public uint BufferSizeKiB, MaximumFileSizeMiB, LogFileMode;
        public uint EventsLost, LogBuffersLost, BuffersWritten;
    }
    public static class Farm32EtwV1 {
        [StructLayout(LayoutKind.Sequential)]
        private struct Wnode {
            public uint BufferSize, ProviderId;
            public ulong HistoricalContext;
            public long TimeStamp;
            public Guid Guid;
            public uint ClientContext, Flags;
        }
        [StructLayout(LayoutKind.Sequential)]
        private struct Properties {
            public Wnode Wnode;
            public uint BufferSize, MinimumBuffers, MaximumBuffers, MaximumFileSize;
            public uint LogFileMode, FlushTimer, EnableFlags, AgeLimit, NumberOfBuffers;
            public uint FreeBuffers, EventsLost, BuffersWritten, LogBuffersLost, RealTimeBuffersLost;
            public IntPtr LoggerThreadId;
            public uint LogFileNameOffset, LoggerNameOffset;
        }
        private const int NameChars = 2048, MaximumSessions = 256;
        private static readonly int PropertiesSize = Marshal.SizeOf(typeof(Properties));
        private static readonly int AllocationSize = PropertiesSize + 4 * NameChars;
        [DllImport("advapi32.dll", CharSet=CharSet.Unicode, ExactSpelling=true)]
        private static extern uint QueryAllTracesW([In, Out] IntPtr[] Properties, uint Count, out uint Actual);
        [DllImport("advapi32.dll", CharSet=CharSet.Unicode, ExactSpelling=true)]
        private static extern uint ControlTraceW(ulong Handle, string Name, IntPtr Properties, uint Operation);
        public static int NativePropertiesSize { get { return PropertiesSize; } }
        private static IntPtr Allocate(string GuidText) {
            if (IntPtr.Size != 8 || PropertiesSize != 120 || Marshal.SizeOf(typeof(Wnode)) != 48)
                throw new InvalidDataException("Unexpected 64-bit Windows ETW layout");
            var Memory = Marshal.AllocHGlobal(AllocationSize);
            try {
                Marshal.Copy(new byte[AllocationSize], 0, Memory, AllocationSize);
                var Value = new Properties();
                Value.Wnode.BufferSize = (uint)AllocationSize;
                if (GuidText != null) Value.Wnode.Guid = new Guid(GuidText);
                Value.LoggerNameOffset = (uint)PropertiesSize;
                Value.LogFileNameOffset = (uint)(PropertiesSize + 2 * NameChars);
                Marshal.StructureToPtr(Value, Memory, false);
                return Memory;
            } catch { Marshal.FreeHGlobal(Memory); throw; }
        }
        private static string ReadString(IntPtr Memory, uint Offset, bool AllowAbsent) {
            if (Offset == 0 && AllowAbsent) return "";
            if (Offset < PropertiesSize || Offset >= AllocationSize || (Offset & 1) != 0)
                throw new InvalidDataException("Invalid ETW string offset");
            int Limit = Math.Min(NameChars, (AllocationSize - (int)Offset) / 2);
            for (int Length = 0; Length < Limit; ++Length) {
                if (Marshal.ReadInt16(Memory, (int)Offset + 2 * Length) == 0)
                    return Marshal.PtrToStringUni(IntPtr.Add(Memory, (int)Offset), Length);
            }
            throw new InvalidDataException("Unterminated ETW string");
        }
        private static Farm32EtwSession Read(IntPtr Memory) {
            var Value = (Properties)Marshal.PtrToStructure(Memory, typeof(Properties));
            return new Farm32EtwSession {
                Name=ReadString(Memory, Value.LoggerNameOffset, false),
                FileName=ReadString(Memory, Value.LogFileNameOffset, true),
                Guid=Value.Wnode.Guid.ToString("D"), Handle=Value.Wnode.HistoricalContext,
                BufferSizeKiB=Value.BufferSize, MaximumFileSizeMiB=Value.MaximumFileSize,
                LogFileMode=Value.LogFileMode, EventsLost=Value.EventsLost,
                LogBuffersLost=Value.LogBuffersLost, BuffersWritten=Value.BuffersWritten
            };
        }
        public static Farm32EtwSession[] All() {
            var Memory = new IntPtr[MaximumSessions];
            try {
                for (int Index = 0; Index < Memory.Length; ++Index) Memory[Index] = Allocate(null);
                uint Count;
                uint Code = QueryAllTracesW(Memory, (uint)Memory.Length, out Count);
                if (Code != 0 || Count > MaximumSessions)
                    throw new InvalidOperationException("QueryAllTracesW failed or exceeded bound: " + Code);
                var Result = new Farm32EtwSession[Count];
                for (int Index = 0; Index < Result.Length; ++Index) Result[Index] = Read(Memory[Index]);
                return Result;
            } finally { foreach (var Item in Memory) if (Item != IntPtr.Zero) Marshal.FreeHGlobal(Item); }
        }
        public static Farm32EtwSession Control(Farm32EtwSession Session, string Operation) {
            if (Session == null || String.IsNullOrEmpty(Session.Name) || Session.Name.Length > 1024 || Session.Handle == 0)
                throw new InvalidDataException("Exact ETW session identity is missing");
            uint Control;
            switch (Operation) { case "QUERY": Control=0; break; case "STOP": Control=1; break;
                case "FLUSH": Control=3; break; default: throw new InvalidDataException("Unsupported ETW operation"); }
            var Memory = Allocate(Session.Guid);
            try {
                // Use the exact unique name, never a potentially recycled numeric handle.
                // Callers still compare the queried GUID/handle/path before each mutation.
                uint Code = ControlTraceW(0, Session.Name, Memory, Control);
                if (Code != 0) throw new InvalidOperationException("ControlTraceW " + Operation + " failed: " + Code);
                return Read(Memory);
            } finally { Marshal.FreeHGlobal(Memory); }
        }
    }
}
'@
    Add-Type -TypeDefinition $EtwDefinition -ErrorAction Stop
}

function GetFarm32NativeSessions {
    InitializeFarm32Etw
    return [GargantuanQualification.Farm32EtwV1]::All()
}

function InvokeFarm32NativeControl($Session, [string]$Operation) {
    InitializeFarm32Etw
    return [GargantuanQualification.Farm32EtwV1]::Control($Session, $Operation)
}

function AssertSameFarm32Session($Expected, $Actual) {
    if ($null -eq $Actual -or $Expected.Name -cne $Actual.Name -or
        $Expected.Guid -cne $Actual.Guid -or $Expected.Handle -ne $Actual.Handle -or
        ![StringComparer]::OrdinalIgnoreCase.Equals($Expected.FileName, $Actual.FileName)) {
        throw 'Native trace identity changed.'
    }
}

function GetOwnedFarm32Session($Owner) {
    $Sessions = @(GetFarm32NativeSessions)
    $Matches = @($Sessions | Where-Object {
        ($null -ne $Owner -and $Owner.TraceIdentityRecorded -and $_.Name -eq $Owner.TraceSessionName) -or
        ($_.FileName -and [StringComparer]::OrdinalIgnoreCase.Equals([IO.Path]::GetFullPath($_.FileName), $Etl))
    })
    if ($Matches.Count -eq 0) { return $null }
    if ($Matches.Count -ne 1) { throw 'Native trace ownership is ambiguous.' }
    $Session = $Matches[0]
    if ($Session.Name -cne ('NetTrace-' + $TraceSessionName) -or !$Session.FileName -or
        ![StringComparer]::OrdinalIgnoreCase.Equals([IO.Path]::GetFullPath($Session.FileName), $Etl) -or
        $Session.Handle -eq 0 -or $Session.BufferSizeKiB -ne 512 -or
        $Session.MaximumFileSizeMiB -ne $TraceMaximumMiB -or
        ($Session.LogFileMode -band 1) -eq 0 -or ($Session.LogFileMode -band 14) -ne 0) {
        throw 'Native trace differs from the task-owned session.'
    }
    if ($null -ne $Owner -and $Owner.TraceIdentityRecorded) {
        if ($Owner.TraceSessionName -cne $Session.Name -or $Owner.TraceSessionGuid -cne $Session.Guid -or
            $Owner.TraceSessionHandle -cne $Session.Handle.ToString([Globalization.CultureInfo]::InvariantCulture)) {
            throw 'Native trace generation differs from the ownership marker.'
        }
    }
    return $Session
}

function WriteFarm32Owner($Owner, [bool]$Initial) {
    $Pending = $Marker + '.pending'
    $Stream = [IO.File]::Open($Pending, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        $Bytes = [Text.UTF8Encoding]::new($false).GetBytes(($Owner | ConvertTo-Json -Depth 6) + "`n")
        $Stream.Write($Bytes, 0, $Bytes.Length)
        $Stream.Flush()
    } finally { $Stream.Dispose() }
    try {
        if ($Initial) { [IO.File]::Move($Pending, $Marker) }
        else { [IO.File]::Replace($Pending, $Marker, [NullString]::Value) }
    } finally { if (Test-Path -LiteralPath $Pending) { Remove-Item -LiteralPath $Pending -Force } }
}

function StopOwnedFarm32Trace($Owner) {
    # No lease-age rejection here: service deadline, lease loss and restart
    # recovery deliberately invoke this fixed operation after expiry.
    $Session = GetOwnedFarm32Session $Owner
    if ($null -ne $Session) {
        $Queried = InvokeFarm32NativeControl $Session 'QUERY'
        AssertSameFarm32Session $Session $Queried
        $Flushed = InvokeFarm32NativeControl $Session 'FLUSH'
        AssertSameFarm32Session $Session $Flushed
        $Current = GetOwnedFarm32Session $Owner
        AssertSameFarm32Session $Session $Current
        $Queried = InvokeFarm32NativeControl $Session 'QUERY'
        AssertSameFarm32Session $Session $Queried
        # STOP errors may already have closed the session. Do not automatically
        # retry or fall back to netsh's global stop; preserve failure for recovery.
        $Stopped = InvokeFarm32NativeControl $Session 'STOP'
        $Owner.NativeStopRecorded = $true
        $Owner.NativeEventsLost = [uint32](($Session.EventsLost, $Flushed.EventsLost, $Queried.EventsLost, $Stopped.EventsLost | Measure-Object -Maximum).Maximum)
        $Owner.NativeLogBuffersLost = [uint32](($Session.LogBuffersLost, $Flushed.LogBuffersLost, $Queried.LogBuffersLost, $Stopped.LogBuffersLost | Measure-Object -Maximum).Maximum)
        $Owner.NativeBuffersWritten = $Stopped.BuffersWritten
        WriteFarm32Owner $Owner $false
        "[Qualification:Capture] NativeStop Name=$($Session.Name) Guid=$($Session.Guid) Handle=$($Session.Handle) BuffersWritten=$($Stopped.BuffersWritten) EventsLost=$($Stopped.EventsLost) LogBuffersLost=$($Stopped.LogBuffersLost)" | Write-Output
    }
    if ($null -ne (GetOwnedFarm32Session $Owner)) { throw 'Owned native trace remains after Stop.' }
    $Status = GetTraceStatus $TraceSessionName
    if ($Status -notmatch 'There is no trace session currently in progress' -and !$Status.Contains($Etl)) {
        throw 'A different netsh trace is protected during cleanup.'
    }
    # Query again immediately before named cleanup, protecting a reused name.
    if ($null -ne (GetOwnedFarm32Session $Owner)) { throw 'Native trace reappeared before netsh cleanup.' }
    $Output = & netsh trace stop "sessionname=$TraceSessionName" 2>&1
    $Code = $LASTEXITCODE
    $Text = $Output -join "`n"
    if ($Code -ne 0 -and $Text -notmatch 'There is no trace session currently in progress') { throw $Text }
    $Text | Write-Output
    if ((GetTraceStatus $TraceSessionName) -notmatch 'There is no trace session currently in progress' -or
        $null -ne (GetOwnedFarm32Session $Owner)) { throw 'Owned trace cleanup did not become idle.' }
    if ($Owner.NativeStopRecorded -and ($Owner.NativeEventsLost -ne 0 -or $Owner.NativeLogBuffersLost -ne 0)) {
        throw 'Native ETW loss was recorded; cleanup completed but capture is incomplete.'
    }
    if ($Owner.TraceIdentityRecorded -and !$Owner.NativeStopRecorded) {
        throw 'Native Stop success is unmeasured; cleanup completed but capture is incomplete.'
    }
}

function InvokeNetsh([string[]]$Arguments) {
    $Output = & netsh @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) { throw ($Output -join "`n") }
    return ($Output -join "`n")
}

function GetTraceStatus([string]$SessionName = '') {
    $Arguments = @('trace','show','status')
    if ($SessionName) { $Arguments += "sessionname=$SessionName" }
    $Output = & netsh @Arguments 2>&1
    $Text = $Output -join "`n"
    if ($LASTEXITCODE -ne 0 -and $Text -notmatch 'There is no trace session currently in progress') {
        throw $Text
    }
    return $Text
}

function AssertTraceHasNoLostEvents([string]$Source, [string]$Summary) {
    $Output = & tracerpt -l $Source -summary $Summary -o NUL -y 2>&1
    if ($LASTEXITCODE -ne 0) { throw ($Output -join "`n") }
    if (!(Test-Path -LiteralPath $Summary -PathType Leaf)) {
        throw 'Task-owned NDIS loss summary is missing.'
    }
    $Text = Get-Content -LiteralPath $Summary -Raw
    $Match = [regex]::Match($Text, '(?m)^Total Events\s+Lost\s+(\d+)\s*$')
    if (!$Match.Success) { throw 'Task-owned NDIS event-loss count is unavailable.' }
    if ([uint64]$Match.Groups[1].Value -ne 0) {
        throw 'Task-owned NDIS trace lost events; capture is incomplete.'
    }
}

function AssertTraceBelowBound([long]$EtlBytes) {
    if ($EtlBytes -ge $NoWrapThresholdMiB * 1MB) {
        throw 'Task-owned NDIS trace reached its completeness size bound; capture is incomplete.'
    }
}

function AssertFarm32DiskReserve([string]$PathName, [long]$RequiredBytes) {
    # Start reserves 16 GiB ETL + 16 GiB export + 2 GiB headroom. Finalize
    # reserves 16 GiB export + 1 GiB headroom in addition to the retained ETL.
    $Drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot([IO.Path]::GetFullPath($PathName)))
    if (-not $Drive.IsReady -or $Drive.AvailableFreeSpace -lt $RequiredBytes) {
        throw 'Farm32 evidence volume lacks the fixed capture/export reserve.'
    }
}

function WriteBlock([IO.BinaryWriter]$Writer, [uint32]$Type, [byte[]]$Body) {
    $Padding = (4 - ($Body.Length % 4)) % 4
    $Length = [uint32](12 + $Body.Length + $Padding)
    if ($Writer.BaseStream.Position + $Length -ge $NoWrapThresholdMiB * 1MB) {
        throw 'Farm32 pcap export reached its completeness size bound.'
    }
    $Writer.Write($Type)
    $Writer.Write($Length)
    $Writer.Write($Body)
    if ($Padding) { $Writer.Write([byte[]]::new($Padding)) }
    $Writer.Write($Length)
}

function ExportNdisTrace([string]$Source, [string]$Destination, [uint32]$IfIndex) {
    # The NDIS provider's packet event carries an Ethernet frame in property 3.
    # Packet Monitor's ETL converter cannot read this provider. Emit a complete
    # Ethernet pcapng so the normal qualifier validates the exact UDP tuples.
    $Events = Get-WinEvent -Path $Source -Oldest -ErrorAction Stop
    $Stream = [IO.File]::Open($Destination, [IO.FileMode]::CreateNew,
                              [IO.FileAccess]::Write, [IO.FileShare]::None)
    $Writer = [IO.BinaryWriter]::new($Stream)
    $Packets = 0
    try {
        $Body = [IO.MemoryStream]::new()
        $Part = [IO.BinaryWriter]::new($Body)
        $Part.Write([uint32]0x1A2B3C4D)
        $Part.Write([uint16]1)
        $Part.Write([uint16]0)
        $Part.Write([int64]-1)
        WriteBlock $Writer 0x0A0D0D0A $Body.ToArray()
        $Body.SetLength(0)
        $Body.Position = 0
        $Part.Write([uint16]1)
        $Part.Write([uint16]0)
        $Part.Write([uint32]65535)
        WriteBlock $Writer 1 $Body.ToArray()
        foreach ($Event in $Events) {
            if ($Event.ProviderName -ne 'Microsoft-Windows-NDIS-PacketCapture' -or $Event.Id -ne 1001) {
                continue
            }
            $Properties = $Event.Properties
            if ($Properties.Count -lt 4 -or [uint32]$Properties[0].Value -ne $IfIndex -or
                [uint32]$Properties[1].Value -ne $IfIndex) { continue }
            $Frame = [byte[]]$Properties[3].Value
            if ($Frame.Length -lt 14 -or $Frame.Length -ne [uint32]$Properties[2].Value) {
                throw 'NDIS packet event is truncated or malformed.'
            }
            $Microseconds = [int64](($Event.TimeCreated.ToUniversalTime().Ticks -
                                      $UnixEpochTicks) / 10)
            $Body.SetLength(0)
            $Body.Position = 0
            $Part.Write([uint32]0)
            $Part.Write([uint32]($Microseconds -shr 32))
            $Part.Write([uint32]($Microseconds -band 0xFFFFFFFFL))
            $Part.Write([uint32]$Frame.Length)
            $Part.Write([uint32]$Frame.Length)
            $Part.Write($Frame)
            WriteBlock $Writer 6 $Body.ToArray()
            $Packets++
        }
        if ($Packets -eq 0) { throw 'NDIS trace contains no complete frames from the worker fiber miniport.' }
    } finally {
        $Writer.Dispose()
    }
    return $Packets
}

function ExportNdisTraceFast([string]$Source, [string]$Destination, [uint32]$IfIndex) {
    # Stream one event and packet at a time. This versioned exporter rejects
    # a packet before it would reach the 15 GiB completeness threshold.
    # Conversion runs offline, after the separately bounded privileged Stop.
    if (-not ('GargantuanQualification.Farm32V2NdisExport' -as [type])) {
        $CompressedDefinition = 'H4sIAAAAAAACCrVWW1PbOBR+z684zcOOXYLXTqC7OwFmgEAns8DSBtoHhgdhK0SDI3kkOZfd8t/3SLKJDTa021mTIbF0rt/R+Y5yxfg9TNZK0/mwk1feghEj91wozWIVnCwo17gXfKYkofKZ5PivYafDyZyqjMQUPhJ5T7jOCf+Uk5RNWUw0Exz+6WT5XcpiUBoXYohTohScEjkf9L/0LxKmTlaZkBoFAZ9MsgXRFGLBlYZUoL9zsmLzfH601lTBPkS7Z/AeorC/U/8aNugrLU3El1IsGGaA2t1zFkuhxFRvf2U8EUu1fTEaT7YvSfxA9THJdC5pt26riFwiCoKnaxfVNWerk0zEsysWP2BcVsM8nC5hhGpXbE696I/fwh5E9hOWn3L3TwwguNaxH1gbCKf1WoPL+nIIeUU2E5HLmPbK5EYUq8Ut2D3IGdcw5prKKRZlzBO68gtkzWOtuVQNlOHwaWdBJHzKqVzjssnA1v5M3Ns1r3R5SfTsap3R4JSl1Lz0oPu+62/MWHE8LwsqFR0xSWN7CPZhSlJFN3LuJHnGqztcz9y6Rc+a8/0mtYnGeswLNROOW/BqcJj1c5HQ4Bj3NL2gy96TLfOY/cM4pkoFXyXT1GlMZkTS4EJwfI9gbw/6YXMMVqcM/Qi9yrVb8lwwfhV789jdiQPFc6IV8J4kngrYLGNB+kxjIRNwX/X95QyTAM8rRPYLiG0bW10PA3uHUedp+jzCSoZOvUnAPGxaSgTjxFiLwjCCb9+KiIKy5y6QIcx2+e43WjMPtixWLqfDRgmDN9rIqNTM8sDGT7E2bI1zIxMcixw7ZA92MNTWSI4FxwOsgytxjaUY9CsGbsLb4AtJc2oRrHfafzUZtZv8GbROpYF+v4LazaDwBETBHfLpzW07aIW6OyamsHYhOKP8Xs8QwehVCGvCmNZr+ffL/Nuz1TMplrbNxnyB4yVBDiUnq5hmtpW6hsMhs8QG1BxxYAq0zDlOIZqAkDAn6VTIOU2Crt+OWUHm5eEyLO2oI3Ehmvg5M+xGUkvwBXfD9rOB0OzCzTM7gdABT4yrDTIIzIcdz0sETgDqO7u/Yl8FYUvElh0cnRdM0XPA92pOWrSLObC19XL7sdP8tvll26ocJDhJ6qV7o1wv/Ln6aWkuEuZgE8YVcIG/51lKcQRPTVYKv8QcjVNYCvmAzDtld/h/zjgzA7JWWUlxkvMySbf+2Gma6wvBkjozV6m8IPkqDboVNzG8cBUehiP8O8wr3msi/d9bt8JVdNg/GhzvjFpFvFzNMDk/eksgbBPYjs7eDu0tbDYz6UfQidpBCf/HjD/s7g52v8P1W0kXvdWQca/gz7LfXnR2FRJzKbskSWLG6j54O8gWXo0gf4EBjmT8X7khGaViG3XMq+8N+rBVp9at0nAlWdObRcJHRBUXo+BSKGYvY1ul2YP92t36h1q4627wkMUkA+pu8OgmniHbMuSEsnM53q1Asb8p3OHkrZNvvWSt5XLRtnbQK8fIYublpjZ+jXUPDmDQ919XbGHQBslqQX5e0so8q2Z5eN4Zpq2LmwrZo1jI3H4XjI8d8/cv4QFStwgOAAA='
        $DefinitionBytes = [Convert]::FromBase64String($CompressedDefinition)
        $InputStream = [IO.MemoryStream]::new($DefinitionBytes)
        $ZipStream = [IO.Compression.GZipStream]::new($InputStream, [IO.Compression.CompressionMode]::Decompress)
        $Reader = [IO.StreamReader]::new($ZipStream, [Text.Encoding]::UTF8)
        try { $Definition = $Reader.ReadToEnd() }
        finally { $Reader.Dispose(); $ZipStream.Dispose(); $InputStream.Dispose() }
        # EventLogReader lives in System.Diagnostics.EventLog on PowerShell 7;
        # its actual assembly also preserves the Windows PowerShell 5 path.
        Add-Type -TypeDefinition $Definition -ReferencedAssemblies @(
            'System.Core.dll', [System.Diagnostics.Eventing.Reader.EventLogReader].Assembly.Location) -ErrorAction Stop
    }
    return [GargantuanQualification.Farm32V2NdisExport]::Export($Source, $Destination, $IfIndex)
}

if ($Action -eq 'Start') {
    AssertFarm32DiskReserve $EvidenceDir 34GB
    $Status = GetTraceStatus
    if ($Status -notmatch 'There is no trace session currently in progress') {
        throw 'An existing Windows trace session is protected.'
    }
    if ((GetTraceStatus $TraceSessionName) -notmatch 'There is no trace session currently in progress') {
        throw 'The named Windows trace session already exists.'
    }
    if ($null -ne (GetOwnedFarm32Session $null)) { throw 'An existing native trace is protected.' }
    foreach ($Artifact in @($Marker, ($Marker + '.pending'), $Etl, $Pcap, ($Pcap + '.pending'),
        (Join-Path $EvidenceDir 'farm32-capture-active.txt'),
        (Join-Path $EvidenceDir 'farm32-capture-summary.txt'))) {
        if (Test-Path -LiteralPath $Artifact) {
            throw "Farm32 capture artifact already exists: $Artifact"
        }
    }
    $Adapter = Get-NetAdapter -Name 'Ethernet 4'
    $IP = Get-NetIPAddress -InterfaceIndex $Adapter.ifIndex -AddressFamily IPv4 |
          Where-Object {$_.IPAddress -eq '10.253.3.2' -and $_.PrefixLength -eq 30}
    $If = Get-NetIPInterface -InterfaceIndex $Adapter.ifIndex -AddressFamily IPv4
    if ($Adapter.Status -ne 'Up' -or $Adapter.LinkSpeed -ne '10 Gbps' -or !$IP -or
        $If.NlMtu -ne 1500 -or $If.Dhcp -ne 'Disabled' -or
        $Adapter.MacAddress -ne '0C-42-A1-52-38-A8' -or $Adapter.ifIndex -ne 19) {
        throw 'Worker Mellanox configuration does not match the qualified direct link.'
    }
    $Guid = ([Guid]$Adapter.InterfaceGuid).ToString('B')
    $Owned = @{Profile=$CaptureProfile; Etl=$Etl; Pcap=$Pcap; InterfaceGuid=$Guid; MiniportIfIndex=$Adapter.ifIndex;
               CapturePort=$CapturePort; CaptureLayers=@('NDIS physical miniport');
               TraceMaximumMiB=$TraceMaximumMiB; NoWrapThresholdMiB=$NoWrapThresholdMiB;
               PerformanceMetadataMerge=$false; StopPolicy=$StopPolicy;
               ControlSessionName=$TraceSessionName; TraceSessionName=$null; TraceIdentityRecorded=$false;
               TraceSessionGuid=$null; TraceSessionHandle=$null;
               NativeStopRecorded=$false; NativeEventsLost=$null; NativeLogBuffersLost=$null; NativeBuffersWritten=$null}
    WriteFarm32Owner $Owned $true
    $StartArguments = @('trace','start','capture=yes','capturetype=physical',
        "CaptureInterface=$Guid",'Ethernet.Type=IPv4','Protocol=17',
        'IPv4.Address=10.253.3.1','CaptureMultiLayer=no','PacketTruncateBytes=1518',
        # perfMerge=no alone did not preserve V5. Native exact-session Stop is
        # a separate stop-policy candidate; keep all capture/storage filters.
        'report=disabled','perfMerge=no','persistent=no','fileMode=single','bufferSize=512',"maxSize=$TraceMaximumMiB","traceFile=$Etl",
        "sessionname=$TraceSessionName")
    InvokeNetsh $StartArguments | Write-Output
    $Session = GetOwnedFarm32Session $null
    if ($null -eq $Session) { throw 'Started trace has no exact native identity.' }
    $Queried = InvokeFarm32NativeControl $Session 'QUERY'
    AssertSameFarm32Session $Session $Queried
    $Owned.TraceSessionName = $Session.Name
    $Owned.TraceSessionGuid = $Session.Guid
    $Owned.TraceSessionHandle = $Session.Handle.ToString([Globalization.CultureInfo]::InvariantCulture)
    $Owned.TraceIdentityRecorded = $true
    WriteFarm32Owner $Owned $false
    $ActiveStatus = GetTraceStatus $TraceSessionName
    if (!$ActiveStatus.Contains($Etl)) { throw 'Windows trace did not become task-owned.' }
    $ActiveStatus | Set-Content -LiteralPath (Join-Path $EvidenceDir 'farm32-capture-active.txt')
} else {
    if (!(Test-Path -LiteralPath $Marker)) {
        if ($Action -eq 'Stop') {
            if ($null -ne (GetOwnedFarm32Session $null)) { throw 'Native trace exists without its ownership marker.' }
            return
        }
        throw 'Farm32 capture ownership marker is missing.'
    }
    if ((Get-Item -LiteralPath $Marker).Length -gt 65536 -or
        ((Get-Item -LiteralPath $Marker).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'Capture ownership marker is oversized or redirected.'
    }
    $Owned = Get-Content -Raw -LiteralPath $Marker | ConvertFrom-Json
    if ($Owned.NativeStopRecorded -isnot [bool]) { throw 'Native stop evidence state is missing.' }
    foreach ($Counter in @('NativeEventsLost','NativeLogBuffersLost','NativeBuffersWritten')) {
        $Value = $Owned.$Counter
        if (($Owned.NativeStopRecorded -and (($Value -isnot [int] -and $Value -isnot [long]) -or $Value -lt 0 -or $Value -gt [uint32]::MaxValue)) -or
            (!$Owned.NativeStopRecorded -and $null -ne $Value)) { throw 'Native stop counter is invalid.' }
    }
    $ParsedHandle = [uint64]0
    if ($Owned.TraceIdentityRecorded -and ![uint64]::TryParse($Owned.TraceSessionHandle, [ref]$ParsedHandle)) {
        throw 'Native trace handle is invalid.'
    }
    if ($Owned.Profile -ne $CaptureProfile -or
        $Owned.Etl -ne $Etl -or $Owned.Pcap -ne $Pcap -or $Owned.MiniportIfIndex -ne 19 -or
        $Owned.TraceMaximumMiB -ne $TraceMaximumMiB -or
        $Owned.NoWrapThresholdMiB -ne $NoWrapThresholdMiB -or
        $Owned.PerformanceMetadataMerge -isnot [bool] -or $Owned.PerformanceMetadataMerge -or
        $Owned.StopPolicy -cne $StopPolicy -or $Owned.ControlSessionName -cne $TraceSessionName -or
        $Owned.TraceIdentityRecorded -isnot [bool] -or
        ($Owned.TraceIdentityRecorded -and
         ($Owned.TraceSessionName -cne ('NetTrace-' + $TraceSessionName) -or
          $Owned.TraceSessionGuid -cnotmatch '^[0-9a-f]{8}-(?:[0-9a-f]{4}-){3}[0-9a-f]{12}$' -or
          $Owned.TraceSessionHandle -isnot [string] -or $Owned.TraceSessionHandle -cnotmatch '^[1-9][0-9]{0,19}$')) -or
        (!$Owned.TraceIdentityRecorded -and ($null -ne $Owned.TraceSessionName -or $null -ne $Owned.TraceSessionGuid -or $null -ne $Owned.TraceSessionHandle))) {
        throw 'Capture ownership marker does not match the evidence directory.'
    }
    if ($Action -eq 'Stop') {
        StopOwnedFarm32Trace $Owned
    } else {
        if (!$Owned.TraceIdentityRecorded) { throw 'Farm32 capture never recorded its native start identity.' }
        if (!$Owned.NativeStopRecorded -or $Owned.NativeEventsLost -ne 0 -or $Owned.NativeLogBuffersLost -ne 0) {
            throw 'Farm32 capture lacks a zero-loss native Stop receipt.'
        }
        if ((GetTraceStatus $TraceSessionName) -notmatch 'There is no trace session currently in progress' -or
            $null -ne (GetOwnedFarm32Session $Owned)) {
            throw 'Farm32 trace is still active; stop it before offline export.'
        }
    }
    if ($Action -eq 'Stop' -and !$Owned.TraceIdentityRecorded -and !(Test-Path -LiteralPath $Owned.Etl)) {
        # Failed Start never created a trace: absence was verified above. This
        # is cleanup only and can never become exportable capture evidence.
        return
    }
    if (Test-Path -LiteralPath $Owned.Etl) {
        # Single-file mode never wraps. A full file may have stopped recording
        # before the workload finished, so treat the reserve as incomplete.
        $EtlBytes = (Get-Item -LiteralPath $Owned.Etl).Length
        AssertTraceBelowBound $EtlBytes
        if ($Action -eq 'Finalize') {
            if (Test-Path -LiteralPath $Owned.Pcap) {
                throw 'Farm32 pcap already exists; refusing to replace it.'
            }
            if (Test-Path -LiteralPath (Join-Path $EvidenceDir 'farm32-capture-summary.txt')) {
                throw 'Farm32 loss summary already exists; refusing to replace it.'
            }
            AssertFarm32DiskReserve $EvidenceDir 17GB
            AssertTraceHasNoLostEvents $Owned.Etl (Join-Path $EvidenceDir 'farm32-capture-summary.txt')
            $Pending = $Owned.Pcap + '.pending'
            if (Test-Path -LiteralPath $Pending) {
                throw 'Farm32 pcap export already has a pending artifact.'
            }
            try {
                $Count = if ($EtlBytes -ge 4MB) {
                    ExportNdisTraceFast $Owned.Etl $Pending ([uint32]$Owned.MiniportIfIndex)
                } else {
                    ExportNdisTrace $Owned.Etl $Pending ([uint32]$Owned.MiniportIfIndex)
                }
                if ((Get-Item -LiteralPath $Pending).Length -ge $NoWrapThresholdMiB * 1MB) {
                    throw 'Farm32 pcap export exceeded its completeness size bound.'
                }
                Move-Item -LiteralPath $Pending -Destination $Owned.Pcap -ErrorAction Stop
                "Exported $Count complete fiber miniport frames." | Write-Output
            } finally {
                if (Test-Path -LiteralPath $Pending) { Remove-Item -LiteralPath $Pending -Force }
            }
        }
    } else {
        throw 'Task-owned Windows trace ETL is missing.'
    }
}
