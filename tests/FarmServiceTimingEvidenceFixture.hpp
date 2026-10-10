#pragma once
#include "../src/host/server/FarmServiceTimingEvidence.hpp"
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

namespace gargantuan::host::detail {
// Supplied reservation reorder only: no production hook or timing change.
struct FarmServiceTimingEvidenceTestAccess {
	using History = FarmServiceTimingEvidence::History;
	static std::uint64_t Reserve(History &Value) { return Value.Attempts.fetch_add(1); }
	static void Publish(History &Value, std::uint64_t Sequence, const FarmServiceTimingEvidence::Record &Record) {
		Value.Publish(Sequence, Record);
	}
};
}

inline void TestFarmServiceTimingEvidence() {
	using Evidence = gargantuan::host::detail::FarmServiceTimingEvidence;
	using namespace SteamNetworkingSocketsLib;
	auto Check = [](bool Value, const char *Message) { if (!Value) throw std::runtime_error(Message); };
	Check(!GargantuanServiceTimingObserver.load(), "timing fixture begins with no borrowed sink");
	Check(!Evidence::EnabledValue("") && !Evidence::EnabledValue("0") && !Evidence::EnabledValue("true") &&
		!Evidence::EnabledValue("11") && Evidence::EnabledValue("1"), "only explicit exact timing opt-in accepted");
	{
		Evidence Disabled(false); Disabled.Freeze(99);
		std::ostringstream Out; Disabled.Write(Out, "disabled", true);
		Check(!Disabled.Enabled() && !GargantuanServiceTimingObserver.load() && Out.str().empty() && Disabled.Stop(),
			"disabled recorder has no storage, clocks, sink or output");
	}
	{
		auto History = std::make_unique<Evidence::History>();
		for (std::uint64_t I = 0; I < Evidence::Capacity + 2; ++I)
			History->Append({.Phase=GargantuanServiceTimingPhase::SnpSender, .ClockValid=true, .BeginQpc=I+1, .EndQpc=I+2, .Count=I});
		std::size_t Count = 0; std::uint64_t Smallest = UINT64_MAX;
		History->Visit([&](std::uint64_t Sequence, const Evidence::Record &Value) { ++Count; Smallest=std::min(Smallest, Sequence); Check(Value.Count+1==Sequence, "retained sequence matches raw record"); });
		Check(Count==Evidence::Capacity && Smallest==3 && History->OverwrittenCount()==2 && History->DroppedCount()==0,
			"wrap preserves bounded newest history and explicitly counts overwritten records");
	}
	{
		using Access = gargantuan::host::detail::FarmServiceTimingEvidenceTestAccess;
		auto History = std::make_unique<Evidence::History>();
		const auto Delayed = Access::Reserve(*History);
		for (std::uint64_t I=0; I<Evidence::Capacity; ++I)
			History->Append({.Phase=GargantuanServiceTimingPhase::SnpSender, .Count=I+1});
		Access::Publish(*History, Delayed, {.Phase=GargantuanServiceTimingPhase::SnpSender, .Count=0});
		bool FoundNewest=false, FoundOld=false;
		History->Visit([&](std::uint64_t Sequence, const Evidence::Record &) { FoundNewest|=Sequence==Evidence::Capacity+1; FoundOld|=Sequence==1; });
		Check(FoundNewest && !FoundOld && History->DroppedCount()==1 && History->OverwrittenCount()==0,
			"delayed pre-lock reservation cannot replace a newer wrap");
		const auto Race = Access::Reserve(*History); History->Freeze(100);
		Access::Publish(*History, Race, {.Phase=GargantuanServiceTimingPhase::Thinkers, .BeginQpc=101});
		Check(History->FilteredCount()==1, "freeze raced after reservation filters future begin under slot claim");
	}
	{
		auto History = std::make_unique<Evidence::History>();
		History->Append({.Phase=GargantuanServiceTimingPhase::Thinkers, .BeginQpc=101, .EndQpc=102});
		History->Freeze(100);
		History->Append({.Phase=GargantuanServiceTimingPhase::Thinkers, .BeginQpc=90, .EndQpc=120});
		History->Append({.Phase=GargantuanServiceTimingPhase::Thinkers, .BeginQpc=101, .EndQpc=130});
		std::size_t Count=0; History->Visit([&](std::uint64_t, const Evidence::Record &Value) { ++Count; Check(Value.EndQpc==120, "in-flight crossing span kept whole"); });
		Check(Count==1 && History->AttemptCount()==2, "freeze excludes already-published future begins without truncating in-flight span");
	}
	{
		auto History = std::make_unique<Evidence::History>(); std::vector<std::thread> Writers;
		for (unsigned Writer=0; Writer<4; ++Writer) Writers.emplace_back([&, Writer] {
			for (std::uint64_t I=0; I<Evidence::Capacity; ++I) History->Append({.Phase=GargantuanServiceTimingPhase::SnpSender,
				.ThreadId=Writer+1, .BeginQpc=I+1, .EndQpc=I+2, .Count=I, .Bytes=I+5});
		});
		for (auto &Writer : Writers) Writer.join();
		std::size_t Retained=0; History->Visit([&](std::uint64_t, const Evidence::Record &Value) { ++Retained; Check(Value.Bytes==Value.Count+5 && Value.EndQpc==Value.BeginQpc+1, "joined ring preserves whole concurrent records"); });
		Check(History->AttemptCount()==4*Evidence::Capacity && Retained<=Evidence::Capacity && !History->Overflowed() &&
			History->WrittenCount()+History->DroppedCount()+History->FilteredCount()==History->AttemptCount(),
			"concurrent wrapped writes finish without blocking and account every accepted attempt");
	}
	{
		Evidence Active(true); Check(Active.Enabled() && GargantuanServiceTimingObserver.load(), "opt-in installs owner");
		bool Rejected=false; try { Evidence Duplicate(true); } catch (const std::runtime_error &) { Rejected=true; }
		Check(Rejected, "existing sink owner cannot be replaced");
		{ GargantuanServiceTimingSpan Span(GargantuanServiceTimingPhase::ReceiveDrain); Span.AddCount(); }
		std::ostringstream BeforeJoin; Active.Write(BeforeJoin, "fixture", false);
		Check(BeforeJoin.str().find("retained=0")!=std::string::npos, "unjoined storage is never read");
		// No native transport exists in this supplied fixture; the span finished.
		Check(Active.Stop() && !GargantuanServiceTimingObserver.load(), "joined stop removes borrowed sink");
		std::ostringstream Out; Active.Write(Out, "fixture", true);
		Check(Out.str().find("attempts=1")!=std::string::npos && Out.str().find("attribution=NOT_MEASURED")!=std::string::npos,
			"raw dump does not manufacture causal attribution");
	}
	Check(!GargantuanServiceTimingObserver.load(), "fixture releases owner before return");
	std::cout << "[Qualification:FarmServiceTiming] fixture=PASS cases=7\n";
}
