#include "host/server/ServerHost.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#endif

using namespace std::chrono_literals;

namespace {
	class DenyingProvider final : public gargantuan::network::IGameAdmissionProvider {
	  public:
		bool Begin(gargantuan::network::GameAdmissionRequest,
			gargantuan::network::GameAdmissionCompletion) override { return false; }
		void Release(std::uint64_t) noexcept override {}
	};

	class Lifecycle final : public gargantuan::host::IManagedServerLifecycle {
	  public:
		std::atomic<bool> AuthorityCurrent{true};
		std::atomic<bool> ReadyAcknowledged{false};
		std::atomic<bool> DrainRequested{false};
		std::atomic<int> ListeningCount{0};
		std::atomic<int> DrainingCount{0};
		std::atomic<int> StoppedCount{0};
		std::atomic<bool> EvidenceValid{false};
		std::atomic<bool> DrainedBeforeStop{false};

		gargantuan::host::ManagedServerSnapshot Snapshot() const noexcept override {
			return {
				.AuthorityCurrent = AuthorityCurrent.load(),
				.ReadyAcknowledged = ReadyAcknowledged.load(),
				.DrainRequested = DrainRequested.load(),
			};
		}
		void OnListening(gargantuan::host::ServerListeningEvidence Evidence) noexcept override {
			EvidenceValid.store(Evidence.Package.Project.IsValid() &&
				Evidence.ContentManifestDigest.IsValid() && Evidence.Endpoint.IsValid());
			ListeningCount.fetch_add(1);
		}
		void OnDraining() noexcept override { DrainingCount.fetch_add(1); }
		void OnStopped() noexcept override {
			DrainedBeforeStop.store(DrainingCount.load() == 1);
			StoppedCount.fetch_add(1);
		}
	};
}

int main(int ArgumentCount, char *Arguments[]) {
#if defined(_WIN32)
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
	if (ArgumentCount < 4) return 2;
	const std::string_view Case = Arguments[1];
	if (Case != "ReadyDrain" && Case != "AuthorityLoss" && Case != "NeverReady" &&
		Case != "PreReady" && Case != "MissingProvider" && Case != "MissingLifecycle") return 2;
	std::vector<char *> HostArguments{Arguments[0]};
	for (int Index = 2; Index < ArgumentCount; ++Index) HostArguments.push_back(Arguments[Index]);
	const auto Observer = std::make_shared<Lifecycle>();
	gargantuan::host::ServerHostConfiguration Configuration;
	Configuration.ManagedLifecycle = Case == "MissingLifecycle" ? nullptr : Observer;
	Configuration.AdmissionProvider = Case == "MissingProvider" ? nullptr : std::make_shared<DenyingProvider>();
	Configuration.ManagedStartupTimeout = Case == "NeverReady" ? 150ms : 6s;
	if (Case == "PreReady") Observer->ReadyAcknowledged.store(true);
	std::jthread Worker([&](std::stop_token Stop) {
		while (!Stop.stop_requested() && Observer->ListeningCount.load() == 0)
			std::this_thread::sleep_for(1ms);
		if (Stop.stop_requested()) return;
		if (Case == "ReadyDrain" || Case == "AuthorityLoss") {
			std::this_thread::sleep_for(20ms);
			Observer->ReadyAcknowledged.store(true);
			std::this_thread::sleep_for(1s);
			if (Case == "ReadyDrain") Observer->DrainRequested.store(true);
			else Observer->AuthorityCurrent.store(false);
		}
	});
	const int Result = gargantuan::host::RunDedicatedServer(
		static_cast<int>(HostArguments.size()), HostArguments.data(), std::move(Configuration));
	Worker.request_stop();
	Worker.join();
	const bool HasListener = Case != "PreReady" && Case != "MissingProvider" && Case != "MissingLifecycle";
	const bool Expected = Result == ((Case == "ReadyDrain") ? 0 :
		(Case == "MissingProvider" || Case == "MissingLifecycle") ? 2 : 7) &&
		Observer->ListeningCount.load() == (HasListener ? 1 : 0) &&
		Observer->DrainingCount.load() == ((Case == "MissingProvider" || Case == "MissingLifecycle") ? 0 : 1) &&
		Observer->StoppedCount.load() == ((Case == "MissingProvider" || Case == "MissingLifecycle") ? 0 : 1) &&
		(!HasListener || Observer->EvidenceValid.load()) &&
		((Case == "MissingProvider" || Case == "MissingLifecycle") || Observer->DrainedBeforeStop.load()) &&
		(Case != "ReadyDrain" || Observer->DrainRequested.load()) &&
		(Case != "AuthorityLoss" || !Observer->AuthorityCurrent.load());
	if (!Expected) {
		std::cerr << "[Runtime:ManagedServerTest] lifecycle case failed\n";
		return 1;
	}
	std::cout << "[Runtime:ManagedServerTest] lifecycle case passed\n";
	return 0;
}
