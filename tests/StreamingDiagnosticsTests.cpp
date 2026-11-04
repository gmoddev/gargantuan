#include "gargantuan/Engine.hpp"
#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/content/ResidencyDemandPolicy.hpp"
#include "gargantuan/editor/EditorHost.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "editor/StreamingDiagnosticsEncoding.hpp"

#include <SDL3/SDL.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <thread>

using namespace gargantuan;

namespace {
	void Check(bool Value, const char *Message) { if (!Value) throw std::runtime_error(Message); }

	void PolicyInspection() {
		PackageContentManifest Manifest;
		Manifest.Package = {*ProjectId::Parse("0123456789abcdef0123456789abcdef"), 5};
		for (std::size_t Index = 0; Index < 200; ++Index) {
			PackageContentEntry Entry;
			Entry.Key = "unit/" + std::to_string(1000 + Index);
			Entry.Digest = *AssetContentId::Parse(std::string(64, 'a'));
			Entry.CoarseBounds = PackageCoarseBounds{{0, 0, 0}, {1, 1, 1}};
			Manifest.Entries.push_back(std::move(Entry));
		}
		ResidencyDemandPolicy Policy(Manifest);
		Check(!Policy.GetDiagnostics().Enabled && Policy.GetDiagnostics().Units.empty(), "diagnostics enabled implicitly");
		Policy.SetDiagnosticsEnabled(true);
		for (std::uint64_t Focus = 1; Focus <= 20; ++Focus) Check(Policy.UpdateFocus(Focus, {0, 0, 0}), "focus denied");
		for (std::uint64_t Tick = 1; Tick <= 200; ++Tick) Policy.Step(nullptr, Tick);
		const auto Before = Policy.GetMetrics();
		const auto Page = Policy.GetDiagnostics({0, 2, MaximumResidencyDiagnosticSamples});
		Check(Page.Enabled && Page.DesiredUnits == 200 && Page.OwnedRequests == 0 &&
			Page.FocusContributionRecords == 20 * 200,
			"stable focus contribution accounting differs from committed candidate count");
		Check(Page.Units.size() == 2 && Page.NextIndex == 2 && Page.Units[0].ManifestIndex == 0,
			"bounded diagnostic page order/cursor differs");
		Check(Page.Package == Manifest.Package && Page.Units[0].ContentDigest == Manifest.Entries[0].Digest &&
			Page.Units[0].PackageSpaceKey == "default" && Page.Units[0].CoarseBounds &&
			Page.Units[0].CoarseBounds->Minimum == Manifest.Entries[0].CoarseBounds->Minimum &&
			Page.Units[0].CoarseBounds->Maximum == Manifest.Entries[0].CoarseBounds->Maximum,
			"bounded package bounds inspection lost its manifest identity or extent");
		StreamingDiagnostics Snapshot;
		Snapshot.Policy = Page;
		const auto Wire = EncodeStreamingDiagnosticsSnapshot(Snapshot);
		Check(Wire["Policy"]["PackageNamespace"]["ProjectId"] == Manifest.Package.Project.ToString() &&
			Wire["Policy"]["PackageNamespace"]["PackageVersion"] == Manifest.Package.PackageVersion &&
			Wire["Policy"]["BoundsAccounting"] == "ManifestCoarseBoundsV1" &&
			Wire["Policy"]["Units"].size() == 2 &&
			Wire["Policy"]["Units"][0]["ContentDigest"] == Manifest.Entries[0].Digest.ToString() &&
			Wire["Policy"]["Units"][0]["PackageSpaceKey"] == "default" &&
			Wire["Policy"]["Units"][0]["CoarseBounds"]["Maximum"][0] == 1.0f,
			"wire bounds inspection is unbounded or differs from immutable manifest metadata");
		Check(Page.TimingSamples.size() == MaximumResidencyDiagnosticSamples && Page.TimingSamples.front().Tick == 73 &&
			Page.TimingSamples.back().Tick == 200, "fixed timing ring did not retire old samples");
		Check(Page.BudgetLimitedTicks != 0 && Page.PolicyStateLogicalBytes != 0 && Page.ReferencedManifestEntryBytes != 0,
			"budget/logical memory gauges missing");
		const auto Next = Policy.GetDiagnostics({*Page.NextIndex, 2, 0});
		Check(Next.Units.size() == 2 && Next.Units[0].ManifestIndex == 2 && Next.TimingSamples.empty(), "next page is not bounded");
		Policy.Step(nullptr, 200);
		Check(Policy.GetMetrics().QueryPages == Before.QueryPages && Policy.GetDiagnostics().Tick == 200,
			"inspection or duplicate tick advanced policy");
		bool Rejected = false;
		try { (void)Policy.GetDiagnostics({0, MaximumResidencyDiagnosticUnits + 1, 0}); }
		catch (const std::invalid_argument &) { Rejected = true; }
		Check(Rejected, "unbounded unit query accepted");
		Policy.SetDiagnosticsEnabled(false);
		Check(!Policy.GetDiagnostics().Enabled && Policy.GetDiagnostics().TimingSamples.empty(), "disable retained diagnostic history");
		Check(Policy.GetMetrics().DemandRecords == Before.DemandRecords, "diagnostic disable changed demand");
		Policy.SetDiagnosticsEnabled(true);
		Check(Policy.GetDiagnostics().TimingSamples.empty(), "re-enable resurrected old timing history");
		for (std::uint64_t Focus = 1; Focus <= 20; ++Focus) Check(Policy.RemoveFocus(Focus), "focus teardown failed");
		Check(Policy.GetDiagnostics().DesiredUnits == 0 && Policy.GetDiagnostics().Metrics.DemandRecords == 0,
			"retired desired counters leaked");
		Policy.Stop(nullptr);
		Check(Policy.GetDiagnostics().Stopped && Policy.GetDiagnostics().OwnedRequests == 0, "stop gauges leaked");
	}

