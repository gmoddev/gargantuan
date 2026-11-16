#include "host/server/ServerHost.hpp"

#include <atomic>
#include <algorithm>
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
				Evidence.ContentManifestDigest.IsValid() && Evidence.Endpoint.IsValid() &&
				std::any_of(Evidence.ServerPublicKey.begin(), Evidence.ServerPublicKey.end(),
					[](std::byte Value) { return Value != std::byte{}; }));
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
		Case != "ListenerUnavailable" &&
		Case != "PreReady" && Case != "MissingProvider" && Case != "MissingLifecycle" &&
		Case != "InsecureOverride") return 2;
	std::vector<char *> HostArguments{Arguments[0]};
	for (int Index = 2; Index < ArgumentCount; ++Index) HostArguments.push_back(Arguments[Index]);
	const auto Observer = std::make_shared<Lifecycle>();
	gargantuan::host::ServerHostConfiguration Configuration;
	Configuration.ManagedLifecycle = Case == "MissingLifecycle" ? nullptr : Observer;
	Configuration.AdmissionProvider = Case == "MissingProvider" ? nullptr : std::make_shared<DenyingProvider>();
	const auto StartupBudget = Configuration.ManagedStartupTimeout;
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
	const auto Started = std::chrono::steady_clock::now();
	const int Result = gargantuan::host::RunDedicatedServer(
		static_cast<int>(HostArguments.size()), HostArguments.data(), std::move(Configuration));
	const auto Elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - Started);
	Worker.request_stop();
	Worker.join();
	const bool RejectedBeforeListening = Case == "MissingProvider" || Case == "MissingLifecycle" ||
		Case == "InsecureOverride";
	const bool ListenerUnavailable = Case == "ListenerUnavailable";
	const bool HasListener = Case != "PreReady" && !RejectedBeforeListening && !ListenerUnavailable;
	const bool Expected = Result == ((Case == "ReadyDrain") ? 0 :
		RejectedBeforeListening ? 2 : 7) &&
		Observer->ListeningCount.load() == (HasListener ? 1 : 0) &&
		Observer->DrainingCount.load() == (RejectedBeforeListening || ListenerUnavailable ? 0 : 1) &&
		Observer->StoppedCount.load() == (RejectedBeforeListening || ListenerUnavailable ? 0 : 1) &&
		(!HasListener || Observer->EvidenceValid.load()) &&
		(RejectedBeforeListening || ListenerUnavailable || Observer->DrainedBeforeStop.load()) &&
		(Case != "ReadyDrain" || Observer->DrainRequested.load()) &&
		(Case != "AuthorityLoss" || !Observer->AuthorityCurrent.load()) &&
		(!ListenerUnavailable ||
			(!Observer->EvidenceValid.load() && !Observer->DrainedBeforeStop.load() &&
				!Observer->ReadyAcknowledged.load())) &&
		(Case != "NeverReady" ||
			(Elapsed >= StartupBudget && !Observer->ReadyAcknowledged.load()));
	std::cout << "[Runtime:ManagedServerTest] case=" << Case
		<< " result=" << Result
		<< " listening=" << Observer->ListeningCount.load()
		<< " draining=" << Observer->DrainingCount.load()
		<< " stopped=" << Observer->StoppedCount.load()
		<< " evidence=" << Observer->EvidenceValid.load()
		<< " drained_before_stop=" << Observer->DrainedBeforeStop.load()
		<< " ready_acknowledged=" << Observer->ReadyAcknowledged.load()
		<< " elapsed_ms=" << Elapsed.count()
		<< " startup_budget_ms=" << StartupBudget.count()
		<< " expected=" << Expected << '\n';
	if (!Expected) {
		std::cerr << "[Runtime:ManagedServerTest] lifecycle case failed\n";
		return 1;
	}
	std::cout << "[Runtime:ManagedServerTest] lifecycle case passed\n";
	return 0;
}
