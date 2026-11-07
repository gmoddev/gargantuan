#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/MeshPart.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/SourceMount.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gargantuan {
	struct WorldRootTestAccess {
		static PhysicsCollisionResidency Collision(const WorldRoot &World) {
			return World.Physics.GetCollisionResidency();
		}
		static PhysicsBodyId Body(const WorldRoot &World, const BasePart &Part) {
			const auto Found = World.PartBodies.find(Part.GetObjectId());
			return Found == World.PartBodies.end() ? PhysicsBodyId{} : Found->second;
		}
		static WorldRaycastResult Raycast(WorldRoot &World, glm::vec3 Origin, glm::vec3 Direction) {
			return World.ResolveRaycast(Origin, Direction);
		}
	};
}

namespace {
	using namespace gargantuan;
	using Clock = std::chrono::steady_clock;
	constexpr int GridWidth = 20;
	constexpr int GridHeight = 10;
	constexpr int TriangleCount = GridWidth * GridHeight * 2;

	void Require(bool Condition, const std::string &Message) {
		if (!Condition) throw std::runtime_error("[Physics:ImportedMesh] " + Message);
	}

	std::string GetGridObj() {
		std::ostringstream Source;
		for (int Z = 0; Z <= GridHeight; ++Z)
			for (int X = 0; X <= GridWidth; ++X)
				Source << "v " << X - GridWidth / 2 << " 0 " << Z - GridHeight / 2 << '\n';
		for (int Z = 0; Z < GridHeight; ++Z)
			for (int X = 0; X < GridWidth; ++X) {
				const int A = Z * (GridWidth + 1) + X + 1;
				const int B = A + 1;
				const int C = A + GridWidth + 1;
				const int D = C + 1;
				// Upward winding makes the imported surface a solid floor.
				Source << "f " << A << ' ' << C << ' ' << B << '\n';
				Source << "f " << B << ' ' << C << ' ' << D << '\n';
			}
		return Source.str();
	}

	double GetPercentile(const std::vector<double> &Sorted, double Fraction) {
		return Sorted[static_cast<std::size_t>((Sorted.size() - 1) * Fraction)];
	}