	struct LocalPackageFixture final {
		std::filesystem::path Root = std::filesystem::temp_directory_path() /
			("gargantuan-streaming-engine-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		PackageContentManifest Manifest;
		AssetContentId ManifestDigest;
		LocalPackageFixture() {
			Check(std::filesystem::create_directories(Root / "content"), "could not create diagnostic package directory");
			Manifest.Package = {*ProjectId::Parse("0123456789abcdef0123456789abcdef"), 63};
			auto Object = std::make_shared<Part>();
			Object->SetName("DiagnosticStreamingUnit");
			Object->SetArchivable(true);
			Object->SetAnchored(true);
			Object->SetCanCollide(false);
			Object->SetSize({1, 1, 1});
			Object->SetCFrame(CFrame(0, 0, 0));
			std::shared_ptr<Instance> SerializableObject = Object;
			const auto Payload = InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, SerializableObject);
			const auto PayloadBytes = std::span(reinterpret_cast<const std::uint8_t *>(Payload.data()), Payload.size());
			PackageContentEntry Entry;
			Entry.Key = "unit/0";
			Entry.BlobReference = "content/unit.json";
			Entry.Digest = AssetContentId::Hash(PayloadBytes);
			Entry.CompressedBytes = Entry.UncompressedBytes = Payload.size();
			Entry.ObjectCount = 1;
			Entry.CoarseBounds = PackageCoarseBounds{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
			Manifest.Entries.push_back(std::move(Entry));
			{ std::ofstream Output(Root / "content/unit.json", std::ios::binary); Output << Payload;
				Check(Output.good(), "could not write diagnostic unit"); }
		Object->Destroy();
		const auto Encoded = EncodePackageContentManifest(Manifest);
		ManifestDigest = AssetContentId::Hash(std::span(
			reinterpret_cast<const std::uint8_t *>(Encoded.data()), Encoded.size()));
		{ std::ofstream Output(Root / "content/manifest.json", std::ios::binary); Output << Encoded;
			Check(Output.good(), "could not write diagnostic manifest"); }
		}
		~LocalPackageFixture() {
			std::error_code Ignored;
			std::filesystem::remove(Root / "content/unit.json", Ignored);
			std::filesystem::remove(Root / "content/manifest.json", Ignored);
			std::filesystem::remove(Root / "content", Ignored);
			std::filesystem::remove(Root, Ignored);
		}
		ContentAvailabilityConfiguration Configuration() const {
			ContentAvailabilityConfiguration Result;
			Result.Provider = std::make_shared<LocalPackageContentProvider>(Root, "content/manifest.json", ManifestDigest);
			Result.Package = Manifest.Package;
			Result.ManifestDigest = ManifestDigest;
			Result.Mode = ContentResidencyMode::OnDemand;
			return Result;
		}
	};

