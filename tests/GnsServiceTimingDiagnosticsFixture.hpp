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
	std::cout << "[Qualification:GnsServiceTiming] cases=6 passed=" << Passed << '\n';
	return Passed;
}
