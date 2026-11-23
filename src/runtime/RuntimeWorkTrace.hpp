#pragma once

#if defined(GARGANTUAN_WORK_TRACE)

#include "runtime/RuntimeWorkDiagnostics.hpp"

#include <atomic>
#include <bitset>
#include <filesystem>
#include <memory>
#include <vector>

namespace gargantuan::runtime_detail {

// A local, one-process trace. Producers may record on separate threads. All
// regions must be closed and other writers joined before exporting or retiring it.
class WorkTrace final {
public:
	static constexpr std::size_t MaximumRecords = 262'144;

	struct Options {
		std::filesystem::path Path;
		std::size_t Capacity = 65'536;
		std::uint64_t Stride = 8;
		std::uint64_t FirstTick = 1;
		std::uint64_t LastTick = std::numeric_limits<std::uint64_t>::max();
		bool Detailed = false;
		std::bitset<WorkPhaseNames.size()> Phases = std::bitset<WorkPhaseNames.size()>{}.set();
	};

	explicit WorkTrace(Options Configuration);
	static std::unique_ptr<WorkTrace> FromEnvironment();

	class Region final {
	public:
		Region(WorkTrace *Trace, std::uint64_t Tick, std::uint32_t Lane) noexcept;
		~Region();
		Region(const Region &) = delete;
		Region &operator=(const Region &) = delete;
		static void SetCurrentLane(std::uint32_t Lane) noexcept;

	private:
		WorkTrace *Owner;
		WorkSpanSink Sink;
		WorkSpanSink *Previous;
		std::uint64_t TickId;
		std::uint32_t LaneId;
		std::uint32_t ThreadId;

		static void RecordSpan(void *Context, WorkPhase Phase, std::uint64_t Started,
			std::uint64_t Inclusive, std::uint64_t Exclusive) noexcept;
	};

	[[nodiscard]] bool SelectFrame(std::uint64_t Frame, std::uint64_t Tick) const noexcept {
		return Tick >= Configuration.FirstTick && Tick <= Configuration.LastTick && Frame % Configuration.Stride == 0;
	}
	void RecordSample(std::uint64_t Tick, std::uint32_t Lane, std::uint64_t At, const WorkSample &Sample) noexcept;
	void RecordFrame(std::uint64_t Tick, std::uint32_t Lane, std::uint64_t Started, std::uint64_t Interval,
		std::uint64_t DeadlineLateness = 0, bool DeadlineMissed = false) noexcept;
	void Write() const;
	[[nodiscard]] std::size_t Dropped() const noexcept;
	[[nodiscard]] std::size_t BufferBytes() const noexcept;

private:
	enum class Kind { Span, Phase, Queue, Counter, Frame };
	struct Record {
		Kind Type;
		std::size_t Key;
		std::uint32_t Thread, Lane;
		std::uint64_t Tick, At;
		std::array<std::uint64_t, 8> Values;
	};
	const Options Configuration;
	const std::uint64_t Origin = WorkTimestamp();
	std::vector<Record> Records;
	std::atomic<std::size_t> Used{0};

	void Append(Record Value) noexcept;
	[[nodiscard]] bool Includes(std::size_t Phase) const noexcept { return Configuration.Phases[Phase]; }
};

}
#endif