	void EngineInspection() {
		LocalPackageFixture Fixture;
		auto World = std::make_shared<DataModel>();
		HeadlessRenderer Renderer(Vector2(32, 32));
		Engine Runtime(World, &Renderer, {}, {.Content = Fixture.Configuration(), .Mode = RuntimeMode::NetworkServer,
			.EnableResidencyDemandPolicy = true});
		Runtime.ProcessService->Alive = true;
		const auto InitiallyDisabled = Runtime.GetStreamingDiagnostics();
		Check(InitiallyDisabled.Status == StreamingDiagnosticStatus::Disabled && !InitiallyDisabled.Enabled &&
			InitiallyDisabled.Policy.Units.empty(), "Engine enabled diagnostic collection implicitly");
		Runtime.SetStreamingDiagnosticsEnabled(true);
		Check(Runtime.GetStreamingDiagnostics().Enabled, "Engine diagnostic opt-in failed");
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while ((!Runtime.Content->IsManifestAvailable() || !Runtime.GetResidencyDemandPolicy()) &&
			std::chrono::steady_clock::now() < Deadline) {
			Runtime.Step();
			std::this_thread::yield();
		}
		Check(Runtime.Content->IsManifestAvailable() && Runtime.GetResidencyDemandPolicy(),
			"real package policy did not prepare");
		const auto Snapshot = Runtime.Content->GetManifestSnapshot();
		Check(Snapshot && Snapshot->Entries.size() == 1, "Engine and CAS did not expose validated package snapshot");
		Check(Runtime.GetResidencyDemandPolicy()->SetRequirement("unit/0", ResidencyDemandClass::Critical),
			"trusted diagnostic demand failed");
		const auto AdmissionDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (Runtime.Content->GetState("unit/0") != ContentResidencyState::Resident &&
			std::chrono::steady_clock::now() < AdmissionDeadline) {
			Runtime.Step();
			std::this_thread::yield();
		}
		Check(Runtime.Content->GetState("unit/0") == ContentResidencyState::Resident,
			"real package content did not admit");
		const auto Active = Runtime.GetStreamingDiagnostics({0, 1, 4});
		const auto Cas = Runtime.Content->GetDiagnosticCounters();
		Check(Active.Status == StreamingDiagnosticStatus::Active && Active.ManifestAvailable &&
			Active.ContentConfigured && Active.Policy.Enabled && Active.Policy.ManifestUnits == 1 &&
			Active.Policy.OwnedRequests == 1 && Active.Policy.Units.size() == 1,
			"active Engine diagnostic projection omitted real residency demand");
		Check(Active.Policy.Units.front().Demand.Key == "unit/0" &&
			Active.Policy.Units.front().AvailabilityState == ContentResidencyState::Resident &&
			Active.Content.Admissions == Cas.Admissions && Active.Content.Acquisitions == Cas.Acquisitions &&
			Active.Content.Requests == Cas.Requests && Active.Policy.Metrics.ServiceBindingFailures == 0 &&
			Active.Policy.Metrics.ManifestBindingEntriesCompared == 0,
			"Engine diagnostic row and counters differ from the bound CAS snapshot");
		bool Rejected = false;
		try { (void)Runtime.GetStreamingDiagnostics({0, MaximumResidencyDiagnosticUnits + 1, 0}); }
		catch (const std::invalid_argument &) { Rejected = true; }
		Check(Rejected, "Engine accepted an unbounded diagnostic query");
		Runtime.SetStreamingDiagnosticsEnabled(false);
		Check(Runtime.GetStreamingDiagnostics().Status == StreamingDiagnosticStatus::Disabled &&
			Runtime.GetStreamingDiagnostics().Policy.Units.empty(), "Engine diagnostic disable retained rows");
		Runtime.SetStreamingDiagnosticsEnabled(true);
		Check(Runtime.GetStreamingDiagnostics({0, 1, 0}).Policy.Units.size() == 1,
			"Engine diagnostic re-enable did not restore current bounded projection");
		Runtime.Content->Stop();
		const auto Stopped = Runtime.GetStreamingDiagnostics({0, 1, 4});
		Check(Stopped.Status == StreamingDiagnosticStatus::Stopped && !Stopped.ManifestAvailable &&
			Stopped.Policy.Units.empty() && Stopped.Content.ReservedCompletionBytes == 0 &&
			Stopped.Content.DecodedDocumentBytes == 0,
			"direct CAS stop exposed stale Engine policy rows or retained preparation bytes");
		Runtime.Step();
		Check(!Runtime.GetResidencyDemandPolicy() && Runtime.GetStreamingDiagnostics().Status == StreamingDiagnosticStatus::Stopped,
			"Engine Step republished stopped CAS residency policy");
		Runtime.Destroy();
		Check(Runtime.GetStreamingDiagnostics().Status == StreamingDiagnosticStatus::Stopped,
			"Engine destroy revived diagnostic state");
		World->Destroy();
	}

