#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/classes/MeshPart.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/SourceMount.hpp"
#include "gargantuan/network/ReplicationRelevance.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/services/AssetService.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
	using namespace gargantuan;
	using namespace gargantuan::network;

	void Require(bool Condition, const char *Message) {
		if (!Condition) throw std::runtime_error(Message);
	}

	void WriteObj(const std::filesystem::path &Path, int Offset) {
		std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
		Output << "v " << Offset << " 0 0\n"
			   << "v " << Offset + 1 << " 0 0\n"
			   << "v " << Offset << " 1 0\n"
			   << "f 1 2 3\n";
		Require(static_cast<bool>(Output), "offset OBJ fixture write failed");
	}

	bool Desired(const ReplicationRelevance &Relevance, ConnectionId Peer, ObjectId Object) {
		const auto *Selection = Relevance.GetSelection(Peer);
		return Selection && std::ranges::binary_search(Selection->DesiredObjects, Object);
	}

	void RunDetachedSourceRoot() {
		auto Root = std::make_shared<Folder>();
		auto Identity = std::make_shared<Folder>();
		Identity->SetParent(Root);
		auto Missing = std::make_shared<MeshPart>();
		Missing->SetMesh("asset://ffffffffffffffffffffffffffffffff");
		Missing->SetParent(Root);
		Require(!Root->GetDataModel() && !Root->FindFirstChildOfClass("AssetService", true),
			"detached source begins without authoritative asset service");
		ReplicationRelevance Relevance(Root);
		const ConnectionId Peer{54, 1};
		Require(Relevance.AddPeer(Peer, Identity->GetObjectId()), "detached peer rejected");
		Require(!Desired(Relevance, Peer, Missing->GetObjectId()),
			"detached missing mesh keeps no-focus selection empty");
		const glm::vec3 Focus{10.0f, 0.0f, 0.0f};
		Require(Relevance.SetTrustedFocus(Peer, std::span(&Focus, 1)) && Relevance.Update(1),
			"detached missing mesh focus update failed");
		Require(Desired(Relevance, Peer, Missing->GetObjectId()) && Relevance.VerifySpatialIndex(),
			"detached missing mesh remains conservatively relevant");
		Require(!Root->GetDataModel() && !Root->FindFirstChildOfClass("AssetService", true),
			"relevance observation must not create an authoritative asset service");
		Root->Destroy();
	}

	void Run() {
		const auto Root = std::filesystem::temp_directory_path() /
			("gargantuan-mesh-relevance-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		struct Cleanup final {
			std::filesystem::path Root;
			~Cleanup() { std::error_code Ignored; std::filesystem::remove_all(Root, Ignored); }
		} CleanupValue{Root};
		std::filesystem::create_directories(Root / "assets");
		WriteObj(Root / "assets" / "offset.obj", 100);
		DiskFilesystem Filesystem(Root);
		SourceMount Mount(Filesystem);
		auto World = std::make_shared<DataModel>();
		World->Root = Root;
		World->Filesystem = &Filesystem;
		auto Assets = std::dynamic_pointer_cast<AssetService>(World->GetService("AssetService"));
		Require(Assets != nullptr, "canonical AssetService missing");
		const auto Imported = Assets->ImportProjectAsset(Mount, "assets/offset.obj", AssetKind::Mesh, "Offset");
		Require(Imported.Ok && Imported.Record, "offset OBJ import failed");
		const auto Reference = Imported.Record->Reference.Value;
		const auto Mesh = Assets->ResolveMeshResource(Reference);
		Require(Mesh && Mesh->Value.Bounds.Minimum.x >= 99.0f,
			"canonical OBJ geometry must retain its asymmetric local offset");

		auto Part = std::make_shared<MeshPart>();
		Part->SetMesh(Reference);
		Part->SetAnchored(true);
		Part->SetCollisionMode(Enums::MeshCollisionMode::StaticMesh);
		Part->SetSize({1.0f, 1.0f, 1.0f});
		Part->SetCFrame(CFrame(0.0f, 0.0f, 0.0f));
		Part->SetParent(World);
		const auto PartId = Part->GetObjectId();
		std::vector<std::shared_ptr<MeshPart>> Shared;
		for (int Index = 0; Index < 128; ++Index) {
			auto Copy = std::make_shared<MeshPart>();
			Copy->SetMesh(Reference);
			Copy->SetAnchored(true);
			Copy->SetCollisionMode(Enums::MeshCollisionMode::StaticMesh);
			Copy->SetCFrame(CFrame(1000.0f, 0.0f, 0.0f));
			Copy->SetParent(World);
			Shared.push_back(std::move(Copy));
		}

		auto Ordinary = std::make_shared<gargantuan::Part>();
		Ordinary->SetSize({1000.0f, 1.0f, 1.0f});
		Ordinary->SetCFrame(CFrame(50.0f, 0.0f, 0.0f));
		Ordinary->SetParent(World);

		ReplicationRelevanceConfiguration Configuration;
		Configuration.EnterRadius = 10.0f;
		Configuration.LeaveRadius = 20.0f;
		Configuration.UpdateIntervalTicks = 1;
		ReplicationRelevance Relevance(World, {}, Configuration);
		const auto InitialGeometry = Relevance.GetMetrics();
		Require(InitialGeometry.GeometryBoundsRebuilds == 1 && InitialGeometry.GeometryVertexVisits > 0,
			"shared canonical MeshParts validate vertices once per reference revision");
		const ConnectionId Peer{53, 1};
		auto PlayerIdentity = std::make_shared<Folder>();
		PlayerIdentity->SetParent(World);
		Require(Relevance.AddPeer(Peer, PlayerIdentity->GetObjectId()), "mesh relevance peer rejected");
		std::uint64_t Tick = 0;
		auto Focus = [&](float X) {
			const glm::vec3 Point{X, 0.0f, 0.0f};
			Require(Relevance.SetTrustedFocus(Peer, std::span(&Point, 1)), "trusted focus rejected");
			Require(Relevance.Update(++Tick), "mesh relevance update failed");
			Require(Relevance.VerifySpatialIndex(), "mesh projection index diverged");
		};

		// The conservative union also contains the inherited Box around the root.
		// Exercise the far side of the geometry AABB, outside that union.
		Focus(112.0f);
		Require(!Desired(Relevance, Peer, PartId), "outside enter distance stays irrelevant");
		Focus(100.0f);
		Require(Desired(Relevance, Peer, PartId), "offset geometry enters despite distant root pose");
		Require(!Desired(Relevance, Peer, Ordinary->GetObjectId()),
			"ordinary Part preserves root-pose exact relevance");
		Focus(120.0f);
		Require(Desired(Relevance, Peer, PartId), "existing mesh stays through leave corridor");
		Focus(122.0f);
		Require(!Desired(Relevance, Peer, PartId), "mesh leaves beyond geometry leave distance");

		Part->SetCFrame(CFrame(-100.0f, 0.0f, 0.0f));
		Focus(0.0f);
		Require(Desired(Relevance, Peer, PartId), "CFrame motion updates offset geometry envelope");
		Require(Relevance.GetMetrics().GeometryVertexVisits == InitialGeometry.GeometryVertexVisits,
			"transform-only projection update does not rescan shared vertices");
		Focus(100.0f);
		Require(!Desired(Relevance, Peer, PartId), "stale pre-motion envelope is retired");

		Part->SetCFrame(CFrame(0.0f, 0.0f, 0.0f));
		WriteObj(Root / "assets" / "offset.obj", -200);
		const auto Reimported = Assets->ReimportProjectAsset(Mount, Reference);
		Require(Reimported.Ok, "offset OBJ reimport failed");
		Focus(-199.0f);
		Require(Desired(Relevance, Peer, PartId), "asset change journal refreshes canonical mesh bounds");
		const auto RevisedGeometry = Relevance.GetMetrics();
		Require(RevisedGeometry.GeometryBoundsRebuilds == InitialGeometry.GeometryBoundsRebuilds + 1 &&
			RevisedGeometry.GeometryVertexVisits == InitialGeometry.GeometryVertexVisits * 2,
			"one reimport validates shared canonical vertices once across all roots");
		Focus(100.0f);
		Require(!Desired(Relevance, Peer, PartId), "old asset revision no longer grants relevance");

		Part->SetMesh("asset://ffffffffffffffffffffffffffffffff");
		Require(Relevance.SetTrustedFocus(Peer, std::span<const glm::vec3>{}), "no-focus input rejected");
		Require(Relevance.Update(++Tick), "no-focus fallback evaluation failed");
		Require(!Desired(Relevance, Peer, PartId), "unqualified mesh is absent before any trusted focus");
		Focus(10000.0f);
		Require(Desired(Relevance, Peer, PartId), "missing geometry uses bounded always-relevant fallback");

		auto Character = std::make_shared<KinematicCharacter>();
		Character->SetPosition({500.0f, 0.0f, 0.0f});
		Character->SetParent(World);
		auto ChildMesh = std::make_shared<MeshPart>();
		ChildMesh->SetMesh("asset://ffffffffffffffffffffffffffffffff");
		ChildMesh->SetParent(Character);
		Focus(0.0f);
		Require(!Desired(Relevance, Peer, ChildMesh->GetObjectId()),
			"Character-rooted MeshPart retains Character point policy");

		Part->Destroy();
		Focus(10000.0f);
		Require(!Desired(Relevance, Peer, PartId), "retired MeshPart identity leaves fallback and projection");
		const auto Metrics = Relevance.GetMetrics();
		std::cout << "[Replication:MeshGeometry] boundsRebuilds=" << Metrics.GeometryBoundsRebuilds
			<< " vertexVisits=" << Metrics.GeometryVertexVisits
			<< " cacheEntries=" << Metrics.GeometryCacheEntries
			<< " cacheLogicalBytes=" << Metrics.GeometryCacheLogicalBytes
			<< " unqualifiedRoots=" << Metrics.UnqualifiedMeshRoots
			<< " updateCpuNanoseconds=" << Metrics.UpdateCpuNanoseconds << '\n';
		World->Destroy();
	}
}

int main() {
	if (!SDL_Init(0)) {
		std::cerr << "[Replication:MeshGeometry] SDL initialization failed\n";
		return 1;
	}
	struct SdlLifetime final { ~SdlLifetime() { SDL_Quit(); } } Sdl;
	try {
		BootstrapNativeRuntimeSchema();
		RunDetachedSourceRoot();
		Run();
		std::cout << "[Replication:MeshGeometry] canonical offset, hysteresis, reimport, fallback, and teardown passed\n";
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Replication:MeshGeometry] " << Error.what() << '\n';
		return 1;
	}
}
