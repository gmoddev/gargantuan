#include "gargantuan/editor/EditorHost.hpp"
#include "gargantuan/editor/AnimationPreview.hpp"
#include "gargantuan/animation/AnimationTrack.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/services/AssetService.hpp"

#include <SDL3/SDL.h>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <vector>

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
		auto Call = [&](std::string Method, Json Parameters) {
			auto Result = Json::parse(Host.HandleRequest(Json{{"Version", 1}, {"RequestId", std::to_string(++Sequence)},
				{"SessionToken", "animation-preview-test"}, {"Method", Method}, {"Params", std::move(Parameters)}}.dump()));
			if (!Result["Ok"]) std::cout << "[Animation:EditorHost] " << Method << " " << Result["Error"].dump() << '\n';
			return Result;
		};
		Check(Call("CreateProject", {{"Destination", Root.generic_string()}, {"Name", "Animation Preview"}})["Ok"], "Create preview test project");
		std::filesystem::create_directory(Root / "assets");
		const auto FixturePath = ArgumentCount == 3 && std::string_view(Arguments[1]) == "--gpu"
			? std::filesystem::path(Arguments[2]) : std::filesystem::path(GARGANTUAN_ANIMATION_PREVIEW_FIXTURE);
		std::filesystem::copy_file(FixturePath, Root / "assets" / "rig.gltf");
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
		std::string PulseClip, LungeClip, StepClip;
		for (const auto &Entry : Rig["Result"]["Clips"]) {
			const auto Name = Entry["Name"].get<std::string>();
			if (Name == "BeaconPulse") PulseClip = Entry["Reference"].get<std::string>();
			if (Name == "BeaconLunge") LungeClip = Entry["Reference"].get<std::string>();
			if (Name == "BeaconStep") StepClip = Entry["Reference"].get<std::string>();
		}
		Check(!PulseClip.empty() && !LungeClip.empty() && !StepClip.empty(), "Preview fixture contains three distinct clips");
		std::ifstream FixtureInput(FixturePath, std::ios::binary);
		std::string IncompatibleSource(std::istreambuf_iterator<char>{FixtureInput}, {});
		const auto JointName = IncompatibleSource.find("BeaconTip");
		Check(FixtureInput.is_open() && JointName != std::string::npos, "Fixture can be copied with a different skeleton path");
		IncompatibleSource.replace(JointName, std::string("BeaconTip").size(), "BeaconFin");
		{
			std::ofstream OtherSource(Root / "assets" / "other-rig.gltf", std::ios::binary);
			OtherSource << IncompatibleSource;
			Check(static_cast<bool>(OtherSource), "Incompatible rig source is written");
		}
		Check(Call("ImportAsset", {{"Source", "assets/other-rig.gltf"}})["Ok"], "Import a distinct skeleton for negative preview checks");
		auto CatalogWithOther = Call("GetAssetCatalog", {{"IncludeBuiltIns", false}});
		std::string OtherMeshReference;
		for (const auto &Asset : CatalogWithOther["Result"]["Assets"])
			if (Asset["Kind"] == "Mesh" && Asset["Reference"] != Reference)
				OtherMeshReference = Asset["Reference"].get<std::string>();
		Check(!OtherMeshReference.empty(), "Distinct skeleton has a catalog Mesh");
		auto OtherRig = Call("GetAnimationRig", {{"Reference", OtherMeshReference}});
		Check(OtherRig["Ok"] && OtherRig["Result"]["SkeletonCompatibilityId"] != Rig["Result"]["SkeletonCompatibilityId"] &&
			!OtherRig["Result"]["Clips"].empty(), "Distinct rig produces incompatible canonical clips");
		const auto IncompatibleClip = OtherRig["Result"]["Clips"][0]["Reference"].get<std::string>();
		const auto State = Call("GetProjectState", Json::object())["Result"];
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
		Check(Call("GetProjectState", Json::object())["Result"] == State, "Preview controls never change authoring state/history");
		Check(Call("CloseAnimationPreview", {{"PreviewId", Id}})["Ok"], "Close releases preview");
		Check(!Call("ControlAnimationPreview", {{"PreviewId", Id}, {"Action", "Play"}})["Ok"], "Closed preview identity is stale");
		Check(Call("ReimportAsset", {{"Reference", Reference}})["Ok"], "Compatible reimport succeeds");
		auto Reimported = Call("GetAnimationRig", {{"Reference", Reference}});
		Check(Reimported["Ok"] && Reimported["Result"]["MeshReference"] == Reference &&
			Reimported["Result"]["SkeletonCompatibilityId"] == Rig["Result"]["SkeletonCompatibilityId"], "Compatible reimport preserves asset and skeleton identity");
		Check(Call("SaveProject", Json::object())["Ok"], "Save imported canonical assets");
		auto Rejects = [](auto &&Action) {
			try { Action(); return false; }
			catch (const std::exception &) { return true; }
		};
		std::shared_ptr<AnimationTrack> RetainedSecondary;
		Json StateAfterExplicitReimport;
		{
			DiskFilesystem Filesystem(Root);
			auto ProjectValue = Project::fromExisting(&Filesystem);
			auto SourceWorld = ProjectValue.DeserializeGame();
			auto Assets = std::dynamic_pointer_cast<AssetService>(SourceWorld->GetService("AssetService"));
			AnimationPreview Scene(Assets->CaptureProjectAssets(), Reference);
			Check(Rejects([&] { Scene.SelectClip(AnimationPreviewClipSlot::Secondary, LungeClip); }) &&
				Rejects([&] { Scene.SetBlendWeight(0.5f); }), "Secondary and blend controls require a primary pair");
			Scene.SelectClip(PulseClip);
			Check(Scene.GetTrack() == Scene.GetTrack(AnimationPreviewClipSlot::Primary) &&
				!Scene.GetTrack(AnimationPreviewClipSlot::Secondary) && Scene.GetBlendWeight() == 0.0f,
				"Existing single-clip API remains primary-only");
			Scene.SelectClip(AnimationPreviewClipSlot::Secondary, LungeClip);
			Check(Scene.GetBlendWeight() == 0.5f && Scene.GetTrack()->GetWeight() == 0.5f &&
				Scene.GetTrack(AnimationPreviewClipSlot::Secondary)->GetWeight() == 0.5f,
				"Two compatible clips start as a balanced visual blend");
			const auto OriginalPrimary = Scene.GetTrack();
			const auto OriginalSecondary = Scene.GetTrack(AnimationPreviewClipSlot::Secondary);
			Check(Rejects([&] { Scene.SelectClip(AnimationPreviewClipSlot::Secondary, "asset://00000000000000000000000000000000"); }) &&
				Rejects([&] { Scene.SelectClip(AnimationPreviewClipSlot::Secondary, IncompatibleClip); }) &&
				Rejects([&] { Scene.SelectClip(AnimationPreviewClipSlot::Primary, IncompatibleClip); }) &&
				Scene.GetTrack() == OriginalPrimary && Scene.GetTrack(AnimationPreviewClipSlot::Secondary) == OriginalSecondary &&
				Scene.GetBlendWeight() == 0.5f, "Missing or incompatible replacement retains the usable pair");
			OriginalPrimary->SetTimePosition(0.5f);
			OriginalSecondary->SetTimePosition(0.5f);
			Scene.SetBlendWeight(0.0f);
			const auto PrimaryPose = Scene.GetPose();
			Check(PrimaryPose && PrimaryPose->JointModelTransforms, "Primary endpoint produces a pose");
			const auto PrimaryRoot = glm::vec3(PrimaryPose->JointModelTransforms->at(0)[3]);
			Scene.SetBlendWeight(1.0f);
			const auto SecondaryPose = Scene.GetPose();
			Check(SecondaryPose && SecondaryPose->JointModelTransforms, "Secondary endpoint produces a pose");
			const auto SecondaryRoot = glm::vec3(SecondaryPose->JointModelTransforms->at(0)[3]);
			Scene.SetBlendWeight(0.5f);
			const auto MixedPose = Scene.GetPose();
			Check(MixedPose && MixedPose->JointModelTransforms &&
				glm::length(PrimaryRoot - SecondaryRoot) > 0.01f &&
				glm::length(glm::vec3(MixedPose->JointModelTransforms->at(0)[3]) -
					(PrimaryRoot + SecondaryRoot) * 0.5f) < 0.001f,
				"Blend endpoints and midpoint affect canonical joint-model pose");
			for (const auto BadWeight : {-0.01f, 1.01f, std::numeric_limits<float>::quiet_NaN()})
				Check(Rejects([&] { Scene.SetBlendWeight(BadWeight); }) && Scene.GetBlendWeight() == 0.5f,
					"Non-unit or non-finite weight leaves the blend unchanged");
			OriginalSecondary->SetSpeed(1.5f);
			OriginalSecondary->SetLooped(true);
			Scene.SelectClip(AnimationPreviewClipSlot::Secondary, StepClip);
			Check(Scene.GetTrack(AnimationPreviewClipSlot::Secondary)->GetSpeed() == 1.5f &&
				Scene.GetTrack(AnimationPreviewClipSlot::Secondary)->GetLooped() &&
				Scene.GetTrack(AnimationPreviewClipSlot::Secondary)->GetPlaybackState() == Enums::AnimationPlaybackState::Paused &&
				Scene.GetBlendWeight() == 0.5f, "Replacing one slot preserves its controls and the blend weight");
			std::weak_ptr<AnimationTrack> UnheldRetiredTrack;
			{
				const auto Current = Scene.GetTrack(AnimationPreviewClipSlot::Secondary);
				UnheldRetiredTrack = Current;
				Scene.SelectClip(AnimationPreviewClipSlot::Secondary, PulseClip);
			}
			Scene.SelectClip(AnimationPreviewClipSlot::Secondary, LungeClip);
			Check(UnheldRetiredTrack.expired(),
				"A stopped, unheld preview track is reaped after its final pose step");
			for (int Index = 0; Index < 24; ++Index)
				Scene.SelectClip(Index % 2 == 0 ? AnimationPreviewClipSlot::Primary : AnimationPreviewClipSlot::Secondary,
					Index % 3 == 0 ? PulseClip : Index % 3 == 1 ? LungeClip : StepClip);
			Check(Scene.GetTrack() && Scene.GetTrack(AnimationPreviewClipSlot::Secondary) && Scene.GetBlendWeight() == 0.5f,
				"Repeated replacements do not exhaust the sixteen-track Animator bound");
			const auto PrimaryBeforeReimport = Scene.GetTrack();
			const auto SecondaryBeforeReimport = Scene.GetTrack(AnimationPreviewClipSlot::Secondary);
			const auto ReimportDuringPreview = Call("ReimportAsset", {{"Reference", Reference}});
			Check(ReimportDuringPreview["Ok"] && ReimportDuringPreview["Result"]["OperationSucceeded"],
				"Source reimport succeeds while disposable preview owns an asset snapshot");
			Scene.Step(0.0f);
			Check(Scene.GetTrack() == PrimaryBeforeReimport &&
				Scene.GetTrack(AnimationPreviewClipSlot::Secondary) == SecondaryBeforeReimport &&
				Scene.GetPose().has_value() && Scene.GetBlendWeight() == 0.5f,
				"Active preview pair remains usable across source reimport");
			StateAfterExplicitReimport = Call("GetProjectState", Json::object())["Result"];
			std::vector<std::shared_ptr<AnimationTrack>> HeldOldTracks;
			bool CapacityRejected = false;
			for (int Index = 0; Index < 24; ++Index) {
				const auto PrimaryBefore = Scene.GetTrack();
				const auto SecondaryBefore = Scene.GetTrack(AnimationPreviewClipSlot::Secondary);
				HeldOldTracks.push_back(SecondaryBefore);
				try { Scene.SelectClip(AnimationPreviewClipSlot::Secondary, Index % 2 ? PulseClip : StepClip); }
				catch (const std::length_error &) {
					CapacityRejected = true;
					Check(Scene.GetTrack() == PrimaryBefore &&
						Scene.GetTrack(AnimationPreviewClipSlot::Secondary) == SecondaryBefore &&
						Scene.GetBlendWeight() == 0.5f, "Capacity rejection retains the previous pair and weight");
					break;
				}
			}
			Check(CapacityRejected, "Externally retained old tracks eventually reach the Animator bound");
			Scene.GetTrack()->SetRootMotionEnabled(true);
			Scene.GetTrack(AnimationPreviewClipSlot::Secondary)->SetRootMotionEnabled(true);
			Scene.Step(0.0f);
			Check(!Scene.GetTrack()->GetRootMotionEnabled() &&
				!Scene.GetTrack(AnimationPreviewClipSlot::Secondary)->GetRootMotionEnabled(),
				"Disposable preview never enables Character root motion");
			RetainedSecondary = Scene.GetTrack(AnimationPreviewClipSlot::Secondary);
			Scene.ClearSecondaryClip();
			Check(!Scene.GetTrack(AnimationPreviewClipSlot::Secondary) && Scene.GetBlendWeight() == 0.0f &&
				Scene.GetTrack()->GetWeight() == 1.0f &&
				RetainedSecondary->GetPlaybackState() == Enums::AnimationPlaybackState::Stopped &&
				Rejects([&] { Scene.SetBlendWeight(0.5f); }), "Clear restores single-clip state and stops the retired track");
			Scene.ClearSecondaryClip();
			SourceWorld->Destroy();
		}
		RetainedSecondary->Play();
		Check(RetainedSecondary->GetPlaybackState() == Enums::AnimationPlaybackState::Stopped,
			"Closing the preview invalidates retained secondary tracks");
		Check(Call("GetProjectState", Json::object())["Result"] == StateAfterExplicitReimport,
			"Two-clip preview controls and close do not mutate project state or history");
		auto Second = Call("OpenAnimationPreview", {{"Reference", Reference}});
		Check(Second["Ok"] && Second["Result"]["PreviewId"] != Id, "New preview receives fresh identity");
		Check(Call("OpenProject", {{"Root", Root.generic_string()}})["Ok"], "Reopen project");
		Check(!Call("ControlAnimationPreview", {{"PreviewId", Second["Result"]["PreviewId"]}, {"Action", "Play"}})["Ok"], "Project replacement retires preview");
		auto Reopened = Call("GetAnimationRig", {{"Reference", Reference}});
		Check(Reopened["Ok"] && Reopened["Result"]["Joints"] == Rig["Result"]["Joints"] && Reopened["Result"]["Clips"].size() == 3,
			"Save/reopen retains canonical rig and clip references");
		// Explicit manual GPU gate; ordinary CI remains genuinely headless.
		if ((ArgumentCount == 2 || ArgumentCount == 3) && std::string_view(Arguments[1]) == "--gpu") {
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
			const auto Camera = MakeLookAtRenderCameraInput({2, 2, -4}, {0, .5f, 0}, {0, 1, 0});
			Scene.GetTrack()->SetTimePosition(0);
			Scene.Step(0);
			auto First = Secondary->Capture(Scene.Publish(Camera, 320, 240));
			Scene.GetTrack()->SetTimePosition(.5f);
			Scene.Step(0);
			auto Second = Secondary->Capture(Scene.Publish(Camera, 320, 240));
			std::size_t Changed = 0;
			for (std::size_t Index = 0; Index < First.RgbPixels.size(); ++Index)
				if (First.RgbPixels[Index] != Second.RgbPixels[Index]) ++Changed;
			auto WriteFrame = [](const char *Path, const EditorViewportFrame &Frame) {
				std::ofstream Output(Path, std::ios::binary);
				Output << "P6\n" << Frame.Width << ' ' << Frame.Height << "\n255\n";
				Output.write(reinterpret_cast<const char *>(Frame.RgbPixels.data()), static_cast<std::streamsize>(Frame.RgbPixels.size()));
				if (!Output) throw std::runtime_error("Could not preserve native preview frame");
			};
			WriteFrame("animation-preview-0.ppm", First);
			WriteFrame("animation-preview-seek.ppm", Second);
			Check(Changed > 30, "Native GPU skinning changes rendered mesh pixels after seek");
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