	void EngineStopBeforePreparationInspection() {
		LocalPackageFixture Fixture;
		auto World = std::make_shared<DataModel>();
		HeadlessRenderer Renderer(Vector2(32, 32));
		Engine Runtime(World, &Renderer, {}, {.Content = Fixture.Configuration(), .Mode = RuntimeMode::NetworkServer,
			.EnableResidencyDemandPolicy = true, .EnableStreamingDiagnostics = true});
		Runtime.ProcessService->Alive = true;
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!Runtime.Content->IsManifestAvailable() && std::chrono::steady_clock::now() < Deadline) {
			Runtime.Content->Step();
			std::this_thread::yield();
		}
		Check(Runtime.Content->IsManifestAvailable() && !Runtime.GetResidencyDemandPolicy(),
			"manifest-only Engine fixture unexpectedly prepared a policy");
		Check(Runtime.GetStreamingDiagnostics().Status == StreamingDiagnosticStatus::AwaitingManifest,
			"Engine misreported manifest-only preparation stage");
		Runtime.Content->Stop();
		const auto Stopped = Runtime.GetStreamingDiagnostics({0, 1, 1});
		Check(Stopped.Status == StreamingDiagnosticStatus::Stopped && !Stopped.ManifestAvailable &&
			Stopped.Policy.Units.empty() && Stopped.Content.Stopped,
			"direct Content stop before Engine Step exposed stale preparation diagnostics");
		Runtime.Step();
		Check(!Runtime.GetResidencyDemandPolicy() && Runtime.GetStreamingDiagnostics().Status == StreamingDiagnosticStatus::Stopped,
			"Engine prepared a policy after direct Content stop");
		Runtime.Destroy();
		World->Destroy();
	}

