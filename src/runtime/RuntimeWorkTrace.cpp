#include "runtime/RuntimeWorkTrace.hpp"

#if defined(GARGANTUAN_WORK_TRACE)

#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <stdexcept>
#include <system_error>

namespace gargantuan::runtime_detail {
namespace {
	std::uint32_t LocalThreadId() noexcept {
		static std::atomic<std::uint32_t> Next{1};
		thread_local const std::uint32_t Id = Next.fetch_add(1, std::memory_order_relaxed);
		return Id;
	}

	std::uint64_t ParsePositive(const char *Setting, std::uint64_t Default, std::uint64_t Maximum) {
		const char *Value = std::getenv(Setting);
		if (!Value) return Default;
		const std::string_view Text(Value);
		std::uint64_t Result = 0;
		const auto [End, Error] = std::from_chars(Text.data(), Text.data() + Text.size(), Result);
		if (Error != std::errc{} || End != Text.data() + Text.size() || Result == 0 || Result > Maximum)
			throw std::invalid_argument(std::string("Invalid ") + Setting);
		return Result;
	}

	std::uint64_t Nanoseconds(std::chrono::steady_clock::duration Duration) noexcept {
		return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Duration).count());
	}
}

WorkTrace::WorkTrace(Options Value)
	: Configuration(std::move(Value)), Records([&] {
		if (Configuration.Capacity == 0 || Configuration.Capacity > MaximumRecords || Configuration.Stride == 0 ||
			Configuration.FirstTick > Configuration.LastTick)
			throw std::invalid_argument("Work trace capacity, stride or tick range is invalid");
		return Configuration.Capacity;
	}()) {}

