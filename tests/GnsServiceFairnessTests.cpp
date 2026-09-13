#include <steam/steamnetworkingsockets.h>
#include <steamnetworkingsockets_thinker.h>
#include <chrono>
#include <cstdio>
#include <thread>

// Private exported test controls in this exact pinned socketthread.cpp. They
// keep dispatch on this test thread; production continues using its GNS thread.
STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_SetManualPollMode(bool);
STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_Poll(int);

// Deliberately tests the pinned dependency's private timer contract. This target
// links no Gargantuan runtime and follows GNS's platform RTTI settings.
namespace {
class RepeatingTimer final : public SteamNetworkingSocketsLib::IThinker {
public:
	int Calls = 0;
	SteamNetworkingMicroseconds LastTimestamp = 0;
	bool Monotonic = true;
private:
	void Think(SteamNetworkingMicroseconds Now) override {
		Monotonic = Monotonic && Now >= LastTimestamp;
		LastTimestamp = Now;
		++Calls;
		if (Calls < 3) SetNextThinkTime(Now + 1000);
		// A rescheduled timer becomes due during its own callback. The next
		// socket-service pass must run before dispatching it again.
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
};
}

int main() {
	SteamNetworkingSockets_SetManualPollMode(true);
	SteamNetworkingErrMsg Error{};
	if (!GameNetworkingSockets_Init(nullptr, Error)) {
		std::fprintf(stderr, "[Gns:Fairness] initialization failed: %s\n", Error);
		return 1;
	}
	bool Passed = true;
	{
		RepeatingTimer First, Second;
		First.SetNextThinkTimeASAP();
		Second.SetNextThinkTimeASAP();
		for (int Pass = 1; Pass <= 3; ++Pass) {
			SteamNetworkingSockets_Poll(0);
			Passed = Passed && First.Calls == Pass && Second.Calls == Pass;
			Passed = Passed && First.Monotonic && Second.Monotonic;
			const auto Gap = First.LastTimestamp > Second.LastTimestamp
				? First.LastTimestamp - Second.LastTimestamp : Second.LastTimestamp - First.LastTimestamp;
			Passed = Passed && Gap >= 1000;
		}
		First.ClearNextThinkTime();
		Second.ClearNextThinkTime();
		SteamNetworkingSockets_Poll(0);
		Passed = Passed && First.Calls == 3 && Second.Calls == 3;
	}
	GameNetworkingSockets_Kill();
	SteamNetworkingSockets_SetManualPollMode(false);
	std::fprintf(stderr, "[Gns:Fairness] %s\n", Passed ? "passed" : "failed");
	return Passed ? 0 : 1;
}
