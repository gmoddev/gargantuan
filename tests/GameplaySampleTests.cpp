#include "gargantuan/Engine.hpp"
#include "gargantuan/animation/AnimationRuntime.hpp"
#include "gargantuan/audio/AudioRuntime.hpp"
#include "gargantuan/classes/Character.hpp"
#include "gargantuan/classes/Frame.hpp"
#include "gargantuan/classes/ImageButton.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/Player.hpp"
#include "gargantuan/classes/ScreenGui.hpp"
#include "gargantuan/classes/Sound.hpp"
#include "gargantuan/classes/ViewportFrame.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/network/GameSession.hpp"
#include "gargantuan/network/SimulatedTransport.hpp"
#if defined(GARGANTUAN_WITH_GNS)
#include "gargantuan/network/GameNetworkingSocketsTransport.hpp"
#endif
#include "gargantuan/packaging/PackageBuilder.hpp"
#include "gargantuan/platform/HostEvent.hpp"
#include "gargantuan/reflection/PreRunRegistration.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/Renderer.hpp"
#include "gargantuan/render/SDLRenderer.hpp"
#include "gargantuan/serialization/GasDocument.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/services/Players.hpp"
#include "gargantuan/services/Workspace.hpp"
#include "runtime/RuntimeWorkTrace.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
	using namespace gargantuan;
	using namespace gargantuan::network;
	using namespace std::chrono_literals;
	using Clock = std::chrono::steady_clock;

	void Require(bool Condition, std::string_view Message) {
		if (!Condition) throw std::runtime_error(std::string(Message));
	}

	bool AttributeEquals(const Instance &Object, std::string_view Name, const WireValue &Value) {
		const auto Actual = Object.GetAttributeValue(Name);
		return Actual && *Actual == Value;
	}

	bool NumberEquals(const Instance &Object, std::string_view Name, int Expected) {
		const auto Actual = Object.GetAttributeValue(Name);
		return Actual && (*Actual == WireValue(Expected) || *Actual == WireValue(static_cast<double>(Expected)));
	}

	void VerifyStatus(const GameSession &Session) {
		if (Session.GetStatus() == GameSessionStatus::Failed)
			throw std::runtime_error(Session.GetFailure());
	}

	class RelayGpuRenderer final : public BaseRenderer {
	  public:
		explicit RelayGpuRenderer(const Vector2 &Size)
			: Backend(Size, {.Offscreen = true, .WaitForGpuCompletion = true, .DebugDevice = false}) {}

		void Draw(RenderPublicationPtr Publication) override {
			const auto Began = Clock::now();
			Backend.Draw(Publication);
			const auto Metrics = Backend.GetMetrics();
			Require(Metrics.FramesSubmitted == Frames + 1 && Frames < Submission.size(),
				"relay offscreen GPU frame did not complete within the capture bounds");
			if (Frames != 0)
				Intervals[Frames - 1] = static_cast<std::uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(Began - PreviousStart).count());
			PreviousStart = Began;
			Submission[Frames] = Metrics.LastSubmissionNanoseconds;
			CompletionWait[Frames] = Metrics.LastGpuCompletionWaitNanoseconds;
			++Frames;
			Camera = Publication->Frame.Camera.Position;
			if (!WatchedDrone) return;
			for (const auto &Update : Publication->Updates) {
				if (Update.Object == *WatchedDrone && HasRenderUpdateDomain(Update.Domains, RenderUpdateDomain::Transform))
					++DroneTransforms;
			}
			DroneRetired = DroneRetired || std::ranges::any_of(Publication->Removes,
				[&](const auto &Remove) { return Remove.Object == *WatchedDrone; });
		}

		void Resize(int Width, int Height) override { Backend.Resize(Width, Height); }
		void Destroy() override { Backend.Destroy(); }
		[[nodiscard]] RendererCapabilities GetCapabilities() const override { return Backend.GetCapabilities(); }
		[[nodiscard]] RenderAnimationVisibilityFeedback GetAnimationVisibilityFeedback() const override {
			return Backend.GetAnimationVisibilityFeedback();
		}
		[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> GetViewportSize() const override {
			return Backend.GetViewportSize();
		}

		void WatchDrone(ObjectId Drone) { WatchedDrone = Drone; }
		[[nodiscard]] bool HasDroneMotion() const { return DroneTransforms > 1; }
		[[nodiscard]] bool HasDroneRetired() const { return DroneRetired; }
		[[nodiscard]] glm::vec3 GetCameraPosition() const { return Camera; }

		void Report(std::string_view Role) {
			const auto Metrics = Backend.GetMetrics();
			Require(Frames > 1 && Metrics.DrawCalls > 0 && Metrics.UiBatches > 0 && Metrics.PaletteUploads > 1 &&
				Metrics.CpuSkinnedVertexUploads == 0 && Metrics.MainShadowPoseMismatches == 0,
				"relay offscreen GPU journey requires rendered world, HUD and animated assets");
			std::cout << "[Gameplay:RelayGpu] target=offscreen peer=" << Role << " driver=" << Backend.GetDriverName()
				<< " frames=" << Frames << " draw_calls=" << Metrics.DrawCalls
				<< " hud_batches=" << Metrics.UiBatches << " palette_uploads=" << Metrics.PaletteUploads
				<< " drone_transforms=" << DroneTransforms << " drone_retired=" << DroneRetired;
			ReportTiming("interval", Intervals, Frames - 1);
			ReportTiming("submit", Submission, Frames);
			ReportTiming("fence_wait", CompletionWait, Frames);
			std::cout << '\n';
		}

	  private:
		static constexpr std::size_t MaximumFrames = 2'048;
		using FrameTimes = std::array<std::uint64_t, MaximumFrames>;

		static void ReportTiming(std::string_view Phase, FrameTimes &Values, std::size_t Count) {
			std::sort(Values.begin(), Values.begin() + Count);
			for (const std::size_t Percentile : {50, 95, 99, 100}) {
				const auto Rank = (Percentile * Count + 99) / 100;
				std::cout << ' ' << Phase << "_p" << Percentile << "_ns=" << Values[Rank - 1];
			}
		}

		SDLRenderer Backend;
		FrameTimes Intervals{};
		FrameTimes Submission{};
		FrameTimes CompletionWait{};
		Clock::time_point PreviousStart;
		std::size_t Frames = 0;
		std::optional<ObjectId> WatchedDrone;
		std::size_t DroneTransforms = 0;
		bool DroneRetired = false;
		glm::vec3 Camera{0.0f};
	};

	struct Client {
		std::unique_ptr<BaseRenderer> Renderer;
		std::unique_ptr<Engine> Runtime;
		std::unique_ptr<GameSession> Session;

		[[nodiscard]] RelayGpuRenderer &GetGpu() const { return dynamic_cast<RelayGpuRenderer &>(*Renderer); }

		void Stop() {
			if (Session) Session->Stop();
			if (Runtime) Runtime->Destroy();
			Session.reset();
			Runtime.reset();
		}
	};

	Gas::Document ReadRelayGasPlace(const std::filesystem::path &Path) {
		std::ifstream Input(Path, std::ios::binary | std::ios::ate);
		const std::streamoff Size = Input.tellg();
		Require(Size > 0 && Size <= static_cast<std::streamoff>(Gas::MaximumDocumentBytes),
			"Relay GASP fixture has a valid size");
		Gas::Binary Bytes(static_cast<std::size_t>(Size));
		Input.seekg(0);
		Require(static_cast<bool>(Input.read(reinterpret_cast<char *>(Bytes.data()), Size)),
			"Relay GASP fixture reads in full");
		auto Document = Gas::Decode(Bytes);
		if (!Document) throw std::runtime_error(Document.error().Format());
		return std::move(*Document);
	}

	void RunRelayRescue(bool UseGns, bool RenderGpu, bool EnableAudio, const std::filesystem::path &ProjectRoot,
		const std::optional<std::filesystem::path> &GasPlacePath) {
		auto Network = UseGns ? nullptr : SimulatedNetwork::Create({.BaseLatency = 6ms});
		const auto CreateTransport = [&]() -> std::shared_ptr<IGameTransport> {
			if (Network) return Network->CreateTransport();
#if defined(GARGANTUAN_WITH_GNS)
			return std::make_shared<GameNetworkingSocketsTransport>();
#else
			throw std::runtime_error("Relay Rescue native transport requires GNS");
#endif
		};

		DiskFilesystem Filesystem(ProjectRoot);
		auto Sample = GasPlacePath ? Project::FromGasOrigin(&Filesystem) : Project::fromExisting(&Filesystem);
		std::optional<Gas::Document> GasPlace;
		if (GasPlacePath) GasPlace = ReadRelayGasPlace(*GasPlacePath);
		const auto LoadWorld = [&] {
			return GasPlace ? Sample.LoadGasPlace(*GasPlace) : Sample.DeserializeGame();
		};
		auto TrustedWorld = LoadWorld();
		auto ServerWorld = LoadWorld();
		ServerWorld->InitializeLoadedProjectRevision();
		auto Assets = std::dynamic_pointer_cast<AssetService>(TrustedWorld->GetService("AssetService"));
		Require(Assets != nullptr, "sample asset catalog loads");
		Require(Assets->ResolveImage("asset://d18f6e730a55d41e65e2cc2447294884") &&
			Assets->ResolveAudio("asset://a0d10f78c368437daac002a4e59fdd64"),
			"the sample image and audio resolve from its own asset catalog");
		const auto AssetSnapshot = Assets->CaptureRuntimeAssets();
		std::size_t ScriptErrors = 0;
		const auto ReportError = [&](std::string Code, std::string Message) {
			if (Code != "Error") return;
			++ScriptErrors;
			std::cerr << "[Gameplay:Relay] " << Message << '\n';
		};
		HeadlessRenderer ServerRenderer(Vector2(320, 240));
		Engine ServerRuntime(ServerWorld, &ServerRenderer, ReportError,
			{.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer});
		const auto LegacyHud = std::make_shared<ScreenGui>();
		LegacyHud->SetName("LegacyReplicationHud");
		LegacyHud->SetParent(ServerRuntime.Workspace);
		const auto LegacyFrame = std::make_shared<Frame>();
		LegacyFrame->SetName("LegacyReplicationFrame");
		LegacyFrame->SetParent(LegacyHud);
		const auto LegacyViewport = std::make_shared<ViewportFrame>();
		LegacyViewport->SetName("LegacyHostViewport");
		LegacyViewport->SetParent(LegacyHud);
		const auto LocalGeometry = std::make_shared<Part>();
		LocalGeometry->SetName("LegacyViewportGeometry");
		LocalGeometry->SetParent(LegacyViewport);
		ServerRuntime.ProcessService->Alive = true;
		GameSessionConfiguration Configuration{
			.Role = GameSessionRole::Server,
			.Endpoint = {UseGns ? "127.0.0.1" : "relay-rescue", 37940},
			.Limits = GameSessionConfiguration::DefaultLimits(),
			.HandshakeTimeoutTicks = 180,
			.AllowInsecureDevelopmentNetwork = !UseGns,
		};
		std::unique_ptr<GameSession> Server;
		for (; Configuration.Endpoint.Port < 37956; ++Configuration.Endpoint.Port) {
			Server = std::make_unique<GameSession>(CreateTransport(), Configuration, &ServerRuntime);
			if (Server->Start().Succeeded()) break;
			Server.reset();
		}
		Require(Server != nullptr, "relay server accepts the session");
		Configuration.Role = GameSessionRole::Client;
		Configuration.HydratePlayerGuiCode = PackageBuilder::MakePlayerGuiCodeHydrator(TrustedWorld);
		std::array<Client, 2> Clients;
		for (std::size_t Index = 0; Index < Clients.size(); ++Index) {
			Configuration.ClientNonce = 0x52454c415900ull + Index;
			Clients[Index].Session = std::make_unique<GameSession>(CreateTransport(), Configuration);
			Require(Clients[Index].Session->Start().Succeeded(), "relay client connects");
		}

#if defined(GARGANTUAN_WORK_TRACE)
		runtime_detail::HostWorkTrace WorkTrace;
		auto PreviousFrame = Clock::now();
		std::uint64_t FrameIndex = 0;
#endif
		const auto Frame = [&] {
			std::this_thread::sleep_for(17ms);
#if defined(GARGANTUAN_WORK_TRACE)
			const auto FrameStarted = Clock::now();
			runtime_detail::HostWorkTrace::Frame TracedFrame(WorkTrace, FrameStarted, PreviousFrame, FrameIndex++);
			PreviousFrame = FrameStarted;
#endif
			if (Network) {
				(void)Network->Advance(17ms);
				Network->Pump();
			}
			(void)Server->Poll();
			ServerRuntime.Step();
			Server->Step(ServerRuntime.GetSimulationTick());
			VerifyStatus(*Server);
			for (auto &Peer : Clients) {
				if (!Peer.Session) continue;
				(void)Peer.Session->Poll();
				if (!Peer.Runtime) {
					if (const auto World = Peer.Session->GetClientDataModel()) {
						auto ClientAssets = std::dynamic_pointer_cast<AssetService>(World->GetService("AssetService"));
						Require(ClientAssets != nullptr, "replica asset service is available");
						ClientAssets->LoadRuntimeAssetSnapshot(AssetSnapshot);
						Require(PackageBuilder::HydrateClientCode(TrustedWorld, World) == 1,
							"replica hydrates the saved RelayClient script");
						if (RenderGpu) Peer.Renderer = std::make_unique<RelayGpuRenderer>(Vector2(320, 240));
						else Peer.Renderer = std::make_unique<HeadlessRenderer>(Vector2(320, 240));
						Peer.Runtime = std::make_unique<Engine>(World, Peer.Renderer.get(), ReportError,
								EngineProviderConfiguration{.AudioEnabled = EnableAudio, .Mode = RuntimeMode::NetworkClient});
						Peer.Runtime->ProcessService->Alive = true;
						Require(Peer.Session->AttachClientRuntime(*Peer.Runtime),
							"relay client attaches to its trusted Player");
					}
				}
				if (Peer.Runtime) Peer.Runtime->Step();
				Peer.Session->Step(ServerRuntime.GetSimulationTick());
				VerifyStatus(*Peer.Session);
			}
			Require(ScriptErrors == 0, "sample code runs without script activation errors");
		};

		const auto Until = [&](int Frames, std::string_view Message, const auto &Predicate) {
			for (int Index = 0; Index < Frames && !Predicate(); ++Index) Frame();
			Require(Predicate(), Message);
		};
		const auto SetKey = [](Client &Peer, PhysicalKey Physical, LogicalKey Logical, ButtonState State) {
			(void)Peer.Runtime->ProcessEvent(KeyEvent{
				.Device = {1}, .Physical = Physical, .Logical = Logical, .State = State,
			});
		};
		const auto WaitForRelease = [&] {
			for (int Index = 0; Index < 5; ++Index) Frame();
		};

		const auto GetPlayerGui = [](const Client &Peer) -> std::shared_ptr<Instance> {
			const auto LocalPlayer = Peer.Runtime->Players->GetLocalPlayer().value_or(nullptr);
			return LocalPlayer ? LocalPlayer->FindFirstChildOfClass("PlayerGui", false) : nullptr;
		};
		Until(180, "both peers hydrate stock Characters, their private HUD, and both animations", [&] {
			for (const auto &Peer : Clients) {
				if (!Peer.Runtime || Peer.Session->GetStatus() != GameSessionStatus::Ready ||
					Peer.Runtime->Animation->GetMetrics().ActiveTracks < 2)
					return false;
				auto PlayerValue = Peer.Runtime->Players->GetLocalPlayer().value_or(nullptr);
				if (!PlayerValue || !PlayerValue->GetCharacter() || !NumberEquals(*PlayerValue, "Health", 100) ||
					!GetPlayerGui(Peer))
					return false;
			}
			return Server->GetMetrics().ReadyPeers == 2;
		});
		const auto PilotGui = GetPlayerGui(Clients.front());
		const auto ObserverGui = GetPlayerGui(Clients.back());
		const auto PilotHud = PilotGui->FindFirstChild("RelayHud", false);
		const auto ObserverHud = ObserverGui->FindFirstChild("RelayHud", false);
		Require(PilotHud && ObserverHud && PilotHud->GetObjectId() != ObserverHud->GetObjectId(),
			"each player gets an independent clone from StarterGui");
		for (const auto &Peer : Clients) {
			const auto LegacyReplica = Peer.Runtime->Workspace->FindFirstChild("LegacyReplicationHud", false);
			Require(LegacyReplica && LegacyReplica->FindFirstChild("LegacyReplicationFrame", false) &&
				!LegacyReplica->FindFirstChild("LegacyHostViewport", false) &&
				!Peer.Runtime->Workspace->FindFirstDescendant("LegacyViewportGeometry"),
				"legacy screen content replicates while its host-local viewport scene stays outside GRPL");
		}
		const auto PilotToggle = std::dynamic_pointer_cast<ImageButton>(PilotHud->FindFirstChild("RelayMixToggle", false));
		const auto ObserverToggle = std::dynamic_pointer_cast<ImageButton>(ObserverHud->FindFirstChild("RelayMixToggle", false));
		Require(PilotToggle && ObserverToggle && PilotToggle->GetScaleType() == Enums::GuiImageScaleType::Fit &&
			ObserverToggle->GetScaleType() == Enums::GuiImageScaleType::Fit,
			"both cloned HUDs retain the authored asset-backed fit ImageButton");
		const auto ClickPilot = [&](float X) {
			for (const auto State : {ButtonState::Pressed, ButtonState::Released})
				(void)Clients.front().Runtime->ProcessEvent(PointerButtonEvent{
					.Device = {2}, .Button = PointerButton::Left, .State = State, .Position = {X, 98},
				});
		};
		ClickPilot(280);
		Require(PilotToggle->GetAccessibilitySelected() && !ObserverToggle->GetAccessibilitySelected(),
			"routed ImageButton activation changes only its client's audio mute");
		ClickPilot(280);
		Require(!PilotToggle->GetAccessibilitySelected() && !ObserverToggle->GetAccessibilitySelected(),
			"the second activation restores the pilot's audio independently");
		const auto PilotEquipment = std::dynamic_pointer_cast<GuiObject>(PilotHud->FindFirstChild("EquipmentPreview", false));
		const auto ObserverEquipment = std::dynamic_pointer_cast<GuiObject>(ObserverHud->FindFirstChild("EquipmentPreview", false));
		Require(PilotEquipment && ObserverEquipment && PilotEquipment->GetVisible() && ObserverEquipment->GetVisible(),
			"both players begin with an authored equipment viewport");
		for (const auto &Equipment : {PilotEquipment, ObserverEquipment}) {
			const auto Camera = Equipment->FindFirstChild("EquipmentCamera", false);
			Require(Camera && Equipment->ReadPropertyWireValue("CurrentCamera") ==
				WireValue(WireObjectReference{WireObjectId::FromObjectId(Camera->GetObjectId())}),
				"cloning and replication remap each camera reference to the same HUD subtree");
		}
		ClickPilot(225);
		Require(!PilotEquipment->GetVisible() && ObserverEquipment->GetVisible(), "equipment selection is client-local");
		ClickPilot(225);
		Require(PilotEquipment->GetVisible(), "equipment preview can be reopened through the same HUD");
		for (const auto &Peer : Clients) {
			const auto Cue = std::dynamic_pointer_cast<Sound>(Peer.Runtime->Workspace->FindFirstChild("RelayChargeTone", true));
			Require(Cue && Cue->GetMixCategory() == Enums::SoundMixCategory::Effects &&
				Cue->GetParent().value_or(nullptr) == Peer.Runtime->Workspace->FindFirstChild("RelayBeacon", true),
				"each client attaches its local charge cue to the streamed beacon");
		}
		const auto FindServerPlayer = [&](const Client &Peer) -> std::shared_ptr<Player> {
			const auto Id = (*Peer.Runtime->Players->GetLocalPlayer())->GetPlayerId();
			for (const auto &Value : ServerRuntime.Players->GetPlayers())
				if (Value->GetPlayerId() == Id) return Value;
			return nullptr;
		};
		auto &PilotClient = Clients.front();
		const auto Pilot = FindServerPlayer(PilotClient);
		const auto SwitchOperator = FindServerPlayer(Clients.back());
		Require(Pilot && SwitchOperator && Pilot != SwitchOperator && Pilot->GetCharacter() &&
				SwitchOperator->GetCharacter(), "stock Characters belong to distinct server Players");
		const auto ServerPilotGui = Pilot->FindFirstChildOfClass("PlayerGui", false);
		Require(ServerPilotGui != nullptr, "the server owns the pilot's GUI lifecycle");
		for (const auto &Peer : Clients) {
			const auto Local = (*Peer.Runtime->Players->GetLocalPlayer())->GetPlayerId();
			for (const auto &Replica : Peer.Runtime->Players->GetPlayers())
				if (Replica->GetPlayerId() != Local)
					Require(!Replica->FindFirstChildOfClass("PlayerGui", false), "another player's GUI is absent from the local graph");
		}
		const auto OldPilotCharacter = *Pilot->GetCharacter();
		const auto Course = ServerRuntime.Workspace->FindFirstChild("RelayCourse", std::nullopt);
		Require(Course != nullptr, "saved RelayCourse is present");
		if (RenderGpu) {
			const glm::vec2 Start(OldPilotCharacter->GetPosition().x, OldPilotCharacter->GetPosition().z);
			SetKey(PilotClient, PhysicalKey::D, LogicalKey::D, ButtonState::Pressed);
			Until(90, "host input moves the authoritative stock Character during the GPU journey", [&] {
				const auto Position = OldPilotCharacter->GetPosition();
				return glm::distance(Start, glm::vec2(Position.x, Position.z)) > 0.5f;
			});
			SetKey(PilotClient, PhysicalKey::D, LogicalKey::D, ButtonState::Released);
			WaitForRelease();
		}
		const auto PartAt = [&](std::string Name) {
			auto Value = std::dynamic_pointer_cast<Part>(Course->FindFirstChild(std::move(Name), std::nullopt));
			Require(Value != nullptr, "saved station is a Part");
			return Value->GetPosition() + glm::vec3(0, 2.0f, 0);
		};
		const auto PlaceAt = [&](const std::shared_ptr<Player> &Value, std::string Name) {
			const auto CharacterValue = Value->GetCharacter();
			Require(CharacterValue.has_value(), "placement requires a stock Character");
			(*CharacterValue)->SetCFrame(CFrame(PartAt(std::move(Name))));
		};
		PlaceAt(SwitchOperator, "GroundSwitch");
		PlaceAt(Pilot, "LaunchPad");
		// Only fixture positioning above bypasses local input. All flight and
		// landing below enter through host events and the saved RelayClient.
		const auto FlightReady = [&] {
			const auto LocalPilot = *PilotClient.Runtime->Players->GetLocalPlayer();
			return Pilot->GetPossessedEntity() && LocalPilot->GetPossessedEntity() &&
				!Pilot->GetCharacter() && OldPilotCharacter->GetDestroyed() &&
				SwitchOperator->GetCharacter() && AttributeEquals(*Course, "RelayEnabled", WireValue(true));
		};
		for (int Index = 0; Index < 90 && !FlightReady(); ++Index) Frame();
		if (!FlightReady()) {
			const auto LocalPilot = *PilotClient.Runtime->Players->GetLocalPlayer();
			const auto PilotPosition = Pilot->GetCharacter() ? (*Pilot->GetCharacter())->GetPosition() : glm::vec3{};
			const auto SwitchPosition = SwitchOperator->GetCharacter()
				? (*SwitchOperator->GetCharacter())->GetPosition() : glm::vec3{};
			std::cerr << "[Gameplay:Relay] launch server_possessed=" << bool(Pilot->GetPossessedEntity())
				<< " client_possessed=" << bool(LocalPilot->GetPossessedEntity())
				<< " stock_retired=" << OldPilotCharacter->GetDestroyed()
				<< " switch=" << AttributeEquals(*Course, "RelayEnabled", WireValue(true))
				<< " multiplayer=" << AttributeEquals(*Course, "RelayMultiplayer", WireValue(true))
				<< " pilot_pos=" << PilotPosition.x << ',' << PilotPosition.y << ',' << PilotPosition.z
				<< " switch_pos=" << SwitchPosition.x << ',' << SwitchPosition.y << ',' << SwitchPosition.z
				<< " leases=" << Server->GetMetrics().PossessionInput.Bindings << '\n';
		}
		Require(FlightReady(), "pilot enters the sample drone and receives its replica");
		const auto Drone = std::dynamic_pointer_cast<Part>(*Pilot->GetPossessedEntity());
		Require(Drone && Drone->GetAnchored(), "sample uses an independently possessed anchored drone");
		glm::vec3 LaunchCamera{0.0f};
		if (RenderGpu) {
			const auto LocalPilot = *PilotClient.Runtime->Players->GetLocalPlayer();
			PilotClient.GetGpu().WatchDrone((*LocalPilot->GetPossessedEntity())->GetObjectId());
			Until(60, "observer receives the drone before measuring its render updates", [&] {
				const auto Replica = Clients.back().Runtime->Workspace->FindFirstChild(Drone->GetName(), true);
				if (!Replica) return false;
				Clients.back().GetGpu().WatchDrone(Replica->GetObjectId());
				return true;
			});
			LaunchCamera = PilotClient.GetGpu().GetCameraPosition();
		}
		const auto Drive = [&](PhysicalKey Physical, LogicalKey Logical, int Frames, const auto &Predicate) {
			SetKey(PilotClient, Physical, Logical, ButtonState::Pressed);
			Until(Frames, "pilot intent failed to reach its terrain or relay target", Predicate);
			SetKey(PilotClient, Physical, Logical, ButtonState::Released);
			WaitForRelease();
		};
		Drive(PhysicalKey::D, LogicalKey::D, 100, [&] { return Drone->GetPosition().x >= -0.7f; });
		SetKey(PilotClient, PhysicalKey::W, LogicalKey::W, ButtonState::Pressed);
		Until(60, "northbound drone reaches the divider", [&] { return Drone->GetPosition().z <= -8.0f; });
		for (int Index = 0; Index < 12; ++Index) Frame();
		Require(Drone->GetPosition().z >= -8.25f, "the saved drone controller casts against the divider");
		SetKey(PilotClient, PhysicalKey::W, LogicalKey::W, ButtonState::Released);
		WaitForRelease();
		Drive(PhysicalKey::D, LogicalKey::D, 100, [&] { return Drone->GetPosition().x >= 6.5f; });
		for (int Charge = 1; Charge <= 3; ++Charge) {
			if (Charge > 1)
				Drive(PhysicalKey::S, LogicalKey::S, 50, [&] { return Drone->GetPosition().z >= -12.65f; });
			Drive(PhysicalKey::W, LogicalKey::W, 80, [&] { return NumberEquals(*Course, "RelayCharges", Charge); });
			Require(NumberEquals(*Course, "RelayCharges", Charge), "one charge is awarded per visit");
		}
		const auto Inventory = Pilot->FindFirstChild("RelayInventory", std::nullopt);
		Require(Inventory && Inventory->GetChildren().size() == 3 && NumberEquals(*Pilot, "RelayCharges", 3) &&
			NumberEquals(*SwitchOperator, "RelayCharges", 0), "only the pilot owns each earned cell");
		for (const auto &Cell : Inventory->GetChildren())
			Require(AttributeEquals(*Cell, "Kind", WireValue(std::string("RelayCell"))), "a charge creates a relay cell");
		const auto Dock = std::dynamic_pointer_cast<Part>(Course->FindFirstChild("CargoDock", std::nullopt));
		const auto Marker = std::dynamic_pointer_cast<Part>(Course->FindFirstChild("CargoMarker", std::nullopt));
		Require(Dock && Marker && Dock->GetFriction() == 1.5f && Dock->GetRestitution() == 0.75f &&
			!Marker->GetAnchored() && Marker->GetPosition().y < 4.0f,
			"the authoritative mission releases dynamic cargo onto the authored contact surface");
		Until(60, "the dynamic cargo rebounds from the dock", [&] {
			return Marker->GetLinearVelocity().y > 0.5f;
		});
		Until(60, "other client receives authoritative mission completion", [&] {
			const auto &World = Clients.back().Runtime->Workspace;
			const auto Replica = World->FindFirstChild("RelayCourse", std::nullopt);
			return Replica && NumberEquals(*Replica, "RelayCharges", 3);
		});
		if (EnableAudio) {
			for (const auto &Peer : Clients) {
				const auto Audio = Peer.Runtime->Audio->GetMetrics();
				Require(Peer.Runtime->Audio->IsAvailable() && Audio.VoiceAdmissions > 0 &&
					Audio.MixedFrames > 0 && Audio.Backend.SubmittedFrames > 0,
					"each client submits its asset-backed cue to the audio backend");
			}
		}
		if (RenderGpu) {
			Require(PilotClient.GetGpu().HasDroneMotion() && Clients.back().GetGpu().HasDroneMotion(),
				"both GPU clients consumed moving drone transforms");
			Require(glm::distance(LaunchCamera, PilotClient.GetGpu().GetCameraPosition()) > 2.0f,
				"the published camera followed the flown drone");
		}

		SetKey(PilotClient, PhysicalKey::Space, LogicalKey::Space, ButtonState::Pressed);
		Until(90, "landing intent returns the pilot to the stock controller", [&] {
			return Drone->GetDestroyed() && !Pilot->GetPossessedEntity() && Pilot->GetCharacter();
		});
		SetKey(PilotClient, PhysicalKey::Space, LogicalKey::Space, ButtonState::Released);
		if (RenderGpu) {
			Until(60, "landing retires the old drone from both GPU views", [&] {
				return PilotClient.GetGpu().HasDroneRetired() && Clients.back().GetGpu().HasDroneRetired();
			});
		}
		const auto BeforeDeath = *Pilot->GetCharacter();
		Require(glm::length(LocalGeometry->GetPosition()) < 0.01f,
			"host-local viewport geometry beneath Workspace never enters gameplay physics");
		const auto BeforeBriefing = PilotGui->FindFirstChild("RelayBriefing", false);
		Require(BeforeBriefing != nullptr, "the pilot retains a spawn-reset briefing before death");
		PlaceAt(Pilot, "Hazard");
		Until(120, "server damage retires the stock Character", [&] {
			return BeforeDeath->GetDestroyed() && !Pilot->GetCharacter() && NumberEquals(*Pilot, "Health", 0);
		});
		const auto DeathTime = Clock::now();
		const auto DeathTick = ServerRuntime.GetSimulationTick();
		Until(180, "sample respawns the same Player with a new Character and full health", [&] {
			return Pilot->GetCharacter() && *Pilot->GetCharacter() != BeforeDeath && NumberEquals(*Pilot, "Health", 100);
		});
		Until(60, "respawn replaces the briefing and keeps the player's active HUD", [&] {
			const auto Briefing = PilotGui->FindFirstChild("RelayBriefing", false);
			return Briefing && Briefing != BeforeBriefing && BeforeBriefing->GetDestroyed() &&
				PilotGui->FindFirstChild("RelayHud", false) == PilotHud;
		});
		const auto RespawnMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - DeathTime);
		std::cout << "[Gameplay:Relay] transport=" << (UseGns ? "gns-loopback" : "simulated")
			<< " respawn_wall_ms=" << RespawnMilliseconds.count()
			<< " respawn_server_ticks=" << ServerRuntime.GetSimulationTick() - DeathTick << '\n';

		PlaceAt(Pilot, "LaunchPad");
		Until(75, "respawned pilot can launch a new drone", [&] { return Pilot->GetPossessedEntity().has_value(); });
		const auto DisconnectDrone = *Pilot->GetPossessedEntity();
		if (RenderGpu) PilotClient.GetGpu().Report("pilot");
		PilotClient.Stop();
		Until(150, "disconnect retires pilot, drone, and its possession while another player stays", [&] {
			return Pilot->GetDestroyed() && DisconnectDrone->GetDestroyed() && !Pilot->GetPossessedEntity() &&
				ServerRuntime.Players->GetPlayers() == std::vector<std::shared_ptr<Player>>{SwitchOperator};
		});
		Require(Inventory->GetDestroyed(), "disconnect also retires player-owned inventory");
		Require(ServerPilotGui->GetDestroyed(), "disconnect retires the server-side private GUI");

		if (RenderGpu) Clients.back().GetGpu().Report("observer");
		Clients.back().Stop();
		Server->Stop();
		ServerRuntime.Destroy();
		TrustedWorld->Destroy();