std::unique_ptr<WorkTrace> WorkTrace::FromEnvironment() {
	const char *Path = std::getenv("GARGANTUAN_WORK_TRACE_FILE");
	if (!Path || !*Path) return nullptr;
	Options Settings;
	Settings.Path = Path;
	Settings.Capacity = static_cast<std::size_t>(ParsePositive("GARGANTUAN_WORK_TRACE_CAPACITY",
		Settings.Capacity, MaximumRecords));
	Settings.Stride = ParsePositive("GARGANTUAN_WORK_TRACE_STRIDE", Settings.Stride, 6'000);
	Settings.FirstTick = ParsePositive("GARGANTUAN_WORK_TRACE_FIRST_TICK", Settings.FirstTick, Settings.LastTick);
	Settings.LastTick = ParsePositive("GARGANTUAN_WORK_TRACE_LAST_TICK", Settings.LastTick, Settings.LastTick);
	if (const char *Mode = std::getenv("GARGANTUAN_WORK_TRACE_MODE")) {
		const std::string_view Value(Mode);
		if (Value != "summary" && Value != "detailed")
			throw std::invalid_argument("GARGANTUAN_WORK_TRACE_MODE must be summary or detailed");
		Settings.Detailed = Value == "detailed";
	}
	if (const char *Phases = std::getenv("GARGANTUAN_WORK_TRACE_PHASES")) {
		Settings.Phases.reset();
		std::string_view Remaining(Phases);
		for (;;) {
			const auto Separator = Remaining.find(',');
			const auto Phase = std::ranges::find(WorkPhaseNames, Remaining.substr(0, Separator));
			if (Phase == WorkPhaseNames.end())
				throw std::invalid_argument("GARGANTUAN_WORK_TRACE_PHASES contains an unknown or empty phase");
			Settings.Phases.set(static_cast<std::size_t>(Phase - WorkPhaseNames.begin()));
			if (Separator == std::string_view::npos) break;
			Remaining.remove_prefix(Separator + 1);
		}
	}
	return std::make_unique<WorkTrace>(std::move(Settings));
}

WorkTrace::Region::Region(WorkTrace *Trace, std::uint64_t Tick, std::uint32_t Lane) noexcept
	: Owner(Trace), Sink{this, RecordSpan}, Previous(ActiveWorkSpanSink), TickId(Tick), LaneId(Lane),
	  ThreadId(Trace && Trace->Configuration.Detailed ? LocalThreadId() : 0) {
	ActiveWorkSpanSink = ThreadId ? &Sink : nullptr;
}

WorkTrace::Region::~Region() { ActiveWorkSpanSink = Previous; }

void WorkTrace::Region::SetCurrentLane(std::uint32_t Lane) noexcept {
	if (ActiveWorkSpanSink && ActiveWorkSpanSink->Record == RecordSpan)
		static_cast<WorkTrace::Region *>(ActiveWorkSpanSink->Context)->LaneId = Lane;
}

void WorkTrace::Region::RecordSpan(void *Context, WorkPhase Phase, std::uint64_t Started,
	std::uint64_t Inclusive, std::uint64_t Exclusive) noexcept {
	auto &Region = *static_cast<WorkTrace::Region *>(Context);
	const auto Key = static_cast<std::size_t>(Phase);
	if (Region.Owner->Includes(Key))
		Region.Owner->Append({Kind::Span, Key, Region.ThreadId, Region.LaneId, Region.TickId, Started,
			{Inclusive, Exclusive}});
}

void WorkTrace::Append(Record Value) noexcept {
	const auto Index = Used.fetch_add(1, std::memory_order_relaxed);
	if (Index < Records.size()) Records[Index] = Value;
}

std::size_t WorkTrace::Dropped() const noexcept {
	const auto Count = Used.load(std::memory_order_relaxed);
	return Count - std::min(Count, Records.size());
}

std::size_t WorkTrace::BufferBytes() const noexcept { return Records.capacity() * sizeof(Record); }

void WorkTrace::RecordSample(std::uint64_t Tick, std::uint32_t Lane, std::uint64_t At, const WorkSample &Sample) noexcept {
	const auto Thread = LocalThreadId();
	for (std::size_t Index = 0; Index < Sample.size(); ++Index) {
		const auto &Value = Sample[Index];
		if (Includes(Index) && (Value.Calls || Value.Units || Value.Retained || Value.Rejected))
			Append({Kind::Phase, Index, Thread, Lane, Tick, At,
				{Value.Nanoseconds, Value.ExclusiveNanoseconds, Value.Calls, Value.Units,
				 Value.Allocations, Value.AllocatedBytes, Value.Retained, Value.Rejected}});
	}
	for (std::size_t Index = 0; Index < Sample.Producers.size(); ++Index) {
		const auto &Value = Sample.Producers[Index];
		if (Value.Enqueued || Value.Serviced || Value.Opportunities || Value.QueueDepthMaximum)
			Append({Kind::Queue, Index, Thread, Lane, Tick, At,
				{Value.Enqueued, Value.Serviced, Value.QueueDepthMaximum, Value.ServiceAgeMaximumNanoseconds,
				 Value.ServiceGapMaximumNanoseconds, Value.BudgetDeferrals, Value.CapacityDeferrals, Value.Opportunities}});
	}
	for (std::size_t Index = 0; Index < Sample.Counters.size(); ++Index)
		if (Sample.Counters[Index]) Append({Kind::Counter, Index, Thread, Lane, Tick, At, {Sample.Counters[Index]}});
}

void WorkTrace::RecordFrame(std::uint64_t Tick, std::uint32_t Lane, std::uint64_t Started, std::uint64_t Interval,
	std::uint64_t DeadlineLateness, bool DeadlineMissed) noexcept {
	Append({Kind::Frame, 0, LocalThreadId(), Lane, Tick, Started, {Interval, DeadlineLateness, DeadlineMissed}});
}

void WorkTrace::Write() const {
	std::ofstream Output(Configuration.Path, std::ios::out | std::ios::binary | std::ios::noreplace);
	if (!Output) throw std::runtime_error("Unable to create work trace output");
	Output.exceptions(std::ios::failbit | std::ios::badbit);
	Output.imbue(std::locale::classic());
	Output << std::fixed << std::setprecision(3) << "{\"traceEvents\":[\n";
	const auto Count = std::min(Used.load(std::memory_order_relaxed), Records.size());
	for (std::size_t Index = 0; Index < Count; ++Index) {
		if (Index != 0) Output << ",\n";
		const auto &Value = Records[Index];
		std::string_view Name = "Frame", Category = "frame";
		switch (Value.Type) {
			case Kind::Span: Name = WorkPhaseNames[Value.Key]; Category = "scope"; break;
			case Kind::Phase: Name = WorkPhaseNames[Value.Key]; Category = "phase"; break;
			case Kind::Queue: Name = WorkProducerNames[Value.Key]; Category = "queue"; break;
			case Kind::Counter: Name = WorkCounterNames[Value.Key]; Category = "counter"; break;
			case Kind::Frame: break;
		}
		Output << "{\"name\":\"" << Name << "\",\"cat\":\"" << Category << "\",\"pid\":1,\"tid\":" << Value.Thread
			<< ",\"ts\":" << static_cast<double>(Value.At - std::min(Value.At, Origin)) / 1'000.0;
		if (Value.Type == Kind::Span) Output << ",\"ph\":\"X\",\"dur\":" << static_cast<double>(Value.Values[0]) / 1'000.0;
		else Output << ",\"ph\":\"i\",\"s\":\"t\"";
		Output << ",\"args\":{\"tick\":" << Value.Tick << ",\"lane\":" << Value.Lane;
		const auto &Data = Value.Values;
		switch (Value.Type) {
			case Kind::Span:
				Output << ",\"exclusive_ns\":" << Data[1];
				break;
			case Kind::Phase:
				Output << ",\"inclusive_ns\":" << Data[0] << ",\"exclusive_ns\":" << Data[1] << ",\"calls\":" << Data[2]
					<< ",\"units\":" << Data[3] << ",\"allocations\":" << Data[4] << ",\"allocated_bytes\":" << Data[5]
					<< ",\"retained\":" << Data[6] << ",\"rejected\":" << Data[7];
				break;
			case Kind::Queue:
				Output << ",\"enqueued\":" << Data[0] << ",\"serviced\":" << Data[1] << ",\"maximum_depth\":" << Data[2]
					<< ",\"maximum_age_ns\":" << Data[3] << ",\"maximum_gap_ns\":" << Data[4]
					<< ",\"budget_deferrals\":" << Data[5] << ",\"capacity_deferrals\":" << Data[6]
					<< ",\"opportunities\":" << Data[7];
				break;
			case Kind::Counter:
				Output << ",\"value\":" << Data[0];
				break;
			case Kind::Frame:
				Output << ",\"interval_ns\":" << Data[0] << ",\"deadline_lateness_ns\":" << Data[1]
					<< ",\"deadline_missed\":" << (Data[2] ? "true" : "false");
				break;
		}
		Output << "}}";
	}
	Output << "\n],\"displayTimeUnit\":\"ns\",\"otherData\":{\"records\":" << Count << ",\"dropped\":" << Dropped()
		<< ",\"capacity\":" << Records.size() << ",\"buffer_bytes\":" << BufferBytes()
		<< ",\"stride\":" << Configuration.Stride << ",\"first_tick\":" << Configuration.FirstTick
		<< ",\"last_tick\":" << Configuration.LastTick << ",\"mode\":\""
		<< (Configuration.Detailed ? "detailed" : "summary") << "\"}}\n";
	Output.close();
}

HostWorkTrace::HostWorkTrace() {
	try {
		auto Collector = WorkTrace::FromEnvironment();
		if (Collector) Sample = std::make_unique<WorkSample>();
		Trace = std::move(Collector);
	} catch (const std::exception &Error) {
		std::cerr << "[Runtime:WorkTrace] " << Error.what() << '\n';
	}
}

void HostWorkTrace::Write() const {
	try {
		if (Trace) Trace->Write();
	} catch (const std::exception &Error) {
		std::cerr << "[Runtime:WorkTrace] " << Error.what() << '\n';
	}
}

HostWorkTrace::Frame::Frame(HostWorkTrace &Loop, Time Started, Time Previous, std::uint64_t PriorTick) noexcept
	: Tick(PriorTick + (PriorTick < std::numeric_limits<std::uint64_t>::max())) {
	if (!Loop.Trace || !Loop.Trace->SelectFrame(PriorTick, Tick)) return;
	Trace = Loop.Trace.get();
	Sample = Loop.Sample.get();
	*Sample = WorkSample{};
	Region.emplace(Trace, Tick, 0);
	Capture.emplace(Sample);
	StartedNanoseconds = Nanoseconds(Started.time_since_epoch());
	IntervalNanoseconds = Nanoseconds(Started - Previous);
}

HostWorkTrace::Frame::~Frame() {
	if (!Trace) return;
	const auto Now = std::chrono::steady_clock::now();
	const auto Late = Scheduled != Time{} && Now > Scheduled ? Nanoseconds(Now - Scheduled) : 0;
	Capture.reset();
	Region.reset();
	Trace->RecordFrame(Tick, 0, StartedNanoseconds, IntervalNanoseconds, Late, Missed);
	Trace->RecordSample(Tick, 0, Nanoseconds(Now.time_since_epoch()), *Sample);
}

void HostWorkTrace::Frame::SetDeadline(Time Deadline, Time BeforeWait) noexcept {
	if (!Trace) return;
	Scheduled = Deadline;
	Missed = BeforeWait > Deadline;
}

}
#endif
