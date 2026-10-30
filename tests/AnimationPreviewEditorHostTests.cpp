#include "gargantuan/editor/EditorHost.hpp"
#include "gargantuan/editor/AnimationPreview.hpp"
#include "gargantuan/animation/AnimationTrack.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/services/AssetService.hpp"

#include <SDL3/SDL.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>

int main(int ArgumentCount, char **Arguments) {
	using namespace gargantuan;
	using Json = nlohmann::ordered_json;
	const auto Root = std::filesystem::temp_directory_path() /
		("gargantuan-animation-preview-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	struct Cleanup {
		std::filesystem::path Root;
		~Cleanup() { std::error_code Ignored; std::filesystem::remove_all(Root, Ignored); SDL_Quit(); }
	} CleanupValue{Root};
	try {
		if (!SDL_Init(0)) throw std::runtime_error("SDL initialization failed");
		BootstrapNativeRuntimeSchema();
		std::size_t Checks = 0, Sequence = 0;
		auto Check = [&](bool Condition, const char *Message) {
			if (!Condition) throw std::runtime_error(Message);
			++Checks;
		};
		EditorHost Host("animation-preview-test");
		EditorHost DeniedHost("denied-preview-test", ScriptSecurityContext{});
		auto Denied = Json::parse(DeniedHost.HandleRequest(Json{{"Version", 1}, {"RequestId", "denied"},
			{"SessionToken", "denied-preview-test"}, {"Method", "GetAnimationRig"},
			{"Params", {{"Reference", "asset://00000000000000000000000000000000"}}}}.dump()));
		Check(!Denied["Ok"] && Denied["Error"]["Code"] == "Unauthorized", "Read capability is required before asset access");
		auto Call = [&](std::string Method, Json Parameters = Json::object()) {
			auto Result = Json::parse(Host.HandleRequest(Json{{"Version", 1}, {"RequestId", std::to_string(++Sequence)},
				{"SessionToken", "animation-preview-test"}, {"Method", Method}, {"Params", std::move(Parameters)}}.dump()));
			if (!Result["Ok"]) std::cout << "[Animation:EditorHost] " << Method << " " << Result["Error"].dump() << '\n';
			return Result;
		};
		Check(Call("CreateProject", {{"Destination", Root.generic_string()}, {"Name", "Animation Preview"}})["Ok"], "Create preview test project");
		std::filesystem::create_directory(Root / "assets");
		std::filesystem::copy_file(GARGANTUAN_ANIMATION_PREVIEW_FIXTURE, Root / "assets" / "rig.gltf");
		Check(Call("ImportAsset", {{"Source", "assets/rig.gltf"}})["Ok"], "Import three-clip rig through EditorHost");
		auto Catalog = Call("GetAssetCatalog", {{"IncludeBuiltIns", false}});
		std::string Reference;
		for (const auto &Asset : Catalog["Result"]["Assets"])
			if (Asset["Kind"] == "Mesh") Reference = Asset["Reference"].get<std::string>();
		Check(!Reference.empty(), "Imported mesh is in canonical catalog");
		auto Rig = Call("GetAnimationRig", {{"Reference", Reference}});
		std::cout << "[Animation:EditorHost] joints=" << Rig["Result"]["Joints"].size()
			<< " clips=" << Rig["Result"]["Clips"].size() << '\n';
		Check(Rig["Ok"] && Rig["Result"]["Joints"].size() == 2 && Rig["Result"]["Clips"].size() == 3,
			"Read-only inspector exposes hierarchy and three clips");
		Check(Rig["Result"]["Joints"][1]["Path"] == "BeaconRoot/BeaconTip" && Rig["Result"]["Joints"][1]["Parent"] == 0,
			"Inspector retains canonical joint paths and parents");
		const auto State = Call("GetProjectState")["Result"];
		auto Open = Call("OpenAnimationPreview", {{"Reference", Reference}});
		Check(Open["Ok"], "Open native-only preview");
		const auto Id = Open["Result"]["PreviewId"];
		Check(!Call("OpenAnimationPreview", {{"Reference", Reference}})["Ok"], "Only one bounded preview is admitted");
		const auto Clip = Rig["Result"]["Clips"][0]["Reference"];
		Check(Call("ControlAnimationPreview", {{"PreviewId", Id}, {"Action", "Clip"}, {"Value", Clip}})["Ok"], "Select canonical clip");
		for (const auto &Action : {"Play", "Pause", "Stop", "Reset"})
			Check(Call("ControlAnimationPreview", {{"PreviewId", Id}, {"Action", Action}})["Ok"], "Playback control succeeds");
		Check(Call("ControlAnimationPreview", {{"PreviewId", Id}, {"Action", "Seek"}, {"Value", 0.5}})["Ok"], "Seek after Stop succeeds");
		Check(!Call("ControlAnimationPreview", {{"PreviewId", Id}, {"Action", "Speed"}, {"Value", 17.0}})["Ok"], "Invalid speed is rejected");
		Check(!Call("ControlAnimationPreview", {{"PreviewId", Id}, {"Action", "Play"}, {"Extra", true}})["Ok"], "Unknown control fields are rejected");
		Check(!Call("CaptureAnimationPreview", {{"PreviewId", Id}, {"Width", 1025}, {"Height", 1}, {"Position", {2, 2, 2}}, {"Target", {0, 0, 0}}})["Ok"], "Capture size fails before GPU allocation");
		Check(Call("GetProjectState")["Result"] == State, "Preview controls never change authoring state/history");
		Check(Call("CloseAnimationPreview", {{"PreviewId", Id}})["Ok"], "Close releases preview");
		Check(!Call("ControlAnimationPreview", {{"PreviewId", Id}, {"Action", "Play"}})["Ok"], "Closed preview identity is stale");
		Check(Call("ReimportAsset", {{"Reference", Reference}})["Ok"], "Compatible reimport succeeds");
		auto Reimported = Call("GetAnimationRig", {{"Reference", Reference}});
		Check(Reimported["Ok"] && Reimported["Result"]["MeshReference"] == Reference &&
			Reimported["Result"]["SkeletonCompatibilityId"] == Rig["Result"]["SkeletonCompatibilityId"], "Compatible reimport preserves asset and skeleton identity");
		Check(Call("SaveProject")["Ok"], "Save imported canonical assets");
		auto Second = Call("OpenAnimationPreview", {{"Reference", Reference}});
		Check(Second["Ok"] && Second["Result"]["PreviewId"] != Id, "New preview receives fresh identity");
		Check(Call("OpenProject", {{"Root", Root.generic_string()}})["Ok"], "Reopen project");
		Check(!Call("ControlAnimationPreview", {{"PreviewId", Second["Result"]["PreviewId"]}, {"Action", "Play"}})["Ok"], "Project replacement retires preview");
		auto Reopened = Call("GetAnimationRig", {{"Reference", Reference}});
		Check(Reopened["Ok"] && Reopened["Result"]["Joints"] == Rig["Result"]["Joints"] && Reopened["Result"]["Clips"].size() == 3,
			"Save/reopen retains canonical rig and clip references");
		// Explicit manual GPU gate; ordinary CI remains genuinely headless.
		if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--gpu") {
			DiskFilesystem Filesystem(Root);
			auto ProjectValue = Project::fromExisting(&Filesystem);
			auto SourceWorld = ProjectValue.DeserializeGame();
			auto Assets = std::dynamic_pointer_cast<AssetService>(SourceWorld->GetService("AssetService"));
			AnimationPreview Scene(Assets->CaptureProjectAssets(), Reference);
			for (const auto &Entry : Rig["Result"]["Clips"])
				if (Entry["Name"] == "BeaconPulse") Scene.SelectClip(Entry["Reference"].get<std::string>());
			Check(Scene.GetTrack() != nullptr, "GPU fixture has the canonical Pulse clip");
			const auto VideoBefore = SDL_WasInit(SDL_INIT_VIDEO);
			auto Primary = std::make_unique<EditorViewportRenderer>(64, 64);
			auto Secondary = std::make_unique<EditorViewportRenderer>(320, 240);
			Primary.reset();
			Check(SDL_WasInit(SDL_INIT_VIDEO) != 0, "Preview retains SDL video after authoring renderer teardown");
			const auto Camera = MakeLookAtRenderCameraInput({2, 2, 4}, {0, .5f, 0}, {0, 1, 0});
			Scene.GetTrack()->SetTimePosition(0);
			Scene.Step(0);
			auto First = Secondary->Capture(Scene.Publish(Camera, 320, 240));
			Scene.GetTrack()->SetTimePosition(.5f);
			Scene.Step(0);
			auto Second = Secondary->Capture(Scene.Publish(Camera, 320, 240));
			std::size_t Changed = 0;
			for (std::size_t Index = 0; Index < First.RgbPixels.size(); ++Index)
				if (First.RgbPixels[Index] != Second.RgbPixels[Index]) ++Changed;
			Check(Changed > 30, "Native GPU skinning changes rendered mesh pixels after seek");
			auto WriteFrame = [](const char *Path, const EditorViewportFrame &Frame) {
				std::ofstream Output(Path, std::ios::binary);
				Output << "P6\n" << Frame.Width << ' ' << Frame.Height << "\n255\n";
				Output.write(reinterpret_cast<const char *>(Frame.RgbPixels.data()), static_cast<std::streamsize>(Frame.RgbPixels.size()));
				if (!Output) throw std::runtime_error("Could not preserve native preview frame");
			};
			WriteFrame("animation-preview-0.ppm", First);
			WriteFrame("animation-preview-seek.ppm", Second);
			Secondary.reset();
			Check(SDL_WasInit(SDL_INIT_VIDEO) == VideoBefore, "Preview renderer releases only its own video reference");
			SourceWorld->Destroy();
			std::cout << "[Animation:PreviewGpu] PASS changed_channels=" << Changed << " frames=2\n";
		}
		std::cout << "[Animation:EditorHost] PASS checks=" << Checks << '\n';
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Animation:EditorHost] FAIL " << Error.what() << '\n';
		return 1;
	}
}