#if defined(GARGANTUAN_WORK_TRACE)
		WorkTrace.Write();
#endif
	}
}

int main(int ArgumentCount, char **Arguments) {
	try {
		bool UseGns = false;
		bool RenderGpu = false;
		bool EnableAudio = false;
		std::filesystem::path ProjectRoot = GARGANTUAN_GAMEPLAY_SAMPLE_ROOT;
		std::optional<std::filesystem::path> GasPlacePath;
		for (int Index = 1; Index < ArgumentCount; ++Index) {
			const std::string_view Argument(Arguments[Index]);
			if (Argument == "--gns" || Argument == "--gns-gpu") {
				UseGns = true;
				RenderGpu = RenderGpu || Argument == "--gns-gpu";
			} else if (Argument == "--audio")
				EnableAudio = true;
			else if (Argument == "--project" && Index + 1 < ArgumentCount)
				ProjectRoot = std::filesystem::absolute(Arguments[++Index]);
			else if (Argument == "--gasp" && Index + 1 < ArgumentCount)
				GasPlacePath = std::filesystem::absolute(Arguments[++Index]);
			else throw std::invalid_argument("Use --gns, --gns-gpu, --audio, --project <root>, or --gasp <file>");
		}
		struct SdlLifetime {
			bool Active;
			~SdlLifetime() { if (Active) SDL_Quit(); }
		} Sdl{RenderGpu && SDL_Init(SDL_INIT_VIDEO)};
		if (RenderGpu && !Sdl.Active)
			throw std::runtime_error(std::string("relay GPU video startup failed: ") + SDL_GetError());
		if (GasPlacePath)
			gargantuan::BootstrapPackagedRuntimeSchema(gargantuan::ReadProjectPreRunSource(ProjectRoot));
		else
			gargantuan::BootstrapNativeRuntimeSchema();
		RunRelayRescue(UseGns, RenderGpu, EnableAudio, ProjectRoot, GasPlacePath);
		std::cout << "[Gameplay:Relay] Saved two-player mission passed\n";
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Gameplay:Relay] " << Error.what() << '\n';
		return 1;
	}
}
