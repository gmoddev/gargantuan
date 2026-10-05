// Bounded diagnostic controller. Never changes the workload or its latency verdict.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace {
constexpr wchar_t SessionName[] = L"Gargantuan-CI-WorkloadScheduler";
constexpr ULONG TraceFlags = EVENT_TRACE_FLAG_PROCESS | EVENT_TRACE_FLAG_THREAD |
    EVENT_TRACE_FLAG_CSWITCH | EVENT_TRACE_FLAG_DISPATCHER;
constexpr DWORD WorkloadLimitMs = 600000;
constexpr DWORD WorkloadCreationFlags = CREATE_SUSPENDED | CREATE_NO_WINDOW |
    EXTENDED_STARTUPINFO_PRESENT | NORMAL_PRIORITY_CLASS;
bool OrdinaryPriority(DWORD Class, int Relative) noexcept {
    return Class == NORMAL_PRIORITY_CLASS && Relative == THREAD_PRIORITY_NORMAL;
}
constexpr ULONGLONG DecodeLimitMs = 180000;
constexpr std::uint64_t CsvLimit = 512ULL * 1024 * 1024;
constexpr std::uint64_t ChildLogLimit = 32ULL * 1024 * 1024; // Each stream, exact write cap.
constexpr GUID ThreadProvider{0x3d6fa8d1, 0xfe05, 0x11d0, {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};
constexpr GUID KernelControl{0x9e814aad, 0x3204, 0x11d2, {0x9a, 0x82, 0x00, 0x60, 0x08, 0xa8, 0x69, 0x39}};
constexpr GUID NullGuid{};
constexpr char CsvHeader[] = "Qpc,Processor,Opcode,Version,HeaderPid,HeaderTid,NewTid,OldTid,TargetTid,TargetPid,OldWaitReason,OldWaitMode,OldState,ReadyAdjustReason,ReadyAdjustIncrement,ReadyFlags\n";
enum class FixedCase { Full, AckStats, Aggregate32, Aggregate32Structural };
struct CaseSpec {
    const wchar_t *Binary;
    const wchar_t *Output;
    const wchar_t *Argument;
    const char *Name;
};
CaseSpec GetCaseSpec(FixedCase Case) {
    switch (Case) {
    case FixedCase::Full:
        return {L"build-ci/gargantuan_game_session_real_transport_tests.exe", L"build-ci/scheduler-trace", L"--reliable-workload", "Full"};
    case FixedCase::AckStats:
        return {L"build-ci/gargantuan_real_transport_tests.exe", L"build-ci/scheduler-trace-ack-stats", L"--ack-stats-boundary", "AckStats"};
    case FixedCase::Aggregate32:
        return {L"build-ci/gargantuan_game_session_real_transport_tests.exe", L"build-ci/scheduler-trace-aggregate32", L"--reliable-workload-32", "Aggregate32"};
    case FixedCase::Aggregate32Structural:
        return {L"build-ci/gargantuan_game_session_real_transport_tests.exe", L"build-ci/scheduler-trace-aggregate32-structural", L"--reliable-workload-32-structural", "Aggregate32Structural"};
    }
    throw std::runtime_error("unsupported fixed workload case");
}
struct Handle {
    HANDLE Value = nullptr;
    ~Handle() { if (Value && Value != INVALID_HANDLE_VALUE) CloseHandle(Value); }
    Handle() = default;
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    void Close() { if (Value && Value != INVALID_HANDLE_VALUE) CloseHandle(Value); Value = nullptr; }
};
struct Properties {
    EVENT_TRACE_PROPERTIES Value{};
    wchar_t Name[1024]{};
    wchar_t File[1024]{};
    Properties() {
        Value.Wnode.BufferSize = sizeof(*this);
        Value.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        Value.LoggerNameOffset = offsetof(Properties, Name);
        Value.LogFileNameOffset = offsetof(Properties, File);
    }
};
std::uint64_t Qpc() noexcept {
    LARGE_INTEGER Value{};
    if (!QueryPerformanceCounter(&Value)) return 0; // Metadata/coverage fails closed.
    return static_cast<std::uint64_t>(Value.QuadPart);
}
std::string JsonString(const std::string &Text) {
    std::string Result = "\"";
    for (const unsigned char Character : Text) {
        if (Character == '\\' || Character == '"') Result += '\\';
        if (Character < 32) throw std::runtime_error("control character in metadata");
        Result += static_cast<char>(Character);
    }
    return Result + '"';
}
std::string Utf8(const std::wstring &Text) {
    const int Length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Text.data(),
        static_cast<int>(Text.size()), nullptr, 0, nullptr, nullptr);
    if (Length <= 0) throw std::runtime_error("invalid path encoding");
    std::string Result(static_cast<std::size_t>(Length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Text.data(), static_cast<int>(Text.size()),
        Result.data(), Length, nullptr, nullptr);
    return Result;
}
GUID ParseGuid(const std::wstring &Text) {
    if (Text.size() != 36 || Text[8] != L'-' || Text[13] != L'-' || Text[18] != L'-' || Text[23] != L'-')
        throw std::runtime_error("invalid session GUID");
    auto Hex = [&](std::size_t At, std::size_t Count) {
        std::uint32_t Value = 0;
        for (std::size_t Index = At; Index < At + Count; ++Index) {
            const wchar_t C = Text[Index];
            if (!((C >= L'0' && C <= L'9') || (C >= L'a' && C <= L'f')))
                throw std::runtime_error("noncanonical session GUID");
            Value = (Value << 4) | static_cast<std::uint32_t>(C <= L'9' ? C - L'0' : C - L'a' + 10);
        }
        return Value;
    };
    GUID Value{Hex(0, 8), static_cast<WORD>(Hex(9, 4)), static_cast<WORD>(Hex(14, 4)), {}};
    constexpr std::array<std::size_t, 8> Positions{19, 21, 24, 26, 28, 30, 32, 34};
    for (std::size_t I = 0; I < Positions.size(); ++I) Value.Data4[I] = static_cast<BYTE>(Hex(Positions[I], 2));
    if (IsEqualGUID(Value, NullGuid) || IsEqualGUID(Value, KernelControl))
        throw std::runtime_error("reserved session GUID");
    return Value;
}
bool Owns(const Properties &Value, const GUID &Id, const std::wstring &Path) {
    return IsEqualGUID(Value.Value.Wnode.Guid, Id) &&
        std::wstring(Value.Name) == SessionName && std::wstring(Value.File) == Path;
}
ULONG Cleanup(const GUID &Id, const std::wstring &Path) {
    Properties Current;
    ULONG Status = ControlTraceW(0, SessionName, &Current.Value, EVENT_TRACE_CONTROL_QUERY);
    if (Status == ERROR_WMI_INSTANCE_NOT_FOUND) return ERROR_SUCCESS;
    if (Status != ERROR_SUCCESS) return Status;
    if (!Owns(Current, Id, Path)) return ERROR_ACCESS_DENIED;
    // Exact session identity was queried; never adopt a pre-existing session.
    Properties Stop;
    Stop.Value.Wnode.Guid = Id;
    return ControlTraceW(Current.Value.Wnode.HistoricalContext, nullptr, &Stop.Value, EVENT_TRACE_CONTROL_STOP);
}
struct Trace {
    TRACEHANDLE Id = 0;
    Properties State;
    ULONG StartStatus = ERROR_NOT_READY, QueryStatus = ERROR_NOT_READY, StopStatus = ERROR_NOT_READY;
    bool Started = false;
    void Start(const GUID &Guid, const std::wstring &Path) {
        if (Path.size() >= std::size(State.File)) throw std::runtime_error("ETL path too long");
        State.Value.Wnode.Guid = Guid;
        State.Value.Wnode.ClientContext = 1; // QPC, never CPU time.
        State.Value.LogFileMode = EVENT_TRACE_SYSTEM_LOGGER_MODE | EVENT_TRACE_FILE_MODE_SEQUENTIAL;
        State.Value.BufferSize = 64;
        State.Value.MinimumBuffers = 128;
        State.Value.MaximumBuffers = 128;
        State.Value.MaximumFileSize = 512;
        State.Value.EnableFlags = TraceFlags;
        std::copy(Path.begin(), Path.end(), State.File);
        StartStatus = StartTraceW(&Id, SessionName, &State.Value);
        Started = StartStatus == ERROR_SUCCESS;
    }
    void Stop() {
        if (!Started) return;
        Properties Query;
        QueryStatus = ControlTraceW(Id, nullptr, &Query.Value, EVENT_TRACE_CONTROL_QUERY);
        if (QueryStatus == ERROR_SUCCESS) State = Query;
        Properties Final;
        StopStatus = ControlTraceW(Id, nullptr, &Final.Value, EVENT_TRACE_CONTROL_STOP);
        if (StopStatus == ERROR_SUCCESS) State = Final;
        Started = false;
    }
    ~Trace() { Stop(); }
};
std::uint32_t U32(const BYTE *Data) { std::uint32_t Value; std::memcpy(&Value, Data, sizeof(Value)); return Value; }
struct Decoder {
    std::ofstream Csv;
    std::ostream *Stream = &Csv;
    std::uint64_t Rows = 0, Bytes = 0, Unsupported = 0, First = 0, Last = 0;
    std::uint64_t Switches = 0, Readies = 0, Lifecycles = 0;
    std::uint64_t UnsupportedLifecycle = 0;
    bool Capped = false, WriteFailed = false;
    bool TimedOut = false;
    ULONGLONG StartedMs = 0;
    std::uint64_t MainFirst = 0, MainLast = 0;
    DWORD MainThread = 0;
    void Event(EVENT_RECORD *Record) {
        if (!IsEqualGUID(Record->EventHeader.ProviderId, ThreadProvider)) return;
        const BYTE Op = Record->EventHeader.EventDescriptor.Opcode;
        if (Op != 36 && Op != 50 && Op != 1 && Op != 2 && Op != 3 && Op != 4) return;
        const BYTE Version = Record->EventHeader.EventDescriptor.Version;
        const auto *Data = static_cast<const BYTE *>(Record->UserData);
        const auto Tick = static_cast<std::uint64_t>(Record->EventHeader.TimeStamp.QuadPart);
        std::ostringstream Row;
        const unsigned Processor = (Record->EventHeader.Flags & EVENT_HEADER_FLAG_PROCESSOR_INDEX)
            ? Record->BufferContext.ProcessorIndex : Record->BufferContext.ProcessorNumber;
        Row << Tick << ',' << Processor << ',' << static_cast<unsigned>(Op) << ','
            << static_cast<unsigned>(Version) << ',' << Record->EventHeader.ProcessId << ',' << Record->EventHeader.ThreadId;
        bool MainEvent = false;
        if (Op == 36 && Version == 2 && Record->UserDataLength == 24) {
            const auto New = U32(Data), Old = U32(Data + 4);
            Row << ',' << New << ',' << Old << ",,," << static_cast<unsigned>(Data[12]) << ','
                << static_cast<unsigned>(Data[13]) << ',' << static_cast<unsigned>(Data[14]) << ",,,";
            MainEvent = New == MainThread || Old == MainThread;
            ++Switches;
        } else if (Op == 36 && Version == 5 && Record->UserDataLength == 28 &&
                   Record->EventHeader.Flags == 848) {
            // The reviewed 64-bit CSwitch v5 trace has the same four fields
            // needed for scheduler attribution at offsets 0/4/12/14. Byte 13
            // is ThreadFlags in v3/v4, not v2 OldWaitMode; do not reinterpret
            // it or the unneeded v5 trailing bytes. Unknown layouts still fail.
            const auto New = U32(Data), Old = U32(Data + 4);
            Row << ',' << New << ',' << Old << ",,," << static_cast<unsigned>(Data[12])
                << ",," << static_cast<unsigned>(Data[14]) << ",,,";
            MainEvent = New == MainThread || Old == MainThread;
            ++Switches;
        } else if (Op == 50 && Version == 2 && Record->UserDataLength == 8) {
            Row << ",,," << U32(Data) << ",,,,," << static_cast<unsigned>(Data[4]) << ','
                << static_cast<int>(static_cast<signed char>(Data[5])) << ',' << static_cast<unsigned>(Data[6]);
            MainEvent = U32(Data) == MainThread;
            ++Readies;
        } else if (Op <= 4 && (Version == 2 || Version == 3) && Record->UserDataLength >= 8) {
            // Documented Thread_TypeGroup payload begins with target PID and TID.
            Row << ",,," << U32(Data + 4) << ',' << U32(Data) << ",,,,,,";
            MainEvent = U32(Data + 4) == MainThread;
            ++Lifecycles;
        } else {
            if (Op == 36 || Op == 50) ++Unsupported; else ++UnsupportedLifecycle;
            return; // Raw ETL retained. Unknown layouts never guessed.
        }
        Row << '\n';
        const std::string Text = Row.str();
        // Every emitted row contains a newline, so the existing byte cap
        // also bounds Rows far below uint64_t. A separate observed row-count
        // cutoff truncated complete traces before reaching that hard cap.
        if (Bytes > CsvLimit || Text.size() > CsvLimit - Bytes) { Capped = true; return; }
        *Stream << Text;
        WriteFailed = !Stream->good();
        Bytes += Text.size(); ++Rows;
        if (!First || Tick < First) First = Tick;
        if (Tick > Last) Last = Tick;
        if (MainEvent) {
            if (!MainFirst || Tick < MainFirst) MainFirst = Tick;
            if (Tick > MainLast) MainLast = Tick;
        }
    }
};
void WINAPI OnEvent(EVENT_RECORD *Record) {
    auto *State = static_cast<Decoder *>(Record->UserContext);
    State->TimedOut = State->StartedMs && GetTickCount64() - State->StartedMs >= DecodeLimitMs;
    try { if (!State->Capped && !State->WriteFailed && !State->TimedOut) State->Event(Record); }
    catch (...) { State->WriteFailed = true; }
}
ULONG WINAPI OnBuffer(EVENT_TRACE_LOGFILEW *Log) {
    auto *State = static_cast<Decoder *>(Log->Context);
    State->TimedOut = State->StartedMs && GetTickCount64() - State->StartedMs >= DecodeLimitMs;
    return State->Capped || State->WriteFailed || State->TimedOut ? FALSE : TRUE;
}
struct DecodeResult {
    ULONG OpenStatus = ERROR_NOT_READY, ProcessStatus = ERROR_NOT_READY, HeaderEventsLost = 0, HeaderBuffersLost = 0;
    std::uint64_t Frequency = 0;
    ULONG ClockType = 0;
    std::uint64_t StartFileTime = 0, EndFileTime = 0;
};
DecodeResult Decode(const fs::path &Etl, Decoder &State) {
    DecodeResult Result;
    State.StartedMs = GetTickCount64();
    auto Name = Etl.wstring();
    EVENT_TRACE_LOGFILEW Log{};
    Log.LogFileName = Name.data();
    Log.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    Log.EventRecordCallback = OnEvent; Log.BufferCallback = OnBuffer; Log.Context = &State;
    TRACEHANDLE Reader = OpenTraceW(&Log);
    if (Reader == INVALID_PROCESSTRACE_HANDLE) { Result.OpenStatus = GetLastError(); return Result; }
    Result.OpenStatus = ERROR_SUCCESS;
    Result.HeaderEventsLost = Log.LogfileHeader.EventsLost;
    Result.HeaderBuffersLost = Log.LogfileHeader.BuffersLost;
    Result.Frequency = static_cast<std::uint64_t>(Log.LogfileHeader.PerfFreq.QuadPart);
    Result.ClockType = Log.LogfileHeader.ReservedFlags;
    Result.StartFileTime = static_cast<std::uint64_t>(Log.LogfileHeader.StartTime.QuadPart);
    Result.EndFileTime = static_cast<std::uint64_t>(Log.LogfileHeader.EndTime.QuadPart);
    Result.ProcessStatus = ProcessTrace(&Reader, 1, nullptr, nullptr);
    CloseTrace(Reader);
    return Result;
}
void RequirePlainLocalPath(const fs::path &Path) {
    if (!Path.is_absolute() || GetDriveTypeW(Path.root_path().c_str()) != DRIVE_FIXED)
        throw std::runtime_error("offline decode requires an absolute local fixed-drive path");
    for (const auto &Component : Path)
        if (Component == L"." || Component == L"..")
            throw std::runtime_error("offline decode path must have canonical components");
    for (auto Current = Path.lexically_normal();; Current = Current.parent_path()) {
        const DWORD Attributes = GetFileAttributesW(Current.c_str());
        if (Attributes == INVALID_FILE_ATTRIBUTES || (Attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("offline decode path missing or reparse-linked");
        if (Current == Current.parent_path()) break;
    }
}
DWORD ParseMainThread(const wchar_t *Text) {
    const std::wstring Value(Text);
    if (Value.empty() || Value.size() > 10 || !std::all_of(Value.begin(), Value.end(),
        [](wchar_t Character) { return Character >= L'0' && Character <= L'9'; }))
        throw std::runtime_error("offline decode requires a positive uint32 main thread ID");
    const auto Number = std::stoull(Value);
    if (Number == 0 || Number > UINT32_MAX)
        throw std::runtime_error("offline decode requires a positive uint32 main thread ID");
    return static_cast<DWORD>(Number);
}
int DecodeOnly(const fs::path &Input, const fs::path &Destination, DWORD MainThread) {
    // This branch only reads an existing ETL. It never constructs Trace,
    // starts a session, launches a child, or changes the retained raw files.
    RequirePlainLocalPath(Input);
    if (!fs::is_regular_file(Input) || fs::file_size(Input) >= CsvLimit)
        throw std::runtime_error("offline decode requires a bounded regular ETL");
    const auto Etl = fs::canonical(Input);
    const auto InputBytes = fs::file_size(Etl);
    if (!Destination.is_absolute() || Destination.filename().empty() || fs::exists(Destination))
        throw std::runtime_error("offline decode output must be a new exclusive directory");
    RequirePlainLocalPath(Destination.parent_path());
    if (!fs::is_directory(Destination.parent_path()))
        throw std::runtime_error("offline decode output parent must be a directory");
    const auto Parent = fs::canonical(Destination.parent_path());
    for (auto Current = Parent;; Current = Current.parent_path()) {
        if (fs::equivalent(Current, Etl.parent_path()))
            throw std::runtime_error("offline decode output cannot be inside the retained input directory");
        if (Current == Current.parent_path()) break;
    }
    const auto Output = Parent / Destination.filename();
    if (!fs::create_directory(Output))
        throw std::runtime_error("offline decode output directory was already created");
    Decoder Decoded;
    Decoded.MainThread = MainThread;
    Decoded.Csv.open(Output / "scheduler.csv", std::ios::binary);
    if (!Decoded.Csv.is_open()) throw std::runtime_error("offline decode CSV could not be created");
    Decoded.Csv << CsvHeader;
    Decoded.Bytes = sizeof(CsvHeader) - 1;
    const auto Result = Decode(Etl, Decoded);
    const auto DurationMs = GetTickCount64() - Decoded.StartedMs;
    Decoded.Csv.flush();
    Decoded.WriteFailed = Decoded.WriteFailed || !Decoded.Csv.good();
    Decoded.Csv.close();
    Decoded.WriteFailed = Decoded.WriteFailed || Decoded.Csv.fail();
    const bool Complete = Result.OpenStatus == ERROR_SUCCESS && Result.ProcessStatus == ERROR_SUCCESS &&
        Result.HeaderEventsLost == 0 && Result.HeaderBuffersLost == 0 && Result.ClockType == 1 && Result.Frequency > 0 &&
        !Decoded.Capped && !Decoded.WriteFailed && !Decoded.TimedOut && Decoded.Unsupported == 0 &&
        Decoded.Switches > 0 && Decoded.Readies > 0 && Decoded.MainFirst > 0 && Decoded.MainLast > Decoded.MainFirst &&
        fs::file_size(Output / "scheduler.csv") == Decoded.Bytes && fs::file_size(Etl) == InputBytes;
    std::ofstream Receipt(Output / "decode.json", std::ios::binary);
    Receipt << "{\n\"Format\":\"GargantuanSchedulerOfflineDecode\",\"Version\":1,\"Scope\":\"DECODE_ONLY\","
        << "\"NoChildLaunched\":true,\"NoCaptureStarted\":true,\"OriginalDiagnosticRelabeled\":false,"
        << "\"CausalVerdict\":\"NOT_CLAIMED\",\"DecodeComplete\":" << (Complete ? "true" : "false")
        << ",\"InputEtl\":" << JsonString(Utf8(Etl.wstring())) << ",\"InputEtlBytes\":" << InputBytes
        << ",\"OutputCsv\":" << JsonString(Utf8((Output / "scheduler.csv").wstring()))
        << ",\"MainThreadId\":" << MainThread << ",\"CsvBytes\":" << Decoded.Bytes
        << ",\"DecodeOpenStatus\":" << Result.OpenStatus << ",\"DecodeProcessStatus\":" << Result.ProcessStatus
        << ",\"HeaderEventsLost\":" << Result.HeaderEventsLost << ",\"HeaderBuffersLost\":" << Result.HeaderBuffersLost
        << ",\"QpcFrequency\":" << Result.Frequency << ",\"ClockType\":" << Result.ClockType
        << ",\"HeaderStartFileTime\":" << Result.StartFileTime << ",\"HeaderEndFileTime\":" << Result.EndFileTime
        << ",\"DecodedRows\":" << Decoded.Rows << ",\"UnsupportedEvents\":" << Decoded.Unsupported
        << ",\"UnsupportedLifecycleEvents\":" << Decoded.UnsupportedLifecycle
        << ",\"DecodedFirstQpc\":" << Decoded.First << ",\"DecodedLastQpc\":" << Decoded.Last
        << ",\"MainFirstQpc\":" << Decoded.MainFirst << ",\"MainLastQpc\":" << Decoded.MainLast
        << ",\"CsvCapped\":" << (Decoded.Capped ? "true" : "false")
        << ",\"DecodeTimedOut\":" << (Decoded.TimedOut ? "true" : "false")
        << ",\"WriteFailed\":" << (Decoded.WriteFailed ? "true" : "false")
        << ",\"DecodeDurationMs\":" << DurationMs
        << ",\"TraceFileCapBytes\":536870912,\"CsvCapBytes\":536870912,\"DecodeDeadlineMs\":180000\n}\n";
    Receipt.flush();
    if (!Receipt.good()) return 125;
    std::cout << "[Qualification:SchedulerDecode] decode_only=1 rows=" << Decoded.Rows
        << " bytes=" << Decoded.Bytes << " complete=" << Complete << " no-session-or-child-created\n";
    return Complete ? 0 : 125;
}
int SelfTest() {
    const auto Aggregate = GetCaseSpec(FixedCase::Aggregate32Structural);
    if (std::wstring(Aggregate.Binary) != L"build-ci/gargantuan_game_session_real_transport_tests.exe" ||
        std::wstring(Aggregate.Output) != L"build-ci/scheduler-trace-aggregate32-structural" ||
        std::wstring(Aggregate.Argument) != L"--reliable-workload-32-structural" ||
        std::string(Aggregate.Name) != "Aggregate32Structural" ||
        std::wstring(GetCaseSpec(FixedCase::Aggregate32).Binary) != L"build-ci/gargantuan_game_session_real_transport_tests.exe" ||
        std::wstring(GetCaseSpec(FixedCase::Aggregate32).Output) != L"build-ci/scheduler-trace-aggregate32" ||
        std::wstring(GetCaseSpec(FixedCase::Aggregate32).Argument) != L"--reliable-workload-32" ||
        std::string(GetCaseSpec(FixedCase::Aggregate32).Name) != "Aggregate32" ||
        std::wstring(GetCaseSpec(FixedCase::Full).Argument) != L"--reliable-workload" ||
        std::wstring(GetCaseSpec(FixedCase::AckStats).Argument) != L"--ack-stats-boundary") return 13;
    try { (void)GetCaseSpec(static_cast<FixedCase>(99)); return 14; } catch (...) {}
    const GUID Id = ParseGuid(L"11111111-2222-4333-8444-555555555555");
    Properties Value;
    Value.Value.Wnode.Guid = Id;
    wcscpy_s(Value.Name, SessionName); wcscpy_s(Value.File, L"C:\\owned.etl");
    if (!Owns(Value, Id, L"C:\\owned.etl") || Owns(Value, Id, L"C:\\other.etl")) return 1;
    Value.Value.Wnode.Guid.Data1++;
    if (Owns(Value, Id, L"C:\\owned.etl")) return 2;
    Value.Value.Wnode.Guid = Id; wcscpy_s(Value.Name, L"OtherSession");
    if (Owns(Value, Id, L"C:\\owned.etl")) return 9;
    Decoder State;
    std::ostringstream Rows;
    State.Stream = &Rows;
    State.MainThread = 17;
    std::array<BYTE, 24> Payload{};
    EVENT_RECORD Event{}; Event.EventHeader.ProviderId = ThreadProvider;
    Event.EventHeader.EventDescriptor.Opcode = 36; Event.EventHeader.EventDescriptor.Version = 2;
    Event.UserData = Payload.data(); Event.UserDataLength = 24;
    const DWORD New = 17, Old = 19;
    std::memcpy(Payload.data(), &New, 4); std::memcpy(Payload.data() + 4, &Old, 4);
    Payload[12] = 5; Payload[13] = 1; Payload[14] = 2;
    Event.EventHeader.TimeStamp.QuadPart = 123;
    State.Event(&Event);
    if (Rows.str() != "123,0,36,2,0,0,17,19,,,5,1,2,,,\n" || State.MainFirst != 123 || State.Switches != 1) return 3;
    Rows.str("");
    Event.EventHeader.EventDescriptor.Opcode = 50; Event.UserDataLength = 8;
    Payload[4] = 2; Payload[5] = 255; Payload[6] = 4;
    Event.EventHeader.TimeStamp.QuadPart = 140;
    State.Event(&Event);
    if (Rows.str() != "140,0,50,2,0,0,,,17,,,,,2,-1,4\n" || State.MainLast != 140 || State.Readies != 1) return 6;
    Rows.str("");
    std::array<BYTE, 29> V5{};
    std::memcpy(V5.data(), &New, 4); std::memcpy(V5.data() + 4, &Old, 4);
    V5[12] = 15; V5[13] = 0x60; V5[14] = 5;
    V5[24] = 8; V5[25] = 0x91; V5[26] = 0x7f; V5[27] = 3;
    Event.UserData = V5.data(); Event.UserDataLength = 28;
    Event.EventHeader.Flags = 848; Event.EventHeader.EventDescriptor.Opcode = 36;
    Event.EventHeader.EventDescriptor.Version = 5;
    Event.EventHeader.TimeStamp.QuadPart = 150;
    State.Event(&Event);
    if (Rows.str() != "150,0,36,5,0,0,17,19,,,15,,5,,,\n" || State.Switches != 2 ||
        State.MainLast != 150) return 11;
    Rows.str("");
    Event.UserDataLength = 27; State.Event(&Event);
    Event.UserDataLength = 29; State.Event(&Event);
    Event.UserDataLength = 28; Event.EventHeader.EventDescriptor.Version = 4; State.Event(&Event);
    Event.EventHeader.EventDescriptor.Version = 5; Event.EventHeader.Flags = 0; State.Event(&Event);
    if (!Rows.str().empty() || State.Unsupported != 4) return 12;
    Event.UserData = Payload.data(); Event.UserDataLength = 8;
    Event.EventHeader.Flags = 0; Event.EventHeader.EventDescriptor.Opcode = 50;
    Event.EventHeader.EventDescriptor.Version = 2;
    // Simulate a consistent large decoded prefix without allocating it.
    // The actual Event path must emit row5,000,001 below the unchanged byte cap.
    const auto RowBytes = std::string("150,0,50,2,0,0,,,17,,,,,2,-1,4\n").size();
    State.Rows = 5000000; State.Bytes = State.Rows * RowBytes;
    State.Event(&Event);
    if (State.Capped || State.Rows != 5000001 || Rows.str().empty()) return 3;
    Event.EventHeader.EventDescriptor.Version = 99;
    State.Event(&Event);
    if (State.Unsupported != 5) return 4;
    Event.EventHeader.EventDescriptor.Version = 2; Event.UserDataLength = 7;
    State.Event(&Event);
    if (State.Unsupported != 6) return 7;
    Event.UserDataLength = 8; State.Rows = 0; State.Bytes = CsvLimit - RowBytes;
    State.Capped = false; Rows.str(""); State.Event(&Event);
    if (State.Capped || State.Rows != 1 || State.Bytes != CsvLimit || Rows.str().size() != RowBytes) return 8;
    State.Rows = 0; State.Bytes = CsvLimit - RowBytes + 1;
    State.Capped = false; State.Event(&Event);
    if (!State.Capped || State.Rows != 0) return 8;
    State.Bytes = UINT64_MAX; State.Capped = false; State.Event(&Event);
    if (!State.Capped || State.Rows != 0) return 15;
    try { (void)ParseGuid(L"00000000-0000-0000-0000-000000000000"); return 5; } catch (...) {}
    try { (void)ParseGuid(L"9e814aad-3204-11d2-9a82-006008a86939"); return 10; } catch (...) {}
    std::cout << "[Qualification:SchedulerTrace] self-test=PASS v5-layout=PASS no-session-or-child-created\n";
    std::cout << "[Qualification:SchedulerTrace] rows-over-five-million=PASS byte-cap=PASS overflow-denial=PASS\n";
    return 0;
}
void Drain(HANDLE Pipe, const fs::path &Path, std::atomic<bool> &Capped, std::atomic<bool> &Failed) noexcept {
    try {
        std::ofstream File(Path, std::ios::binary);
        std::array<char, 65536> Buffer{};
        std::uint64_t Written = 0;
        DWORD Read = 0;
        while (true) {
            if (!ReadFile(Pipe, Buffer.data(), static_cast<DWORD>(Buffer.size()), &Read, nullptr)) {
                if (GetLastError() != ERROR_BROKEN_PIPE) Failed = true;
                break;
            }
            if (!Read) break;
            const auto Allowed = std::min<std::uint64_t>(Read, ChildLogLimit - Written);
            File.write(Buffer.data(), static_cast<std::streamsize>(Allowed));
            Written += Allowed;
            if (!File.good()) Failed = true;
            if (Allowed < Read) Capped = true;
        }
        File.flush(); if (!File.good()) Failed = true;
    } catch (...) { Failed = true; }
}
// This separate regression launches only exit-only copies of this helper. The
// existing --self-test still creates neither a session nor a child.
void PriorityTestChild(const std::wstring &Argument, DWORD Class, DWORD Expected) {
    std::array<wchar_t, 32768> Name{};
    const DWORD Length = GetModuleFileNameW(nullptr, Name.data(), static_cast<DWORD>(Name.size()));
    if (!Length || Length >= Name.size()) throw std::runtime_error("priority test helper path unavailable");
    const DWORD ParentClass = GetPriorityClass(GetCurrentProcess());
    Handle Job, Process, Thread, Input, Write, Read;
    Job.Value = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION Limits{};
    Limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!Job.Value || !SetInformationJobObject(Job.Value, JobObjectExtendedLimitInformation, &Limits, sizeof(Limits)))
        throw std::runtime_error("priority test job unavailable");
    SECURITY_ATTRIBUTES Security{sizeof(Security), nullptr, TRUE};
    if (!CreatePipe(&Read.Value, &Write.Value, &Security, 4096) ||
        !SetHandleInformation(Read.Value, HANDLE_FLAG_INHERIT, 0))
        throw std::runtime_error("priority test output pipe unavailable");
    Input.Value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &Security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (Input.Value == INVALID_HANDLE_VALUE) throw std::runtime_error("priority test input unavailable");
    SIZE_T AttributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &AttributeBytes);
    std::vector<BYTE> AttributeStorage(AttributeBytes);
    auto *Attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(AttributeStorage.data());
    if (!InitializeProcThreadAttributeList(Attributes, 1, 0, &AttributeBytes))
        throw std::runtime_error("priority test handle list unavailable");
    HANDLE Inherited[]{Write.Value, Input.Value};
    if (!UpdateProcThreadAttribute(Attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        Inherited, sizeof(Inherited), nullptr, nullptr)) {
        DeleteProcThreadAttributeList(Attributes);
        throw std::runtime_error("priority test handle list update failed");
    }
    STARTUPINFOEXW Startup{}; Startup.StartupInfo.cb = sizeof(Startup);
    Startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    Startup.StartupInfo.hStdOutput = Write.Value; Startup.StartupInfo.hStdError = Write.Value;
    Startup.StartupInfo.hStdInput = Input.Value; Startup.lpAttributeList = Attributes;
    PROCESS_INFORMATION Child{};
    std::wstring Command = L"\"" + std::wstring(Name.data(), Length) + L"\" " + Argument;
    const BOOL Created = CreateProcessW(Name.data(), Command.data(), nullptr, nullptr, TRUE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | Class,
        nullptr, nullptr, &Startup.StartupInfo, &Child);
    DeleteProcThreadAttributeList(Attributes);
    Write.Close(); Input.Close();
    if (!Created) throw std::runtime_error("priority test child unavailable");
    Process.Value = Child.hProcess; Thread.Value = Child.hThread;
    const bool Assigned = AssignProcessToJobObject(Job.Value, Process.Value) != FALSE;
    const DWORD Actual = GetPriorityClass(Process.Value);
    const int Relative = GetThreadPriority(Thread.Value);
    bool Passed = Assigned && Actual == Expected && Relative == THREAD_PRIORITY_NORMAL;
    if (Passed) {
        const DWORD Deadline = Argument == L"--priority-parent-test" ? 25000 : 5000;
        Passed = ResumeThread(Thread.Value) != static_cast<DWORD>(-1) &&
            WaitForSingleObject(Process.Value, Deadline) == WAIT_OBJECT_0;
        DWORD Exit = 125;
        Passed = Passed && GetExitCodeProcess(Process.Value, &Exit) && Exit == 0;
    }
    // Reap even on query, ownership or resume failure. No retry or replacement.
    TerminateProcess(Process.Value, 125);
    const bool RootReaped = WaitForSingleObject(Process.Value, 5000) == WAIT_OBJECT_0;
    TerminateJobObject(Job.Value, 125);
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION Accounting{};
    bool TreeReaped = false;
    for (unsigned Attempt = 0; Attempt < 100; ++Attempt) {
        if (!QueryInformationJobObject(Job.Value, JobObjectBasicAccountingInformation,
            &Accounting, sizeof(Accounting), nullptr)) break;
        if (Accounting.ActiveProcesses == 0) { TreeReaped = true; break; }
        Sleep(10);
    }
    std::cout << "[Qualification:PriorityCase] argument=" << Utf8(Argument) << " parent_class=" << ParentClass
        << " requested_class=" << Class << " expected_class=" << Expected << " actual_class=" << Actual
        << " relative_priority=" << Relative << " assigned=" << Assigned << " root_reaped=" << RootReaped
        << " tree_reaped=" << TreeReaped << " passed=" << Passed << '\n';
    if (!RootReaped || !TreeReaped) throw std::runtime_error("priority test reap failed");
    std::array<char, 512> Buffer{};
    DWORD Count = 0;
    std::size_t Total = 0;
    while (ReadFile(Read.Value, Buffer.data(), static_cast<DWORD>(Buffer.size()), &Count, nullptr) && Count) {
        Total += Count;
        if (Total > 8192) throw std::runtime_error("priority test child output exceeded bound");
        std::cout.write(Buffer.data(), Count);
    }
    if (GetLastError() != ERROR_BROKEN_PIPE) throw std::runtime_error("priority test output read failed");
    if (!Passed) throw std::runtime_error("priority test query/exit failed");
}
int PriorityParentTest() {
    const DWORD ParentClass = GetPriorityClass(GetCurrentProcess());
    if ((ParentClass != NORMAL_PRIORITY_CLASS && ParentClass != BELOW_NORMAL_PRIORITY_CLASS &&
         ParentClass != IDLE_PRIORITY_CLASS) || GetThreadPriority(GetCurrentThread()) != THREAD_PRIORITY_NORMAL)
        throw std::runtime_error("priority test parent outside fixed ordinary classes");
    PriorityTestChild(L"--priority-probe", 0, ParentClass);
    PriorityTestChild(L"--priority-probe", NORMAL_PRIORITY_CLASS, NORMAL_PRIORITY_CLASS);
    return 0;
}
int PrioritySelfTest() {
    const DWORD OriginalClass = GetPriorityClass(GetCurrentProcess());
    const int OriginalRelative = GetThreadPriority(GetCurrentThread());
    if (!OriginalClass || OriginalRelative == THREAD_PRIORITY_ERROR_RETURN)
        throw std::runtime_error("priority test controller query failed");
    for (const DWORD Class : {NORMAL_PRIORITY_CLASS, BELOW_NORMAL_PRIORITY_CLASS, IDLE_PRIORITY_CLASS})
        PriorityTestChild(L"--priority-parent-test", Class, Class);
    if (!OrdinaryPriority(NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_NORMAL) ||
        OrdinaryPriority(0, THREAD_PRIORITY_NORMAL) || OrdinaryPriority(NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_ERROR_RETURN) ||
        OrdinaryPriority(BELOW_NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_NORMAL) ||
        OrdinaryPriority(IDLE_PRIORITY_CLASS, THREAD_PRIORITY_NORMAL) ||
        OrdinaryPriority(HIGH_PRIORITY_CLASS, THREAD_PRIORITY_NORMAL) ||
        OrdinaryPriority(NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_BELOW_NORMAL) ||
        OrdinaryPriority(NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_ABOVE_NORMAL) ||
        GetPriorityClass(GetCurrentProcess()) != OriginalClass || GetThreadPriority(GetCurrentThread()) != OriginalRelative)
        throw std::runtime_error("priority test denial/controller preservation failed");
    std::cout << "[Qualification:SchedulerTrace] priority-self-test=PASS parents=3 default-inheritance-controls=3"
        " explicit-normal-children=3 query-denials=PASS own-children-reaped=PASS controller-unchanged=PASS no-session-or-workload-created\n";
    return 0;
}
int Run(const fs::path &Root, const GUID &Guid, const std::wstring &GuidText, FixedCase Case = FixedCase::Full) {
    if (!Root.is_absolute() || !fs::is_directory(Root)) throw std::runtime_error("absolute repository required");
    const auto Selection = GetCaseSpec(Case);
    const fs::path Binary = Root / Selection.Binary;
    if (!fs::is_regular_file(Binary)) throw std::runtime_error("fixed workload executable absent");
    const fs::path Output = Root / Selection.Output;
    if (!fs::create_directory(Output)) throw std::runtime_error("new output directory required");
    const fs::path Etl = Output / "scheduler.etl";
    SECURITY_ATTRIBUTES Security{sizeof(Security), nullptr, TRUE};
    Handle Stdout, Stderr, OutRead, ErrRead, Input, Job, Process, Thread;
    if (!CreatePipe(&OutRead.Value, &Stdout.Value, &Security, 65536) ||
        !CreatePipe(&ErrRead.Value, &Stderr.Value, &Security, 65536) ||
        !SetHandleInformation(OutRead.Value, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(ErrRead.Value, HANDLE_FLAG_INHERIT, 0))
        throw std::runtime_error("owned output pipes failed");
    Input.Value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &Security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Job.Value = CreateJobObjectW(nullptr, nullptr);
    if (Input.Value == INVALID_HANDLE_VALUE || !Job.Value)
        throw std::runtime_error("owned output/job creation failed");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION Limits{};
    Limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(Job.Value, JobObjectExtendedLimitInformation, &Limits, sizeof(Limits)))
        throw std::runtime_error("job ownership limit failed");
    Trace Recording;
    const auto StartQpc = Qpc();
    LARGE_INTEGER ControllerFrequency{};
    if (!QueryPerformanceFrequency(&ControllerFrequency)) throw std::runtime_error("QPC frequency unavailable");
    Recording.Start(Guid, Etl.wstring());
    const auto TraceStartedQpc = Qpc();
    if (Recording.StartStatus != ERROR_SUCCESS) {
        std::ofstream Failure(Output / "metadata.json", std::ios::binary);
        Failure << "{\"Format\":\"GargantuanSchedulerTrace\",\"Version\":1,\"DiagnosticComplete\":false,"
            << "\"WorkloadCase\":" << JsonString(Selection.Name)
            << ",\"WorkloadArguments\":[" << JsonString(Utf8(Selection.Argument)) << "],"
            << "\"State\":\"CAPTURE_UNAVAILABLE\",\"ChildLaunchAttempts\":0,\"ChildResumed\":false,"
            << "\"CausalVerdict\":\"NOT_CLAIMED\",\"StartStatus\":" << Recording.StartStatus
            << ",\"SessionGuid\":" << JsonString(Utf8(GuidText)) << ",\"EtlPath\":" << JsonString(Utf8(Etl.wstring())) << "}\n";
        return 125; // Diagnostic opt-in: never launch an untraced replacement workload.
    }
    SIZE_T AttributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &AttributeBytes);
    std::vector<BYTE> AttributeStorage(AttributeBytes);
    auto *Attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(AttributeStorage.data());
    if (!InitializeProcThreadAttributeList(Attributes, 1, 0, &AttributeBytes))
        throw std::runtime_error("child handle list initialization failed");
    HANDLE Inherited[]{Stdout.Value, Stderr.Value, Input.Value};
    if (!UpdateProcThreadAttribute(Attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, Inherited,
        sizeof(Inherited), nullptr, nullptr)) {
        DeleteProcThreadAttributeList(Attributes);
        throw std::runtime_error("child handle inheritance restriction failed");
    }
    STARTUPINFOEXW Startup{}; Startup.StartupInfo.cb = sizeof(Startup);
    Startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    Startup.StartupInfo.hStdOutput = Stdout.Value; Startup.StartupInfo.hStdError = Stderr.Value;
    Startup.StartupInfo.hStdInput = Input.Value;
    Startup.lpAttributeList = Attributes;
    PROCESS_INFORMATION Child{};
    std::wstring Command = L"\"" + Binary.wstring() + L"\" " + Selection.Argument;
    const BOOL Created = CreateProcessW(Binary.c_str(), Command.data(), nullptr, nullptr, TRUE,
        WorkloadCreationFlags, nullptr, Root.c_str(),
        &Startup.StartupInfo, &Child);
    const DWORD LaunchError = Created ? ERROR_SUCCESS : GetLastError();
    DWORD ExitCode = 125, OwnershipError = ERROR_SUCCESS;
    const DWORD ControllerPriorityClass = GetPriorityClass(GetCurrentProcess());
    DWORD ChildPriorityClass = 0, PriorityError = ERROR_SUCCESS;
    int ChildThreadPriority = THREAD_PRIORITY_ERROR_RETURN;
    bool PriorityVerifiedBeforeResume = false;
    if (Created) {
        Process.Value = Child.hProcess; Thread.Value = Child.hThread;
        if (!AssignProcessToJobObject(Job.Value, Process.Value)) {
            OwnershipError = GetLastError();
            TerminateProcess(Process.Value, 125); WaitForSingleObject(Process.Value, 10000);
        }
    }
    DeleteProcThreadAttributeList(Attributes);
    Stdout.Close(); Stderr.Close(); Input.Close();
    std::atomic<bool> LogCapped = false, LogFailed = false;
    std::thread OutputReader, ErrorReader;
    try {
        OutputReader = std::thread(Drain, OutRead.Value, Output / "workload.stdout.txt", std::ref(LogCapped), std::ref(LogFailed));
        ErrorReader = std::thread(Drain, ErrRead.Value, Output / "workload.stderr.txt", std::ref(LogCapped), std::ref(LogFailed));
    } catch (...) {
        OwnershipError = ERROR_NOT_ENOUGH_MEMORY; LogFailed = true;
        TerminateJobObject(Job.Value, 125);
        if (Created) { TerminateProcess(Process.Value, 125); WaitForSingleObject(Process.Value, 10000); }
        if (OutputReader.joinable()) OutputReader.join();
        if (ErrorReader.joinable()) ErrorReader.join();
    }
    bool TimedOut = false, TreeReaped = false, Resumed = false;
    std::uint64_t BeforeResumeQpc = 0, AfterChildExitQpc = 0;
    if (Created) {
        if (OwnershipError == ERROR_SUCCESS) {
            ChildPriorityClass = GetPriorityClass(Process.Value);
            const DWORD ClassError = ChildPriorityClass ? ERROR_SUCCESS : GetLastError();
            ChildThreadPriority = GetThreadPriority(Thread.Value);
            const DWORD ThreadError = ChildThreadPriority == THREAD_PRIORITY_ERROR_RETURN ? GetLastError() : ERROR_SUCCESS;
            PriorityError = ClassError ? ClassError : ThreadError;
            PriorityVerifiedBeforeResume = PriorityError == ERROR_SUCCESS &&
                OrdinaryPriority(ChildPriorityClass, ChildThreadPriority);
            if (!PriorityVerifiedBeforeResume && !PriorityError) PriorityError = ERROR_INVALID_DATA;
        }
        if (OwnershipError != ERROR_SUCCESS) {
            TerminateProcess(Process.Value, 125);
        } else if (!PriorityVerifiedBeforeResume) {
            OwnershipError = PriorityError; TerminateJobObject(Job.Value, 125);
        } else if ((BeforeResumeQpc = Qpc(), ResumeThread(Thread.Value)) == static_cast<DWORD>(-1)) {
            OwnershipError = GetLastError(); TerminateJobObject(Job.Value, 125);
        } else {
            Resumed = true;
            const ULONGLONG Started = GetTickCount64();
            while (true) {
                const DWORD Wait = WaitForSingleObject(Process.Value, 100);
                if (Wait == WAIT_OBJECT_0) break;
                TimedOut = GetTickCount64() - Started >= WorkloadLimitMs;
                if (Wait != WAIT_TIMEOUT || TimedOut || LogCapped || LogFailed) {
                    TerminateJobObject(Job.Value, TimedOut ? 124 : 125); break;
                }
            }
        }
        WaitForSingleObject(Process.Value, 10000);
        if (!GetExitCodeProcess(Process.Value, &ExitCode) || ExitCode == STILL_ACTIVE) ExitCode = 125;
        AfterChildExitQpc = Qpc();
        // Reap any surviving descendants even after a normal root exit.
        TerminateJobObject(Job.Value, 125);
        for (unsigned Attempt = 0; Attempt < 100; ++Attempt) {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION Accounting{};
            if (QueryInformationJobObject(Job.Value, JobObjectBasicAccountingInformation, &Accounting,
                sizeof(Accounting), nullptr) && Accounting.ActiveProcesses == 0) { TreeReaped = true; break; }
            Sleep(10);
        }
    }
    if (OutputReader.joinable()) OutputReader.join();
    if (ErrorReader.joinable()) ErrorReader.join();
    // Retain the original workload exit before potentially expensive ETL stop/decode.
    // The outer controller watchdog can therefore preserve it even if decoding stalls.
    {
        std::ofstream ChildResult(Output / "child-result.json", std::ios::binary);
        ChildResult << "{\"ChildResumed\":" << (Resumed ? "true" : "false")
            << ",\"ChildExitCode\":" << ExitCode << ",\"ChildPid\":" << Child.dwProcessId
            << ",\"RequestedChildCreationFlags\":" << WorkloadCreationFlags
            << ",\"ControllerPriorityClass\":" << ControllerPriorityClass
            << ",\"ChildPriorityClass\":" << ChildPriorityClass << ",\"ChildThreadPriority\":" << ChildThreadPriority
            << ",\"PriorityQueryError\":" << PriorityError
            << ",\"PriorityVerifiedBeforeResume\":" << (PriorityVerifiedBeforeResume ? "true" : "false")
            << ",\"ChildTreeReaped\":" << (TreeReaped ? "true" : "false") << "}\n";
        ChildResult.flush();
    }
    const auto EndQpc = Qpc();
    Recording.Stop();
    Decoder Decoded; Decoded.MainThread = Child.dwThreadId;
    Decoded.Csv.open(Output / "scheduler.csv", std::ios::binary);
    Decoded.Csv << CsvHeader; Decoded.Bytes = sizeof(CsvHeader) - 1;
    DecodeResult Decoding;
    try {
        if (Recording.StartStatus == ERROR_SUCCESS && fs::is_regular_file(Etl)) Decoding = Decode(Etl, Decoded);
    } catch (...) { Decoded.WriteFailed = true; }
    Decoded.Csv.flush();
    const bool DiagnosticComplete = Recording.StartStatus == 0 && Recording.QueryStatus == 0 && Recording.StopStatus == 0 &&
        Recording.State.Value.EventsLost == 0 && Recording.State.Value.LogBuffersLost == 0 &&
        Decoding.OpenStatus == 0 && Decoding.ProcessStatus == 0 && Decoding.HeaderEventsLost == 0 &&
        Decoding.HeaderBuffersLost == 0 && Decoding.ClockType == 1 && Decoding.Frequency > 0 &&
        !Decoded.Capped && !Decoded.WriteFailed && !Decoded.TimedOut && Decoded.Csv.good() && Decoded.Unsupported == 0 &&
        Decoded.Switches > 0 && Decoded.Readies > 0 && Decoded.MainFirst > 0 && Decoded.MainLast > Decoded.MainFirst &&
        fs::file_size(Etl) < 512ULL * 1024 * 1024 && !TimedOut && !LogCapped && !LogFailed && TreeReaped && Resumed &&
        PriorityVerifiedBeforeResume;
    std::ofstream Metadata(Output / "metadata.json", std::ios::binary);
    Metadata << "{\n\"Format\":\"GargantuanSchedulerTrace\",\"Version\":1,\n"
        << "\"WorkloadCase\":" << JsonString(Selection.Name)
        << ",\"WorkloadArguments\":[" << JsonString(Utf8(Selection.Argument)) << "],\n"
        << "\"SessionName\":" << JsonString(Utf8(SessionName)) << ",\"SessionGuid\":" << JsonString(Utf8(GuidText))
        << ",\"EtlPath\":" << JsonString(Utf8(Etl.wstring())) << ",\n"
        << "\"DiagnosticComplete\":" << (DiagnosticComplete ? "true" : "false") << ",\"CausalVerdict\":\"NOT_CLAIMED\",\n"
        << "\"StartStatus\":" << Recording.StartStatus << ",\"QueryStatus\":" << Recording.QueryStatus
        << ",\"StopStatus\":" << Recording.StopStatus << ",\"EventsLost\":" << Recording.State.Value.EventsLost
        << ",\"LogBuffersLost\":" << Recording.State.Value.LogBuffersLost << ",\"RealTimeBuffersLost\":" << Recording.State.Value.RealTimeBuffersLost
        << ",\"HeaderEventsLost\":" << Decoding.HeaderEventsLost << ",\"HeaderBuffersLost\":" << Decoding.HeaderBuffersLost << ",\n"
        << "\"ChildLaunchAttempts\":1,\"ChildPid\":" << Child.dwProcessId << ",\"ChildMainTid\":" << Child.dwThreadId
        << ",\"LaunchError\":" << LaunchError << ",\"OwnershipError\":" << OwnershipError
        << ",\"RequestedChildCreationFlags\":" << WorkloadCreationFlags
        << ",\"ControllerPriorityClass\":" << ControllerPriorityClass
        << ",\"ChildPriorityClass\":" << ChildPriorityClass << ",\"ChildThreadPriority\":" << ChildThreadPriority
        << ",\"PriorityQueryError\":" << PriorityError
        << ",\"PriorityVerifiedBeforeResume\":" << (PriorityVerifiedBeforeResume ? "true" : "false")
        << ",\"ChildExitCode\":" << ExitCode << ",\"ChildResumed\":" << (Resumed ? "true" : "false")
        << ",\"TimedOut\":" << (TimedOut ? "true" : "false") << ",\"ChildTreeReaped\":" << (TreeReaped ? "true" : "false") << ",\n"
        << "\"ChildLogCapped\":" << (LogCapped ? "true" : "false") << ",\"ChildLogFailed\":" << (LogFailed ? "true" : "false") << ",\n"
        << "\"ControllerStartQpc\":" << StartQpc << ",\"ControllerEndQpc\":" << EndQpc
        << ",\"AfterTraceStartQpc\":" << TraceStartedQpc << ",\"BeforeChildResumeQpc\":" << BeforeResumeQpc
        << ",\"AfterChildExitQpc\":" << AfterChildExitQpc
        << ",\"ControllerQpcFrequency\":" << ControllerFrequency.QuadPart
        << ",\"QpcFrequency\":" << Decoding.Frequency << ",\"ClockType\":" << Decoding.ClockType
        << ",\"HeaderStartFileTime\":" << Decoding.StartFileTime << ",\"HeaderEndFileTime\":" << Decoding.EndFileTime
        << ",\"DecodeOpenStatus\":" << Decoding.OpenStatus << ",\"DecodeProcessStatus\":" << Decoding.ProcessStatus
        << ",\"DecodedRows\":" << Decoded.Rows << ",\"UnsupportedEvents\":" << Decoded.Unsupported
        << ",\"UnsupportedLifecycleEvents\":" << Decoded.UnsupportedLifecycle
        << ",\"DecodedFirstQpc\":" << Decoded.First << ",\"DecodedLastQpc\":" << Decoded.Last
        << ",\"MainFirstQpc\":" << Decoded.MainFirst << ",\"MainLastQpc\":" << Decoded.MainLast
        << ",\"CsvCapped\":" << (Decoded.Capped ? "true" : "false")
        << ",\"DecodeTimedOut\":" << (Decoded.TimedOut ? "true" : "false")
        << ",\"TraceFileCapBytes\":536870912,\"CsvCapBytes\":536870912,\"ChildStreamCapBytes\":33554432"
        << ",\"WorkloadDeadlineMs\":600000,\"DecodeDeadlineMs\":180000\n}\n";
    Metadata.flush();
    if (!Metadata.good()) return ExitCode != 0 ? static_cast<int>(ExitCode) : 125;
    std::cout << "[Qualification:SchedulerTrace] child_exit=" << ExitCode << " diagnostic_complete=" << DiagnosticComplete << '\n';
    if (ExitCode != 0) return static_cast<int>(ExitCode);
    return DiagnosticComplete ? 0 : 125;
}
} // namespace
int wmain(int Count, wchar_t **Args) {
    try {
        if (Count == 2 && std::wstring(Args[1]) == L"--self-test") return SelfTest();
        if (Count == 2 && std::wstring(Args[1]) == L"--priority-self-test") return PrioritySelfTest();
        if (Count == 2 && std::wstring(Args[1]) == L"--priority-parent-test") return PriorityParentTest();
        if (Count == 2 && std::wstring(Args[1]) == L"--priority-probe")
            return GetPriorityClass(GetCurrentProcess()) != 0 &&
                GetThreadPriority(GetCurrentThread()) == THREAD_PRIORITY_NORMAL ? 0 : 125;
        if (Count == 5 && std::wstring(Args[1]) == L"--decode")
            return DecodeOnly(fs::path(Args[2]), fs::path(Args[3]), ParseMainThread(Args[4]));
        if (Count == 4 && std::wstring(Args[1]) == L"--run")
            return Run(fs::path(Args[2]), ParseGuid(Args[3]), Args[3]);
        if (Count == 4 && std::wstring(Args[1]) == L"--run-ack-stats")
            return Run(fs::path(Args[2]), ParseGuid(Args[3]), Args[3], FixedCase::AckStats);
        if (Count == 4 && std::wstring(Args[1]) == L"--run-aggregate32")
            return Run(fs::path(Args[2]), ParseGuid(Args[3]), Args[3], FixedCase::Aggregate32);
        if (Count == 4 && std::wstring(Args[1]) == L"--run-aggregate32-structural")
            return Run(fs::path(Args[2]), ParseGuid(Args[3]), Args[3], FixedCase::Aggregate32Structural);
        if (Count == 4 && std::wstring(Args[1]) == L"--cleanup")
            return Cleanup(ParseGuid(Args[2]), Args[3]) == ERROR_SUCCESS ? 0 : 125;
        std::cerr << "[Qualification:SchedulerTrace] invalid arguments\n";
    } catch (const std::exception &Error) {
        std::cerr << "[Qualification:SchedulerTrace] " << Error.what() << '\n';
    }
    return 125;
}