	void EditorInspection() {
		using Json = nlohmann::ordered_json;
		const auto Root = std::filesystem::temp_directory_path() /
			("gargantuan-streaming-diagnostics-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		struct Cleanup {
			std::filesystem::path Root;
			~Cleanup() { std::error_code Ignored; std::filesystem::remove_all(Root, Ignored); }
		} CleanupValue{Root};
		EditorHost Host("streaming-diagnostic-test");
		std::size_t Sequence = 0;
		const auto Call = [&](std::string Method, Json Parameters, std::string Token = "streaming-diagnostic-test") {
			return Json::parse(Host.HandleRequest(Json{{"Version", 1}, {"RequestId", std::to_string(++Sequence)},
				{"SessionToken", Token}, {"Method", Method}, {"Params", std::move(Parameters)}}.dump()));
		};
		const auto Handshake = Call("Handshake", Json::object());
		Check(Handshake["Ok"] && Handshake["Result"]["StreamingDiagnosticsVersion"] == 1, "diagnostic capability missing");
		Check(Call("CreateProject", {{"Destination", Root.generic_string()}, {"Name", "Streaming Diagnostics"}})["Ok"], "project create failed");
		const auto Revision = Call("GetProjectState", Json::object())["Result"]["AuthoritativeRevision"];
		const auto Start = Call("StartPlaySession", Json::object());
		Check(Start["Ok"], "Play start failed");
		const auto Id = Start["Result"]["PlaySessionId"];
		const auto Disabled = Call("GetStreamingDiagnostics", {{"PlaySessionId", Id}});
		Check(Disabled["Ok"] && !Disabled["Result"]["Diagnostics"]["Enabled"], "Play collection enabled implicitly");
		const auto Enabled = Call("ConfigureStreamingDiagnostics", {{"PlaySessionId", Id}, {"Enabled", true}});
		Check(Enabled["Ok"] && Enabled["Result"]["Diagnostics"]["Enabled"] &&
			Enabled["Result"]["Diagnostics"]["Status"] == "NotConfigured", "Play invented a streaming provider/policy");
		Check(Enabled["Result"]["Diagnostics"]["Policy"]["TimingAccounting"] == "CorePhasesExcludeObservation",
			"Play response did not identify timing observation scope");
		Check(!Call("GetStreamingDiagnostics", {{"PlaySessionId", Id}, {"MaximumUnits", 129}})["Ok"], "IPC unit cap bypass");
		Check(!Call("GetStreamingDiagnostics", {{"PlaySessionId", Id}, {"MaximumTimingSamples", -1}})["Ok"], "IPC negative cap accepted");
		Check(!Call("GetStreamingDiagnostics", {{"PlaySessionId", Id}, {"RequestContent", "unit/a"}})["Ok"], "IPC demand field accepted");
		Check(!Call("GetStreamingDiagnostics", {{"PlaySessionId", Id}}, "bad-token")["Ok"], "wrong session token read diagnostics");
		const auto Snapshot = Call("GetStreamingDiagnostics", {{"PlaySessionId", Id}, {"MaximumUnits", 0}, {"MaximumTimingSamples", 0}});
		Check(Snapshot["Ok"] && Snapshot.dump().size() < 16384, "aggregate diagnostic response not bounded");
		Check(Call("GetProjectState", Json::object())["Result"]["AuthoritativeRevision"] == Revision,
			"diagnostic collection mutated authoring history");
		Check(Call("StopPlaySession", {{"PlaySessionId", Id}})["Ok"], "Play stop failed");
		Check(!Call("GetStreamingDiagnostics", {{"PlaySessionId", Id}})["Ok"], "stale Play diagnostics accepted");
	}
}

int main() {
	try {
		Check(SDL_Init(0), "SDL init failed");
		BootstrapNativeRuntimeSchema();
		PolicyInspection();
		EngineInspection();
		EngineStopBeforePreparationInspection();
		EditorInspection();
		SDL_Quit();
		std::cout << "[Content:StreamingDiagnostics] bounded inspection, opt-in and authority tests passed\n";
	} catch (const std::exception &Error) {
		SDL_Quit();
		std::cerr << "[Content:StreamingDiagnostics] " << Error.what() << '\n';
		return 1;
	} catch (...) {
		SDL_Quit();
		std::cerr << "[Content:StreamingDiagnostics] unknown failure\n";
		return 1;
	}
}