	void Run() {
		const auto Root = std::filesystem::temp_directory_path() /
			("gargantuan-physics-imported-mesh-" +
				std::to_string(Clock::now().time_since_epoch().count()));
		struct Cleanup {
			std::filesystem::path Root;
			~Cleanup() { std::error_code Error; std::filesystem::remove_all(Root, Error); }
		} CleanupValue{Root};
		const auto ObjPath = Root / "assets" / "dense-floor.obj";
		std::filesystem::create_directories(ObjPath.parent_path());
		const auto Obj = GetGridObj();
		{
			std::ofstream Output(ObjPath, std::ios::binary | std::ios::trunc);
			Output.write(Obj.data(), static_cast<std::streamsize>(Obj.size()));
			Require(static_cast<bool>(Output), "Could not write bounded OBJ fixture");
		}

		DiskFilesystem Filesystem(Root);
		SourceMount Mount(Filesystem);
		auto Game = std::make_shared<DataModel>();
		Game->Root = Root;
		Game->Filesystem = &Filesystem;
		auto Space = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		auto Assets = std::dynamic_pointer_cast<AssetService>(Game->GetService("AssetService"));
		Require(Space && Assets, "Workspace or canonical AssetService is missing");
		const auto ImportStarted = Clock::now();
		const auto Imported = Assets->ImportProjectAsset(Mount, "assets/dense-floor.obj", AssetKind::Mesh, "Dense floor");
		const auto ImportUs = std::chrono::duration<double, std::micro>(Clock::now() - ImportStarted).count();
		Require(Imported.Ok && Imported.Record && Imported.Records.size() == 2,
			"Real OBJ import did not publish paired Mesh and Collision assets: " + Imported.Diagnostic.Code);
		const auto Collision = Assets->ResolveCollisionForMesh(Imported.Record->Reference.Value);
		Require(Collision && Collision->Value.Positions && Collision->Value.Indices &&
			Collision->Id != Imported.Record->Id &&
			Collision->Value.Indices->size() == TriangleCount * 3,
			"Imported collision artifact lost dense source triangles");
		const auto AssetStats = Assets->ReadCollisionStats();
		Require(AssetStats.ResidentRecords >= 1 && AssetStats.RetainedArtifactBytes > 0,
			"AssetService did not retain imported collision artifact bytes");

		auto Floor = std::make_shared<MeshPart>();
		Floor->SetName("ImportedDenseFloor");
		Floor->SetAnchored(true);
		Floor->SetCanCollide(true);
		Floor->SetCanTouch(true);
		Floor->SetSize({1.0f, 1.0f, 1.0f});
		Floor->SetMesh(Imported.Record->Reference.Value);
		Floor->SetCollisionMode(Enums::MeshCollisionMode::StaticMesh);
		Floor->SetParent(Space);
		Space->StepPhysics(0.0f, std::nullopt);
		const auto Admitted = WorldRootTestAccess::Collision(*Space);
		Require(WorldRootTestAccess::Body(*Space, *Floor).IsValid() &&
			Admitted.ActiveResources == 1 && Admitted.ActiveConsumers == 1 &&
			Admitted.NativeChunks >= 2 && Admitted.ActiveMeshShapes >= 2 &&
			Admitted.NativeMeshBytes > 0 && Admitted.FailedAdmissions == 0,
			"WorldRoot did not admit all imported collision chunks");
		for (const float X : {-3.25f, 3.25f}) {
			const auto Hit = WorldRootTestAccess::Raycast(*Space, {X, 5.0f, 0.25f}, {0.0f, -10.0f, 0.0f});
			Require(Hit.Succeeded() && Hit.Instance == Floor && std::abs(Hit.Position.y) < 0.05f &&
				Hit.Normal.y > 0.9f,
				"WorldRoot ray missed the authored imported surface");
		}

		auto AddBody = [&](std::string Name, Enums::PartType Shape, float X, float Y) {
			auto Body = std::make_shared<Part>();
			Body->SetName(std::move(Name));
			Body->SetShape(Shape);
			Body->SetSize({1.0f, 1.0f, 1.0f});
			Body->SetAnchored(false);
			Body->SetCanCollide(true);
			Body->SetCanTouch(true);
			Body->SetCFrame(CFrame(X, Y, 0.25f));
			Body->SetParent(Space);
			return Body;
		};
		int FloorContacts = 0;
		auto TouchConnection = Floor->Touched->Connect([&](std::shared_ptr<BasePart> Other) {
			if (Other && Other->GetName() != "ImportedDenseFloor") ++FloorContacts;
		});
		auto Sphere = AddBody("SphereProbe", Enums::PartType::Ball, -3.25f, 2.5f);
		auto Lower = AddBody("StackLower", Enums::PartType::Block, 3.25f, 2.5f);
		auto Upper = AddBody("StackUpper", Enums::PartType::Block, 3.25f, 4.0f);
		Require(WorldRootTestAccess::Body(*Space, *Sphere).IsValid() &&
			WorldRootTestAccess::Body(*Space, *Lower).IsValid() &&
			WorldRootTestAccess::Body(*Space, *Upper).IsValid(),
			"Dynamic probes did not enter the WorldRoot backend");
		std::vector<double> StepUs;
		StepUs.reserve(240);
		for (int Tick = 0; Tick < 240; ++Tick) {
			const auto Started = Clock::now();
			Space->StepPhysics(1.0f / 60.0f, std::nullopt);
			StepUs.push_back(std::chrono::duration<double, std::micro>(Clock::now() - Started).count());
		}
		const auto SpherePosition = Sphere->GetCFrame().Position;
		const auto LowerPosition = Lower->GetCFrame().Position;
		const auto UpperPosition = Upper->GetCFrame().Position;
		Require(FloorContacts >= 2 &&
			std::isfinite(SpherePosition.y) && SpherePosition.y >= 0.35f && SpherePosition.y <= 0.95f &&
			std::isfinite(LowerPosition.y) && LowerPosition.y >= 0.35f && LowerPosition.y <= 0.95f &&
			std::isfinite(UpperPosition.y) && UpperPosition.y >= 1.1f && UpperPosition.y <= 2.1f,
			"Dynamic sphere and box stack did not settle on imported collision surface");
		TouchConnection->Disconnect();
		Sphere->Destroy();
		Lower->Destroy();
		Upper->Destroy();
		Floor->Destroy();
		Space->StepPhysics(0.0f, std::nullopt);
		const auto Retired = WorldRootTestAccess::Collision(*Space);
		Require(Retired.ActiveResources == 0 && Retired.ActiveConsumers == 0 &&
			Retired.ActiveMeshShapes == 0 && Retired.NativeMeshBytes == 0 &&
			Retired.CreatedResources == Retired.RetiredResources &&
			Retired.CreatedConsumers == Retired.RetiredConsumers &&
			Retired.CreatedMeshShapes == Retired.RetiredMeshShapes,
			"Imported collision chunks or consumers survived WorldRoot retirement");
		std::sort(StepUs.begin(), StepUs.end());
		std::cout << "[Physics:ImportedMesh] triangles=" << TriangleCount
			<< " sourceBytes=" << Obj.size()
			<< " importUs=" << ImportUs
			<< " assetArtifactBytes=" << AssetStats.RetainedArtifactBytes
			<< " admittedChunks=" << Admitted.NativeChunks
			<< " admittedMeshShapes=" << Admitted.ActiveMeshShapes
			<< " admittedNativeBytes=" << Admitted.NativeMeshBytes
			<< " contactStepSamples=" << StepUs.size()
			<< " contactStepP50Us=" << GetPercentile(StepUs, 0.50)
			<< " contactStepP95Us=" << GetPercentile(StepUs, 0.95)
			<< " contactStepP99Us=" << GetPercentile(StepUs, 0.99)
			<< " floorContacts=" << FloorContacts
			<< " sphereY=" << SpherePosition.y
			<< " lowerY=" << LowerPosition.y
			<< " upperY=" << UpperPosition.y
			<< " retiredResources=" << Retired.RetiredResources
			<< " retiredMeshShapes=" << Retired.RetiredMeshShapes << '\n';
		Game->Destroy();
	}
}

int main() {
#ifdef _WIN32
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
		Run();
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Physics:ImportedMesh] " << Error.what() << '\n';
		return 1;
	}
}
