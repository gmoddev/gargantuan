#include "gargantuan/editor/EditorHost.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"

#include <SDL3/SDL.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

int main() {
	using namespace gargantuan;
	using Json = nlohmann::ordered_json;
	const auto Root = std::filesystem::temp_directory_path() /
		("gargantuan-cross-inspection-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	struct Cleanup {
		std::filesystem::path Root;
		~Cleanup() {
			std::error_code Ignored;
			std::filesystem::remove_all(Root, Ignored);
			SDL_Quit();
		}
	} CleanupValue{Root};
	try {
		if (!SDL_Init(0)) throw std::runtime_error("SDL initialization failed");
		BootstrapNativeRuntimeSchema();
		std::size_t Checks = 0, Sequence = 0;
		auto Check = [&](bool Condition, const char *Message) {
			if (!Condition) throw std::runtime_error(Message);
			++Checks;
		};
		EditorHost Host("cross-inspection");
		auto Call = [&](const char *Method, Json Parameters, const char *Token = "cross-inspection") {
			return Json::parse(Host.HandleRequest(Json{
				{"Version", EditorHostProtocolVersion}, {"RequestId", std::to_string(++Sequence)},
				{"SessionToken", Token}, {"Method", Method}, {"Params", std::move(Parameters)},
			}.dump()));
		};
		EditorHost Denied("denied-inspection", ScriptSecurityContext{});
		auto Denial = Json::parse(Denied.HandleRequest(Json{
			{"Version", EditorHostProtocolVersion}, {"RequestId", "denied"},
			{"SessionToken", "denied-inspection"}, {"Method", "GetAssetCatalog"},
			{"Params", {{"IncludeBuiltIns", false}}},
		}.dump()));
		Check(!Denial["Ok"] && Denial["Error"]["Code"] == "Unauthorized", "Catalog still requires ReadDataModel");
		const auto TokenDenial = Call("GetAssetCatalog", Json::object(), "wrong-token");
		Check(!TokenDenial["Ok"] && TokenDenial["Error"]["Code"] == "Unauthorized", "Catalog still requires the exact token");
		Check(Call("CreateProject", {{"Destination", Root.generic_string()}, {"Name", "Geometry inspection"}})["Ok"],
			"Create disposable inspection project");
		std::filesystem::create_directory(Root / "assets");
		{
			std::ofstream Source(Root / "assets" / "offset.obj", std::ios::binary);
			Source << "v 10 -2 1\nv 14 -2 1\nv 10 3 1\nf 1 2 3\n";
			if (!Source) throw std::runtime_error("Cannot write OBJ fixture");
		}
		auto Imported = Call("ImportAsset", {{"Source", "assets/offset.obj"}, {"Kind", "Mesh"}});
		Check(Imported["Ok"] && Imported["Result"]["OperationSucceeded"], "Import canonical Mesh/Collision pair");
		const auto State = Call("GetProjectState", Json::object())["Result"];
		auto Catalog = Call("GetAssetCatalog", {{"IncludeBuiltIns", false}});
		Check(Catalog["Ok"] && Catalog["Result"]["CatalogVersion"] == 2, "Existing catalog version is preserved");
		Json Mesh, Collision;
		for (const auto &Record : Catalog["Result"]["Assets"]) {
			if (Record["Kind"] == "Mesh") Mesh = Record;
			if (Record["Kind"] == "Collision") Collision = Record;
		}
		Check(!Mesh.is_null() && !Collision.is_null(), "Both canonical records are inspectable");
		const auto &MeshMetadata = Mesh["Metadata"];
		const auto &CollisionMetadata = Collision["Metadata"];
		Check(MeshMetadata["GeometryInspectionVersion"] == 1 && CollisionMetadata["GeometryInspectionVersion"] == 1,
			"Geometry inspection is independently versioned");
		Check(MeshMetadata["LocalBounds"]["Space"] == "AssetLocal" &&
			MeshMetadata["LocalBounds"]["Coverage"] == "DeclaredMesh" &&
			MeshMetadata["LocalBounds"]["Minimum"] == Json::array({10, -2, 1}) &&
			MeshMetadata["LocalBounds"]["Maximum"] == Json::array({14, 3, 1}),
			"Bounds retain canonical local coordinates without normalization or Part scaling");
		Check(CollisionMetadata["LocalBounds"]["Space"] == "AssetLocal" &&
			CollisionMetadata["LocalBounds"]["Coverage"] == "DeclaredCollision" &&
			CollisionMetadata["LocalBounds"]["Minimum"] == MeshMetadata["LocalBounds"]["Minimum"] &&
			CollisionMetadata["LocalBounds"]["Maximum"] == MeshMetadata["LocalBounds"]["Maximum"] &&
			CollisionMetadata["Representation"] == "StaticTriangles" &&
			CollisionMetadata["VertexCount"] == 3 && CollisionMetadata["IndexCount"] == 3,
			"Collision metadata describes canonical triangles, not native shapes");
		Check(MeshMetadata["CollisionAssetId"] == Collision["AssetId"] &&
			!Mesh["ContentId"].is_null() && Mesh["ContentRevision"].get<std::uint64_t>() > 0 &&
			!Collision["ContentId"].is_null() && Collision["ContentRevision"].get<std::uint64_t>() > 0,
			"Declared collision identity remains associated with each exact catalog content revision");
		Check(Call("GetAssetCatalog", {{"IncludeBuiltIns", false}})["Result"] == Catalog["Result"] &&
			Call("GetProjectState", Json::object())["Result"] == State,
			"Repeated inspection is stable and changes no authoring state or history");
		std::filesystem::copy_file(std::filesystem::path(GARGANTUAN_ANIMATION_PREVIEW_FIXTURE), Root / "assets" / "rig.gltf");
		auto RigImport = Call("ImportAsset", {{"Source", "assets/rig.gltf"}});
		Check(RigImport["Ok"] && RigImport["Result"]["OperationSucceeded"], "Import skinned bind-pose fixture");
		auto RigCatalog = Call("GetAssetCatalog", {{"IncludeBuiltIns", false}});
		std::size_t BindPoseRecords = 0;
		for (const auto &Record : RigCatalog["Result"]["Assets"]) {
			if (Record["Kind"] != "Mesh" || !Record["Metadata"].contains("JointCount")) continue;
			++BindPoseRecords;
			Check(Record["Metadata"]["LocalBounds"]["Coverage"] == "DeclaredBindPose" &&
				Record["Metadata"]["CollisionAssetId"].is_null(),
				"Skinned inspection is bind-pose data, not a conservative animation or collision envelope");
		}
		Check(BindPoseRecords == 1, "Actual skinned fixture was inspected");
		std::cout << "[Integration:Authoring] PASS checks=" << Checks << '\n';
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Integration:Authoring] FAIL " << Error.what() << '\n';
		return 1;
	}
}
