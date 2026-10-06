#pragma once
#include "../cmake/gns/ServiceTimingDiagnostics.hpp"
#include <array>
#include <iostream>

// Supplied bounded sink only: no GNS initialization, transport or timer APIs.
inline bool TestGnsServiceTimingDiagnostics() {
	using namespace SteamNetworkingSocketsLib;
	if (GargantuanServiceTimingObserver.load(std::memory_order_acquire) != nullptr) return false;
	struct Storage {
		std::array<GargantuanServiceTimingRecord, 2> Records{};
		unsigned Count = 0;
		bool Overflow = false;
	} Values;
	auto Record = [](void *Context, const GargantuanServiceTimingRecord &Value) noexcept {
		auto &Destination = *static_cast<Storage *>(Context);
		if (Destination.Count == Destination.Records.size()) { Destination.Overflow = true; return; }
		Destination.Records[Destination.Count++] = Value;
	};
	GargantuanServiceTimingSink Sink{&Values, Record}, MissingCallback{&Values, nullptr}, MissingContext{nullptr, Record};
	bool Passed = !GargantuanSetServiceTimingSink(&MissingCallback) && !GargantuanSetServiceTimingSink(&MissingContext);
	{
		GargantuanServiceTimingSpan Disabled(GargantuanServiceTimingPhase::ReceiveDrain);
		Passed = Passed && !Disabled.Enabled();
		Disabled.AddCount(); Disabled.AddBytes(99); Disabled.Finish();
	}
	Passed = Passed && Values.Count == 0 && GargantuanSetServiceTimingSink(&Sink);
	Passed = Passed && !GargantuanSetServiceTimingSink(&Sink); // Never replace an existing owner.
	{
		GargantuanServiceTimingSpan Active(GargantuanServiceTimingPhase::ReceiveDrain);
		Passed = Passed && Active.Enabled();
		Active.AddCount(); Active.AddCount(); Active.AddBytes(10); Active.AddBytes(20);
		Active.SetDetail(7); Active.SetResult(-1); Active.SetError(5); Active.AddFlags(1); Active.AddFlags(2);
		Active.Finish(); Active.Finish(); // Explicit finish plus destructor must emit once.
	}
	const auto &First = Values.Records[0];
	Passed = Passed && Values.Count == 1 && First.Phase == GargantuanServiceTimingPhase::ReceiveDrain &&
		First.Count == 2 && First.Bytes == 30 && First.Detail == 7 && First.Result == -1 && First.Error == 5 && First.Flags == 3;
#if defined(_WIN32)
	Passed = Passed && First.ClockValid && First.ThreadId != 0 && First.BeginQpc != 0 && First.EndQpc >= First.BeginQpc;
#endif
	for (unsigned Index = 0; Index < 2; ++Index) {
		GargantuanServiceTimingSpan Active(GargantuanServiceTimingPhase::Thinkers);
		Active.AddCount(); Active.AddDetail();
	}
	Passed = Passed && Values.Count == 2 && Values.Overflow;
	Passed = GargantuanSetServiceTimingSink(nullptr) && Passed;
	{
		GargantuanServiceTimingSpan Disabled(GargantuanServiceTimingPhase::TimerWait);
		Passed = Passed && !Disabled.Enabled();
	}
	Passed = Passed && Values.Count == 2;
	Values.Count = 0; Values.Overflow = false;
	Passed = GargantuanSetServiceTimingSink(&Sink) && Passed;
	{
		GargantuanServiceTimingSpan Callback(GargantuanServiceTimingPhase::ThinkerCallback);
		GargantuanServiceTimingSpan Sender(GargantuanServiceTimingPhase::SnpSender);
		Passed = Passed && !Callback.Enabled() && !Sender.Enabled();
		GargantuanServiceTimingPacket(99, 9);
	}
	Passed = Passed && Values.Count == 0;
	GargantuanSetServiceTimingSink(nullptr);
	Sink.DetailedCallbacks = true;
	Passed = GargantuanSetServiceTimingSink(&Sink) && Passed;
	{
		GargantuanServiceTimingSpan Callback(GargantuanServiceTimingPhase::ThinkerCallback);
		Callback.SetCount(3); Callback.SetDetail(123); Callback.SetResult(456);
	}
	{
		GargantuanServiceTimingSpan Sender(GargantuanServiceTimingPhase::SnpSender);
		Sender.SetDetail(77); Sender.SetResult(99);
		GargantuanServiceTimingPacket(10, 99); GargantuanServiceTimingPacket(20, 99);
	}
	GargantuanServiceTimingPacket(999, 99); // Outside sender callback: ignored.
	Passed = Passed && Values.Count == 2 && Values.Records[0].Phase == GargantuanServiceTimingPhase::ThinkerCallback &&
		Values.Records[0].Count == 3 && Values.Records[0].Detail == 123 && Values.Records[0].Result == 456 &&
		Values.Records[1].Phase == GargantuanServiceTimingPhase::SnpSender && Values.Records[1].Count == 2 &&
		Values.Records[1].Bytes == 30 && Values.Records[1].Detail == 77 && Values.Records[1].Result == 99;
	Values.Count = 0;
	{
		GargantuanServiceTimingSpan Outer(GargantuanServiceTimingPhase::SnpSender);
		Outer.SetResult(1); GargantuanServiceTimingPacket(5, 1);
		{
			GargantuanServiceTimingSpan Inner(GargantuanServiceTimingPhase::SnpSender);
			Inner.SetResult(2); GargantuanServiceTimingPacket(7, 2);
			GargantuanServiceTimingPacket(999, 1); // Different connection cannot charge this inner visit.
		}
		GargantuanServiceTimingPacket(11, 1);
	}
	Passed = Passed && Values.Count == 2 && Values.Records[0].Bytes == 7 && Values.Records[0].Count == 1 &&
		Values.Records[1].Bytes == 16 && Values.Records[1].Count == 2 && GargantuanCurrentSnpTiming == nullptr;
	GargantuanSetServiceTimingSink(nullptr);
	std::cout << "[Qualification:GnsServiceTiming] cases=9 passed=" << Passed << '\n';
	return Passed;
}
