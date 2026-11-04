#include "gargantuan/Engine.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/content/ContentAvailability.hpp"
#include "gargantuan/content/ResidencyDemandPolicy.hpp"
#include "gargantuan/content/ResidencySpatialMetadata.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/packaging/PackageBuilder.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/Renderer.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
	using namespace gargantuan;

	void Check(bool Condition, const char *Message) {
		if (!Condition) throw std::runtime_error(Message);
	}

	std::string ReadBytes(const std::filesystem::path &Path) {
		std::ifstream Input(Path, std::ios::binary);
		Check(Input.good(), "package manifest could not be opened");
		return {std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>()};
	}

	AssetContentId DigestOf(std::string_view Bytes) {
		return AssetContentId::Hash(std::span(
			reinterpret_cast<const std::uint8_t *>(Bytes.data()), Bytes.size()));
	}

	struct PackageFixture final {
		std::filesystem::path Root = std::filesystem::temp_directory_path() /
			("gargantuan-cross-consumer-" +
				std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		std::filesystem::path ProjectRoot = Root / "Project";
		std::filesystem::path PackageRoot = Root / "Package";
		PackageContentManifest Manifest;
		std::string ManifestBytes;
		AssetContentId ManifestDigest;
		std::array<std::string, 3> CandidateKeys;
		std::array<AssetContentId, 3> CandidateDigests;

		PackageFixture() {
			std::filesystem::create_directory(Root);
			std::filesystem::copy(std::filesystem::path(GARGANTUAN_FIRST_COMPLETE_GAME_ROOT), ProjectRoot,
				std::filesystem::copy_options::recursive);
			BootstrapProjectRuntimeSchema(ProjectRoot);
			DiskFilesystem Filesystem(ProjectRoot);
			auto ProjectValue = Project::fromExisting(&Filesystem);
			auto World = ProjectValue.DeserializeGame();
			World->InitializeLoadedProjectRevision();
			World->Root = ProjectRoot;
			World->Filesystem = &Filesystem;
			auto Space = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
			Check(Space != nullptr, "production project has no Workspace");
			for (int Index = 0; Index < 3; ++Index) {
				auto PartValue = std::make_shared<Part>();
				PartValue->SetArchivable(true);
				PartValue->SetName("CrossConsumerCandidate" + std::to_string(Index));
				PartValue->SetAnchored(true);
				PartValue->SetSize({2, 2, 2});
				PartValue->SetCFrame(CFrame(0, 0, 0));
				PartValue->SetParent(Space);
			}
			const auto Revision = World->GetAuthoritativeRevision();
			auto Payload = PackageBuilder::Capture(ProjectValue, World, Revision, Revision);
			std::array<std::size_t, 3> CandidateCounts{};
			for (const auto &Unit : Payload.ContentUnits) {
				const auto Document = nlohmann::json::parse(Unit.Payload);
				for (std::size_t Index = 0; Index < CandidateCounts.size(); ++Index) {
					if (Document.value("Name", "") != "CrossConsumerCandidate" + std::to_string(Index)) continue;
					Check(Document.value("ClassName", "") == "Part" && Unit.Entry.CoarseBounds.has_value(),
						"named candidate did not become a spatial Part unit");
					++CandidateCounts[Index];
					CandidateKeys[Index] = Unit.Entry.Key;
					CandidateDigests[Index] = Unit.Entry.Digest;
				}
			}
			Check(std::ranges::all_of(CandidateCounts, [](std::size_t Count) { return Count == 1; }),
				"production capture did not partition each named Part exactly once");
			const auto Build = PackageBuilder::Build({
				.Payload = std::move(Payload),
				.RuntimeDistributionRoot = std::filesystem::path(GARGANTUAN_RUNTIME_DISTRIBUTION_ROOT),
				.OutputDirectory = PackageRoot,
				.Configuration = PackageConfiguration::Release,
			});
			Check(Build.Ok, "production package build failed");
			Check(std::ranges::none_of(PackageBuilder::Validate(PackageRoot), [](const PackageDiagnostic &Value) {
				return Value.Severity == PackageDiagnosticSeverity::Error;
			}), "production package validation failed");
			ManifestBytes = ReadBytes(PackageRoot / "content/content.manifest.json");
			ManifestDigest = DigestOf(ManifestBytes);
			auto Parsed = ParsePackageContentManifest(ManifestBytes);
			Check(Parsed.has_value(), "package-produced manifest bytes did not parse");
			Manifest = std::move(*Parsed);
			Check(Manifest.Package.Project == ProjectValue.Identity && Manifest.Package.PackageVersion == Revision,
				"package-produced namespace changed");
			for (std::size_t Index = 0; Index < CandidateKeys.size(); ++Index) {
				const auto Entry = std::ranges::find_if(Manifest.Entries, [&](const PackageContentEntry &Value) {
					return Value.Key == CandidateKeys[Index];
				});
				Check(Entry != Manifest.Entries.end() && Entry->Digest == CandidateDigests[Index] &&
					Entry->CoarseBounds.has_value(), "named candidate identity was absent from emitted manifest bytes");
			}
			World->Destroy();
		}

		~PackageFixture() {
			std::error_code Ignored;
			std::filesystem::remove_all(Root, Ignored);
		}

		ContentAvailabilityConfiguration Configuration() const {
			ContentAvailabilityConfiguration Result;
			Result.Provider = std::make_shared<LocalPackageContentProvider>(
				PackageRoot, "content/content.manifest.json", ManifestDigest);
			Result.Package = Manifest.Package;
			Result.ManifestDigest = ManifestDigest;
			Result.Mode = ContentResidencyMode::OnDemand;
			return Result;
		}
	};

	struct ManifestProvider final : IContentAvailabilityProvider {
		PackageContentNamespace Package;
		std::string Bytes;

		ManifestProvider(PackageContentNamespace PackageValue, std::string BytesValue)
			: Package(PackageValue), Bytes(std::move(BytesValue)) {}
		std::string_view Name() const override { return "cross-consumer-manifest"; }
		ContentManifestProviderResult GetManifest(const ContentRequestContext &,
			const PackageContentNamespace &Requested) override {
			if (Requested != Package)
				return std::unexpected(ContentProviderError{ContentProviderErrorCode::NotFound, "namespace mismatch"});
			return Bytes;
		}
		ContentPayloadProviderResult GetContent(const ContentRequestContext &,
			const PackageContentIdentity &) override {
			return std::unexpected(ContentProviderError{ContentProviderErrorCode::Unavailable, "no payload"});
		}
	};

	struct ServiceFixture final {
		std::shared_ptr<DataModel> World = std::make_shared<DataModel>();
		std::shared_ptr<Workspace> Space =
			std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
		std::unique_ptr<ContentAvailabilityService> Service;

		explicit ServiceFixture(ContentAvailabilityConfiguration Configuration) {
			Service = std::make_unique<ContentAvailabilityService>(World, Space, std::move(Configuration));
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (!Service->IsManifestAvailable() && std::chrono::steady_clock::now() < Deadline) {
				Service->Step();
				std::this_thread::yield();
			}
			Check(Service->IsManifestAvailable(), "validated manifest was not published");
		}
		~ServiceFixture() {
			if (Service) Service->Stop();
			if (World) World->Destroy();
		}
	};

	ContentAvailabilityConfiguration ManifestConfiguration(const PackageContentManifest &Manifest) {
		const auto Bytes = EncodePackageContentManifest(Manifest);
		Check(ParsePackageContentManifest(Bytes).has_value(), "replacement manifest failed 3L validation");
		ContentAvailabilityConfiguration Result;
		Result.Provider = std::make_shared<ManifestProvider>(Manifest.Package, Bytes);
		Result.Package = Manifest.Package;
		Result.ManifestDigest = DigestOf(Bytes);
		Result.Mode = ContentResidencyMode::OnDemand;
		return Result;
	}

	void CheckIssue(const ResidencyMetadataReport &Report, ResidencyMetadataStatus Status,
		ResidencyMetadataIssueCode Code, const char *Message) {
		Check(Report.Status == Status && std::ranges::any_of(Report.Issues,
			[&](const ResidencyMetadataIssue &Value) { return Value.Code == Code; }), Message);
	}

	void MetadataAndCandidates(const PackageFixture &Package) {
		ResidencySpatialMetadata Proof;
		Proof.Package = Package.Manifest.Package;
		for (const auto &Entry : Package.Manifest.Entries)
			if (Entry.CoarseBounds)
				Proof.Units.push_back({Entry.Key, Entry.Digest, Entry.PackageSpaceKey, *Entry.CoarseBounds});
		Check(!Proof.Units.empty(), "production package has no spatial manifest units");
		const auto Compatible = InspectResidencySpatialMetadata(Package.Manifest, Proof);
		Check(Compatible.Status == ResidencyMetadataStatus::Compatible &&
			Compatible.SpatialUnits == Proof.Units.size() && Compatible.CompatibleUnits == Proof.Units.size(),
			"package bounds did not pass exact detached compatibility coverage");
		// Exact-copy compatibility proves identity/coverage only; it cannot prove the writer's geometry extent.
		auto Changed = Proof;
		Changed.Units.front().ContentDigest = DigestOf("changed payload");
		CheckIssue(InspectResidencySpatialMetadata(Package.Manifest, Changed), ResidencyMetadataStatus::Rejected,
			ResidencyMetadataIssueCode::ContentMismatch, "changed content digest retained metadata trust");
		Changed = Proof;
		++Changed.Package.PackageVersion;
		CheckIssue(InspectResidencySpatialMetadata(Package.Manifest, Changed), ResidencyMetadataStatus::Rejected,
			ResidencyMetadataIssueCode::NamespaceMismatch, "changed package namespace retained metadata trust");
		Changed = Proof;
		Changed.Units.front().ConservativeBounds.Maximum[0] += 1.0f;
		CheckIssue(InspectResidencySpatialMetadata(Package.Manifest, Changed), ResidencyMetadataStatus::Rejected,
			ResidencyMetadataIssueCode::UnderboundedManifest, "underbounded package geometry was accepted");
		Changed = Proof;
		Changed.Units.erase(Changed.Units.begin());
		CheckIssue(InspectResidencySpatialMetadata(Package.Manifest, Changed), ResidencyMetadataStatus::Incomplete,
			ResidencyMetadataIssueCode::MissingMetadata, "missing spatial metadata became a complete miss");
		Changed = Proof;
		++Changed.Version;
		CheckIssue(InspectResidencySpatialMetadata(Package.Manifest, Changed), ResidencyMetadataStatus::Rejected,
			ResidencyMetadataIssueCode::UnsupportedVersion, "unsupported metadata version was accepted");
		Changed = Proof;
		Changed.Units.front().PackageSpaceKey = "other";
		CheckIssue(InspectResidencySpatialMetadata(Package.Manifest, Changed), ResidencyMetadataStatus::Rejected,
			ResidencyMetadataIssueCode::SpaceMismatch, "wrong package space was accepted");

		PackageContentCoarseIndex First(Package.Manifest);
		PackageContentCoarseIndex Second(Package.Manifest);
		const auto Unit = std::ranges::find_if(Proof.Units, [&](const ResidencyUnitBoundsMetadata &Value) {
			return Value.Key == Package.CandidateKeys.front();
		});
		Check(Unit != Proof.Units.end(), "named spatial candidate was absent from metadata coverage");
		const PackageCoarseBounds Query{{0, 0, 0}, {0, 0, 0}};
		std::optional<PackageCoarseQueryCursor> FirstCursor;
		std::optional<PackageCoarseQueryCursor> SecondCursor;
		std::vector<std::size_t> Candidates;
		bool Completed = false;
		for (std::size_t PageNumber = 0; PageNumber < MaximumPackageCoarseQueryCells; ++PageNumber) {
			const auto A = First.QueryPage(Unit->PackageSpaceKey, Query, {}, FirstCursor);
			const auto B = Second.QueryPage(Unit->PackageSpaceKey, Query, {}, SecondCursor);
			Check(A.Status == B.Status && A.Entries == B.Entries &&
				A.Status != PackageCoarseQueryStatus::InvalidInput &&
				A.Status != PackageCoarseQueryStatus::QueryCellLimit &&
				A.Status != PackageCoarseQueryStatus::AllocationFailure,
				"equivalent package indexes disagreed or rejected a valid candidate query");
			Candidates.insert(Candidates.end(), A.Entries.begin(), A.Entries.end());
			if (A.Status == PackageCoarseQueryStatus::Complete) {
				Completed = true;
				break;
			}
			Check(A.Status == PackageCoarseQueryStatus::More && A.NextCursor && B.NextCursor,
				"incomplete package candidate page lost its continuation");
			FirstCursor = A.NextCursor;
			SecondCursor = B.NextCursor;
		}
		Check(Completed && std::ranges::is_sorted(Candidates),
			"package spatial candidate page did not complete in deterministic manifest order");
		for (const auto &Key : Package.CandidateKeys)
			Check(std::ranges::any_of(Candidates, [&](std::size_t Index) {
				return Index < Package.Manifest.Entries.size() && Package.Manifest.Entries[Index].Key == Key;
			}), "a named package candidate was absent from the shared origin cell");
		Check(First.QueryPage(Unit->PackageSpaceKey, Query, {0, 1, 1}).Status ==
			PackageCoarseQueryStatus::InvalidInput, "invalid candidate budget became an empty spatial miss");
	}

	void BindingAndFallback(const PackageFixture &Package) {
		ServiceFixture Original(Package.Configuration());
		const auto Snapshot = Original.Service->GetManifestSnapshot();
		Check(Snapshot && Snapshot->Package == Package.Manifest.Package &&
			Snapshot->Entries.size() == Package.Manifest.Entries.size(), "local provider changed package identity");
		const auto Key = Package.Manifest.Entries.front().Key;
		{
			ResidencyDemandPolicy Critical(Snapshot);
			Check(Critical.SetRequirement(Key, ResidencyDemandClass::Critical), "trusted critical fallback was rejected");
			Critical.Step(Original.Service.get(), 1);
			Check(Critical.GetMetrics().Requests == 1 && Original.Service->GetMetrics().Requests == 1 &&
				Critical.GetMetrics().ManifestBindingEntriesCompared == 0,
				"exact package snapshot did not permit one trusted fallback request");
			Critical.Stop(Original.Service.get());
		}
		auto Changed = Package.Manifest;
		Changed.Entries.front().Digest = DigestOf("replacement payload");
		ServiceFixture DigestService(ManifestConfiguration(Changed));
		ResidencyDemandPolicy DigestPolicy(Package.Manifest);
		Check(DigestPolicy.SetRequirement(Key, ResidencyDemandClass::Critical), "digest fixture requirement rejected");
		DigestPolicy.Step(DigestService.Service.get(), 1);
		Check(DigestPolicy.GetMetrics().Requests == 0 && DigestPolicy.GetMetrics().ServiceBindingFailures == 1 &&
			DigestService.Service->GetMetrics().Requests == 0, "changed digest acquired content through stale policy");
		DigestPolicy.Stop(nullptr);

		Changed = Package.Manifest;
		Changed.Entries.front().Dependencies = {Changed.Entries.back().Key};
		ServiceFixture DependencyService(ManifestConfiguration(Changed));
		ResidencyDemandPolicy DependencyPolicy(Package.Manifest);
		Check(DependencyPolicy.SetRequirement(Key, ResidencyDemandClass::Critical), "dependency fixture requirement rejected");
		DependencyPolicy.Step(DependencyService.Service.get(), 1);
		Check(DependencyPolicy.GetMetrics().Requests == 0 &&
			DependencyPolicy.GetMetrics().ServiceBindingFailures == 1 &&
			DependencyService.Service->GetMetrics().Requests == 0,
			"changed dependency graph acquired content through stale policy");
		DependencyPolicy.Stop(nullptr);
		Changed = Package.Manifest;
		++Changed.Package.PackageVersion;
		ServiceFixture NamespaceService(ManifestConfiguration(Changed));
		ResidencyDemandPolicy NamespacePolicy(Package.Manifest);
		Check(NamespacePolicy.SetRequirement(Key, ResidencyDemandClass::Critical), "namespace fixture requirement rejected");
		NamespacePolicy.Step(NamespaceService.Service.get(), 1);
		Check(NamespacePolicy.GetMetrics().Requests == 0 && NamespacePolicy.GetMetrics().ServiceBindingFailures == 1 &&
			NamespaceService.Service->GetMetrics().Requests == 0,
			"changed namespace acquired content through stale policy");
		NamespacePolicy.Stop(nullptr);

		auto ClientWorld = std::make_shared<DataModel>();
		HeadlessRenderer Renderer(Vector2(32, 32));
		Engine Client(ClientWorld, &Renderer, {}, {.Content = Package.Configuration(),
			.Mode = RuntimeMode::NetworkClient, .EnableResidencyDemandPolicy = true});
		Check(Client.GetResidencyDemandPolicy() == nullptr,
			"client composition installed server-only trusted residency policy");
		Client.Destroy();
	}
}

int main() {
	try {
		Check(SDL_Init(0), "SDL initialization failed");
		struct SdlLifetime final { ~SdlLifetime() { SDL_Quit(); } } Sdl;
		PackageFixture Package;
		MetadataAndCandidates(Package);
		BindingAndFallback(Package);
		std::cout << "[Content:CrossConsumer] package metadata, candidates, binding and authority passed\n";
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Content:CrossConsumer] " << Error.what() << '\n';
		return 1;
	}
}
