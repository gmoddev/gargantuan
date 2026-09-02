#include "gargantuan/classes/Attachment.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/WorldRoot.hpp"
#include "gargantuan/network/CharacterNetwork.hpp"
#include "gargantuan/network/SimulatedTransport.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"

#include <Luau/Compiler.h>
#include <lua.h>
#include <luacode.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace {
	using namespace gargantuan;
	using namespace gargantuan::network;
	using namespace std::chrono_literals;

	int Failures = 0;

	void Check(bool Condition, const char *Message) {
		if (Condition) return;
		std::cerr << "FAIL: " << Message << '\n';
		++Failures;
	}

	bool Near(const glm::vec3 &A, const glm::vec3 &B, float Epsilon = 0.01f) {
		return glm::length(A - B) <= Epsilon;
	}

	NetworkLimits TestLimits() {
		return {
			.MaximumReliableMessageBytes = 4096,
			.MaximumUnreliableMessageBytes = 1200,
			.MaximumQueuedReliableBytes = 1024 * 1024,
			.MaximumInFlightRemoteRequests = 16,
			.MaximumDecodedMessageBytes = 4096,
			.MaximumSendBytesPerTick = 1024 * 1024,
			.MaximumReceiveBytesPerTick = 1024 * 1024,
			.MaximumMessagesPerTick = 1024,
		};
	}

	AssetContentId Content(std::uint8_t Seed) {
		std::array<std::uint8_t, 4> Bytes{Seed, static_cast<std::uint8_t>(Seed + 1), 17, 93};
		return AssetContentId::Hash(Bytes);
	}

	CharacterActionDefinition Action(std::uint32_t Token, std::uint8_t Revision, float MetresPerTick = 2.0f) {
		return {
			.Token = Token,
			.Animation = AssetId::FromBuiltInName("CharacterNetworkAction" + std::to_string(Token)),
			.ContentRevision = Content(Revision),
			.DurationTicks = 4,
			.EvaluateRootMotion =
				[MetresPerTick](std::uint64_t FromTick, std::uint64_t ToTick) -> std::optional<RootMotionDelta> {
				if (ToTick <= FromTick) return std::nullopt;
				return RootMotionDelta{
					.Translation = {MetresPerTick * static_cast<float>(ToTick - FromTick), 0.0f, 0.0f},
					.IntervalStart = static_cast<double>(FromTick),
					.IntervalEnd = static_cast<double>(ToTick),
				};
			},
		};
	}

	std::optional<CharacterActionDefinition> FirstCompleteGameAction() {
		std::ifstream Stream(
			std::filesystem::path(GARGANTUAN_FIRST_COMPLETE_GAME_ROOT) / ".gargantuan" / "assets" / "catalog.json",
			std::ios::binary
		);
		if (!Stream) return std::nullopt;
		nlohmann::json Catalog;
		Stream >> Catalog;
		for (const auto &Asset : Catalog.value("Assets", nlohmann::json::array())) {
			if (Asset.value("Name", "") != "BeaconLunge" || Asset.value("Kind", "") != "Animation" ||
				Asset.value("State", "") != "Ready")
				continue;
			auto Animation = AssetId::Parse(Asset.value("AssetId", ""));
			auto Revision = AssetContentId::Parse(Asset.value("ContentId", ""));
			if (!Animation || !Revision) return std::nullopt;
			return CharacterActionDefinition{
				.Token = 3,
				.Animation = *Animation,
				.ContentRevision = *Revision,
				.DurationTicks = 4,
				.EvaluateRootMotion = [](std::uint64_t FromTick,
										 std::uint64_t ToTick) -> std::optional<RootMotionDelta> {
					if (ToTick <= FromTick) return std::nullopt;
					return RootMotionDelta{.Translation = {0.125f * static_cast<float>(ToTick - FromTick), 0.0f, 0.0f}};
				}
			};
		}
		return std::nullopt;
	}

	CharacterMotionRequest Movement(const CharacterInputCommand &Command, const KinematicCharacter &) {
		const auto Speed = 6.0f;
		return {
			.Translation =
				{Command.MoveIntent.x * Speed * Command.DeltaSeconds,
				 0.0f,
				 Command.MoveIntent.y * Speed * Command.DeltaSeconds},
			.Velocity = {Command.MoveIntent.x * Speed, 0.0f, Command.MoveIntent.y * Speed},
			.YawRadians = Command.FacingYawRadians,
			.Source = CharacterMotionSource::Script,
		};
	}

	std::vector<TransportEvent> Drain(const std::shared_ptr<SimulatedTransport> &Transport) {
		std::vector<TransportEvent> Result;
		std::array<TransportEvent, 256> Buffer;
		for (;;) {
			const auto Count = Transport->PollEvents(Buffer);
			if (Count == 0) break;
			for (std::size_t Index = 0; Index < Count; ++Index)
				Result.push_back(std::move(Buffer[Index]));
		}
		return Result;
	}

	ConnectionId ConnectedId(const std::vector<TransportEvent> &Events) {
		for (const auto &Event : Events) {
			const auto *State = std::get_if<ConnectionStateEvent>(&Event);
			if (State && State->Current == ConnectionState::Connected) return State->Connection;
		}
		return {};
	}

	int RunLua(ScriptEngine &Engine, std::string_view Source) {
		size_t BytecodeSize = 0;
		char *Bytecode = luau_compile(Source.data(), Source.size(), &Engine.CompileOptions, &BytecodeSize);
		if (!Bytecode) return LUA_ERRSYNTAX;
		const int Loaded = luau_load(Engine.L, "character-network-policy", Bytecode, BytecodeSize, 0);
		std::free(Bytecode);
		if (Loaded != LUA_OK) return Loaded;
		return lua_pcall(Engine.L, 0, 0, 0);
	}

	struct RecordingScheduler final : INetworkScheduler {
		std::vector<NetworkMessageIntent> Messages;
		bool RegisterConnection(ConnectionId, const NetworkLimits &) override {
			return true;
		}
		SchedulerSubmitResult Submit(NetworkMessageIntent Intent) override {
			Messages.push_back(std::move(Intent));
			return {SchedulerSubmitStatus::Accepted};
		}
		SchedulerFlushResult Flush(ConnectionId, SchedulerTickBudget) override {
			return {SchedulerFlushStatus::Drained};
		}
		bool CancelConnection(ConnectionId) override {
			return true;
		}
		std::optional<SchedulerStatistics> GetStatistics(ConnectionId) const override {
			return std::nullopt;
		}
	};

	ReceivedMessageEvent
	StateFrameEvent(ConnectionId Connection, StateChannelId Channel, const CharacterStateFrame &Frame) {
		auto Bytes = EncodeCharacterMessage(CharacterMessage(Frame));
		return {
			Connection,
			DeliveryMode::UnreliableSequenced,
			TrafficClass::RealtimeState,
			RealtimeStateOrder{Channel, RealtimeStateSequence(Frame.FrameSequence.Value())},
			Bytes ? std::move(*Bytes) : std::vector<std::byte>{},
		};
	}

	void TestCodec() {
		const ObjectId Character{99, 7};
		const auto Definition = Action(12, 3);
		std::array<CharacterMessage, 5> Messages{
			CharacterControlTransition{Character, CharacterControlEpoch(2), StateChannelId(9), 40, true},
			CharacterControlTransition{Character, CharacterControlEpoch(2), {}, 41, false},
			CharacterInputCommand{
				Character,
				CharacterControlEpoch(2),
				CharacterInputSequence(3),
				42,
				1.0f / 60.0f,
				{0.5f, -0.5f},
				0.25f,
				static_cast<std::uint8_t>(CharacterInputFlag::JumpRequested)
			},
			CharacterActionRequest{
				Character, CharacterControlEpoch(2), CharacterActionSequence(4), CharacterInputSequence(3), 12
			},
			CharacterAuthoritativeState{
				.Character = Character,
				.ControlEpoch = CharacterControlEpoch(2),
				.StateSequence = RealtimeStateSequence(5),
				.AcknowledgedInput = CharacterInputSequence(3),
				.ResolvedAction = CharacterActionSequence(4),
				.AuthoritativeTick = 43,
				.Transform = CFrame(1.0f, 2.0f, 3.0f),
				.Velocity = {4.0f, 5.0f, 6.0f},
				.FloorNormal = {0.0f, 1.0f, 0.0f},
				.Flags = static_cast<std::uint8_t>(CharacterStateFlag::Grounded),
				.ActiveAction = CharacterActionState{
					CharacterActionSequence(4), 12, Definition.Animation, Definition.ContentRevision, 40, 4
				},
			},
		};
		for (const auto &Message : Messages) {
			auto Encoded = EncodeCharacterMessage(Message);
			Check(
				Encoded.has_value() && Encoded->size() <= MaximumCharacterFrameBytes,
				"every Character opcode encodes within the fixed frame ceiling"
			);
			if (!Encoded) continue;
			auto Decoded = DecodeCharacterMessage(*Encoded);
			Check(
				Decoded.has_value() && GetCharacterMessageKind(*Decoded) == GetCharacterMessageKind(Message),
				"every Character opcode round trips through the independent codec"
			);
			for (std::size_t Boundary = 0; Boundary < Encoded->size(); ++Boundary)
				Check(
					!DecodeCharacterMessage(std::span<const std::byte>(*Encoded).first(Boundary)),
					"every truncated Character frame boundary fails closed"
				);
			auto Trailing = *Encoded;
			Trailing.push_back(std::byte{0});
			Check(!DecodeCharacterMessage(Trailing), "Character frames reject trailing bytes");
		}

		auto Input = EncodeCharacterMessage(Messages[2]);
		Check(Input.has_value(), "malformed corpus input seed encodes");
		if (Input) {
			auto WrongMagic = *Input;
			WrongMagic[0] = std::byte{0};
			Check(!DecodeCharacterMessage(WrongMagic), "wrong Character magic fails closed");
			auto WrongVersion = *Input;
			WrongVersion[4] = std::byte{9};
			Check(!DecodeCharacterMessage(WrongVersion), "unknown Character version fails closed");
			auto WrongOpcode = *Input;
			WrongOpcode[6] = std::byte{0xff};
			Check(!DecodeCharacterMessage(WrongOpcode), "unknown Character opcode fails closed");
			auto BadReserved = *Input;
			BadReserved[7] = std::byte{1};
			Check(!DecodeCharacterMessage(BadReserved), "nonzero Character reserved bytes fail closed");
			auto ZeroObject = *Input;
			for (std::size_t Index = 8; Index < 16; ++Index)
				ZeroObject[Index] = std::byte{0};
			Check(!DecodeCharacterMessage(ZeroObject), "zero Character ObjectId fails closed");
			auto Nan = *Input;
			for (std::size_t Index = 40; Index < 44; ++Index)
				Nan[Index] = std::byte{0xff};
			Check(!DecodeCharacterMessage(Nan), "non-finite Character command fields fail closed");
		}
		std::vector<std::byte> Oversized(MaximumCharacterFrameBytes + 1);
		Check(!DecodeCharacterMessage(Oversized), "oversized Character frame fails before parsing");
		CharacterInputCommand SpeedHack{
			Character, CharacterControlEpoch(2), CharacterInputSequence(9), 50, 1.0f / 60.0f, {1.01f, 1.01f}, 0.0f, 0
		};
		Check(
			!SpeedHack.IsValid() && !EncodeCharacterMessage(CharacterMessage(SpeedHack)),
			"client displacement and over-length movement intent cannot enter the Character protocol"
		);

		CharacterStateFrame Frame{
			.ServerTick = 60,
			.FrameSequence = CharacterStateFrameSequence(7),
			.StateCount = 2,
		};
		Frame.States[0] = {
			.Character = ObjectId{100, 1},
			.ControlEpoch = CharacterControlEpoch(2),
			.StateSequence = RealtimeStateSequence(10),
			.AcknowledgedInput = CharacterInputSequence(8),
			.AuthoritativeTick = 60,
			.Transform = CFrame::fromEulerAnglesYXZ(0.1f, 0.2f, -0.1f),
			.Velocity = {4.125f, -2.25f, 0.0f},
			.FloorNormal = {0.0f, 1.0f, 0.0f},
			.Flags = static_cast<std::uint8_t>(CharacterStateFlag::Grounded),
		};
		Frame.States[0].Transform.Position = {1000000.25f, 2.0f, -1000000.5f};
		Frame.States[1] = Frame.States[0];
		Frame.States[1].Character = ObjectId{101, 1};
		Frame.States[1].StateSequence = RealtimeStateSequence(11);
		Frame.States[1].Transform.Position = {-3.0f, 4.0f, 5.0f};
		auto FrameBytes = EncodeCharacterMessage(CharacterMessage(Frame));
		Check(
			FrameBytes && FrameBytes->size() == CharacterStateFrameHeaderBytes + 2 * CompactCharacterStateBytes,
			"GCHR v3 batches two compact absolute states with an exact ABI-independent size"
		);
		if (FrameBytes) {
			auto Decoded = DecodeCharacterMessage(*FrameBytes);
			auto *DecodedFrame = Decoded ? std::get_if<CharacterStateFrame>(&*Decoded) : nullptr;
			Check(
				DecodedFrame && DecodedFrame->StateCount == 2 &&
					Near(DecodedFrame->States[0].Transform.Position, Frame.States[0].Transform.Position, 0.001f) &&
					Near(DecodedFrame->States[0].Velocity, Frame.States[0].Velocity, 0.008f) &&
					DecodedFrame->States[0].Transform.AngleBetween(Frame.States[0].Transform) < 0.001,
				"compact state preserves full-range position and bounded rotation/velocity error"
			);
			for (std::size_t Boundary = 0; Boundary < FrameBytes->size(); ++Boundary)
				Check(
					!DecodeCharacterMessage(std::span<const std::byte>(*FrameBytes).first(Boundary)),
					"every GCHR v3 truncation boundary fails closed"
				);
			auto Duplicate = *FrameBytes;
			std::copy_n(Duplicate.begin() + 28, 8, Duplicate.begin() + 102);
			Check(!DecodeCharacterMessage(Duplicate), "GCHR v3 rejects duplicate Character identities");
			auto BadCount = *FrameBytes;
			BadCount[24] = std::byte{16};
			BadCount[25] = std::byte{0};
			Check(!DecodeCharacterMessage(BadCount), "GCHR v3 rejects excessive state counts before decode");
			auto BadQuantizedRotation = *FrameBytes;
			BadQuantizedRotation[80] = std::byte{0};
			BadQuantizedRotation[81] = std::byte{0x80};
			Check(!DecodeCharacterMessage(BadQuantizedRotation), "reserved int16 minimum quantized values fail closed");
			auto BadFlags = *FrameBytes;
			BadFlags[100] = std::byte{0x80};
			Check(!DecodeCharacterMessage(BadFlags), "undefined compact Character state flags fail closed");
			auto Trailing = *FrameBytes;
			Trailing.push_back(std::byte{0});
			Check(!DecodeCharacterMessage(Trailing), "GCHR v3 rejects trailing bytes");
		}
		CharacterStateFrame MaximumFrame{
			.ServerTick = 61,
			.FrameSequence = CharacterStateFrameSequence(8),
			.StateCount = static_cast<std::uint16_t>(MaximumCharacterStatesPerFrame),
		};
		for (std::size_t Index = 0; Index < MaximumFrame.StateCount; ++Index) {
			MaximumFrame.States[Index] = Frame.States[0];
			MaximumFrame.States[Index].Character = ObjectId{static_cast<std::uint32_t>(200 + Index), 1};
			MaximumFrame.States[Index].StateSequence = RealtimeStateSequence(Index + 1);
			MaximumFrame.States[Index].AuthoritativeTick = MaximumFrame.ServerTick;
		}
		auto MaximumBytes = EncodeCharacterMessage(CharacterMessage(MaximumFrame));
		Check(
			MaximumBytes && MaximumBytes->size() == 1138 && MaximumBytes->size() <= MaximumCharacterStateFrameBytes,
			"the fixed 15-state compact batch remains below the 1200-byte protocol ceiling"
		);
		auto OutOfRange = Frame.States[0];
		OutOfRange.Velocity.x = MaximumCompactCharacterVelocity + CompactCharacterVelocityResolution;
		Check(
			GetCompactCharacterStateEncodedBytes(OutOfRange) == 0,
			"out-of-range velocity fails before compact encode rather than silently clamping or wrapping"
		);

		std::mt19937 Random(0x3b);
		for (std::size_t Case = 0; Case < 5000; ++Case) {
			const auto Size = Random() % (MaximumCharacterStateFrameBytes + 16);
			std::vector<std::byte> Bytes(Size);
			for (auto &Byte : Bytes)
				Byte = static_cast<std::byte>(Random() & 0xff);
			(void)DecodeCharacterMessage(Bytes);
		}
	}

	struct Fixture {
		NetworkLimits Limits = TestLimits();
		std::shared_ptr<SimulatedNetwork> Network;
		std::shared_ptr<SimulatedTransport> ServerTransport;
		std::shared_ptr<SimulatedTransport> ClientTransport;
		std::unique_ptr<NetworkScheduler> ServerScheduler;
		std::unique_ptr<NetworkScheduler> ClientScheduler;
		std::unique_ptr<AuthoritativeCharacterNetwork> Server;
		std::unique_ptr<PredictedCharacterNetwork> Client;
		std::shared_ptr<WorldRoot> ServerWorld = std::make_shared<WorldRoot>();
		std::shared_ptr<WorldRoot> ClientWorld = std::make_shared<WorldRoot>();
		std::shared_ptr<KinematicCharacter> ServerCharacter = std::make_shared<KinematicCharacter>();
		std::shared_ptr<KinematicCharacter> ClientCharacter = std::make_shared<KinematicCharacter>();
		ConnectionId ServerConnection;
		ConnectionId ClientConnection;
		ObjectId SourceCharacter;
		std::uint64_t Tick = 1;

		explicit Fixture(
			SimulatedTransportConfiguration Configuration = {}, CharacterMovementPolicy ClientMovement = Movement
		) {
			Network = SimulatedNetwork::Create(Configuration);
			ServerTransport = Network->CreateTransport();
			ClientTransport = Network->CreateTransport();
			Check(
				ServerTransport->Start({TransportRole::Server, {"character-tests", 301}, Limits, {}}).Succeeded(),
				"Character simulator server starts"
			);
			Check(
				ClientTransport->Start({TransportRole::Client, {"character-tests", 301}, Limits, {}}).Succeeded(),
				"Character simulator client starts"
			);
			Network->Pump();
			ServerConnection = ConnectedId(Drain(ServerTransport));
			ClientConnection = ConnectedId(Drain(ClientTransport));
			Check(ServerConnection.IsValid() && ClientConnection.IsValid(), "Character simulator peers connect");
			ServerScheduler = std::make_unique<NetworkScheduler>(*ServerTransport);
			ClientScheduler = std::make_unique<NetworkScheduler>(*ClientTransport);
			Check(
				ServerScheduler->RegisterConnection(ServerConnection, Limits), "server Character scheduler registers"
			);
			Check(
				ClientScheduler->RegisterConnection(ClientConnection, Limits), "client Character scheduler registers"
			);
			Server = std::make_unique<AuthoritativeCharacterNetwork>(
				*ServerScheduler,
				Limits,
				Movement,
				[](ConnectionId, const CharacterActionRequest &Request) -> std::optional<std::uint32_t> {
					return Request.RequestedActionToken == 2
							   ? std::nullopt
							   : std::optional<std::uint32_t>(Request.RequestedActionToken);
				}
			);
			Client = std::make_unique<PredictedCharacterNetwork>(*ClientScheduler, Limits, std::move(ClientMovement));
			Check(
				Server->AddPeer(ServerConnection) && Client->AddPeer(ClientConnection),
				"Character managers register their connection generations"
			);
			ServerCharacter->SetPosition({0.0f, 3.0f, 0.0f});
			ClientCharacter->SetPosition({0.0f, 3.0f, 0.0f});
			ServerCharacter->SetParent(ServerWorld);
			ClientCharacter->SetParent(ClientWorld);
			SourceCharacter = ServerCharacter->GetObjectId();
			Check(Server->RegisterCharacter(ServerCharacter), "server registers canonical Character identity");
			Check(
				Server->MarkMaterialized(ServerConnection, SourceCharacter, StateChannelId(700)),
				"server gates Character state on structural materialization"
			);
			Check(
				Client->MarkMaterialized(SourceCharacter, ClientCharacter),
				"client maps only an already-materialized Character replica"
			);
			Check(
				Server->RegisterAction(Action(1, 11)) && Server->RegisterAction(Action(2, 12)),
				"server registers bounded authoritative action definitions"
			);
			Check(
				Client->RegisterAction(Action(1, 11)) && Client->RegisterAction(Action(2, 12)),
				"client registers matching prediction content revisions"
			);
			Check(
				Server->BindControl(ServerConnection, SourceCharacter, Tick).has_value(),
				"server reliably binds one controlling connection and epoch"
			);
			for (int Index = 0; Index < 120 && !Client->GetControl(ClientConnection); ++Index)
				Cycle();
			Check(Client->GetControl(ClientConnection).has_value(), "client receives reliable control bind");
		}

		void Cycle(std::chrono::milliseconds Delta = 5ms) {
			(void)ServerScheduler->Flush(ServerConnection, SchedulerTickBudget::FromNetworkLimits(Limits));
			(void)ClientScheduler->Flush(ClientConnection, SchedulerTickBudget::FromNetworkLimits(Limits));
			(void)Network->Advance(Delta);
			Network->Pump();
			for (const auto &Event : Drain(ServerTransport))
				(void)Server->HandleTransportEvent(Event);
			for (const auto &Event : Drain(ClientTransport))
				(void)Client->HandleTransportEvent(Event);
			Server->Step(*ServerWorld, Tick);
			Client->Reconcile(*ClientWorld);
			Client->UpdatePresentation(Tick);
			++Tick;
		}

		void SetServerWall(float X) {
			auto Wall = std::make_shared<Part>();
			Wall->SetAnchored(true);
			Wall->SetSize({1.0f, 12.0f, 12.0f});
			Wall->SetCFrame(CFrame(X, 3.0f, 0.0f));
			Wall->SetParent(ServerWorld);
			ServerWorld->StepPhysics(0.0, std::nullopt);
		}
	};

	void TestPredictionAuthorityAndActions() {
		Fixture Value;
		ChangeJournal::Get().Clear();
		const auto Start = Value.ClientCharacter->GetPosition();
		Check(
			Value.Client->SubmitInput(
				Value.ClientConnection, *Value.ClientWorld, Value.Tick, 1.0f / 60.0f, {1.0f, 0.0f}, 0.0f, false
			),
			"client submits bounded semantic input and predicts locally"
		);
		Check(
			Value.ClientCharacter->GetPosition().x > Start.x,
			"local Character prediction is responsive before server state"
		);
		for (int Index = 0; Index < 4; ++Index) {
			Value.Cycle();
			if (Index != 3)
				Check(
					Value.Client->SubmitInput(
						Value.ClientConnection, *Value.ClientWorld, Value.Tick, 1.0f / 60.0f, {1.0f, 0.0f}, 0.0f, false
					),
					"continuous movement intent is sampled once per client simulation tick"
				);
		}
		Check(
			Value.Client->SubmitInput(
				Value.ClientConnection, *Value.ClientWorld, Value.Tick, 1.0f / 60.0f, {}, 0.0f, false
			),
			"client submits a neutral intent when movement ends"
		);
		for (int Index = 0; Index < 8; ++Index)
			Value.Cycle();
		Check(
			Near(Value.ClientCharacter->GetPosition(), Value.ServerCharacter->GetPosition()),
			"acknowledged input restores authoritative state and converges prediction"
		);
		Check(
			Value.Client->GetPredictionHistorySize(Value.ClientConnection) == 0,
			"acknowledged prediction history is discarded"
		);
		Check(
			ChangeJournal::Get().ReadSince(0).empty(),
			"predicted and authoritative runtime Character motion emits no authoring journal records"
		);

		auto SampleAction = FirstCompleteGameAction();
		Check(
			SampleAction && SampleAction->Animation.ToString() == "d9d9e9649adbad59588d137c2a642e1d" &&
				Value.Server->RegisterAction(*SampleAction) && Value.Client->RegisterAction(*SampleAction),
			"FirstCompleteGame BeaconLunge pins its real catalog AssetId and content revision on both peers"
		);
		const auto BeforeSampleAction = Value.ServerCharacter->GetPosition();
		Check(
			Value.Client->RequestAction(Value.ClientConnection, 3, Value.Tick),
			"FirstCompleteGame requests BeaconLunge as a semantic action rather than client root displacement"
		);
		for (int Index = 0; Index < 5; ++Index) {
			Value.Cycle();
			if (Value.Client->GetControl(Value.ClientConnection))
				(void)Value.Client->SubmitInput(
					Value.ClientConnection, *Value.ClientWorld, Value.Tick, 1.0f / 60.0f, {0.0f, 0.0f}, 0.0f, false
				);
		}
		for (int Index = 0; Index < 3; ++Index)
			Value.Cycle();
		Check(
			Value.ServerCharacter->GetPosition().x > BeforeSampleAction.x + 0.2f &&
				Near(Value.ClientCharacter->GetPosition(), Value.ServerCharacter->GetPosition(), 0.05f),
			"FirstCompleteGame BeaconLunge is server-derived and reconciles in the headless network vertical"
		);

		Value.SetServerWall(Value.ServerCharacter->GetPosition().x + 1.5f);
		ChangeJournal::Get().Clear();
		Check(
			Value.Client->RequestAction(Value.ClientConnection, 1, Value.Tick),
			"client reliably requests a semantic root-motion action without a root delta"
		);
		Value.Cycle();
		Check(
			Value.Client->SubmitInput(
				Value.ClientConnection, *Value.ClientWorld, Value.Tick, 1.0f / 60.0f, {0.0f, 0.0f}, 0.0f, false
			),
			"client advances predicted root action from pinned content"
		);
		const auto PredictedX = Value.ClientCharacter->GetPosition().x;
		for (int Index = 0; Index < 5; ++Index)
			Value.Cycle();
		Check(
			PredictedX > Value.ServerCharacter->GetPosition().x + 0.25f,
			"client-only clear world predicts farther than server collision authority"
		);
		Check(
			Near(Value.ClientCharacter->GetPosition(), Value.ServerCharacter->GetPosition(), 0.05f),
			"server-only collider correction rewinds the client with no root-motion debt"
		);

		const auto BeforeRejected = Value.ServerCharacter->GetPosition();
		Check(
			Value.Client->RequestAction(Value.ClientConnection, 2, Value.Tick),
			"client may speculate on an action that authoritative policy can reject"
		);
		Value.Cycle();
		Check(
			Value.Client->SubmitInput(
				Value.ClientConnection, *Value.ClientWorld, Value.Tick, 1.0f / 60.0f, {0.0f, 0.0f}, 0.0f, false
			),
			"rejected action prediction advances speculatively"
		);
		for (int Index = 0; Index < 5; ++Index)
			Value.Cycle();
		Check(
			Near(Value.ServerCharacter->GetPosition(), BeforeRejected, 0.05f),
			"rejected client action never moves the authoritative Character"
		);
		Check(
			Near(Value.ClientCharacter->GetPosition(), Value.ServerCharacter->GetPosition(), 0.05f),
			"action rejection removes invalid predicted root motion"
		);
		auto ServerMetrics = Value.Server->GetMetrics();
		auto ClientMetrics = Value.Client->GetMetrics();
		Check(
			ServerMetrics.CommandsAccepted > 0 && ServerMetrics.ActionRequestsAccepted > 0 &&
				ServerMetrics.ActionRequestsRejected > 0 && ClientMetrics.PredictionCorrections > 0,
			"Character networking exposes bounded authority and correction diagnostics"
		);
		Check(
			ChangeJournal::Get().ReadSince(0).empty(),
			"root-motion prediction, collision correction, and action rejection stay outside structural replication"
		);
	}

	void TestServerAuthoritativePredictionMode() {
		Fixture Value;
		Value.Client->SetPredictionEnabled(false);
		const auto Start = Value.ClientCharacter->GetPosition();
		Check(
			Value.Client->SubmitInput(
				Value.ClientConnection, *Value.ClientWorld, Value.Tick, 1.0f / 60.0f, {1.0f, 0.0f}, 0.0f, false
			),
			"server-authoritative mode still submits bounded semantic input"
		);
		Check(
			Near(Value.ClientCharacter->GetPosition(), Start) &&
				Value.Client->GetPredictionHistorySize(Value.ClientConnection) == 0,
			"disabled prediction performs no local motion and retains no replay history"
		);
		for (int Index = 0; Index < 16; ++Index)
			Value.Cycle();
		Check(
			Value.ClientCharacter->GetPosition().x > Start.x,
			"server-authoritative mode remains playable through authoritative state presentation"
		);
		Value.Client->SetPredictionEnabled(true);
		Check(
			Value.Client->GetPredictionHistorySize(Value.ClientConnection) == 0,
			"prediction mode transition starts with a clean bounded history"
		);
	}

	void TestStaleControlRelevanceAndNpc() {
		Fixture Value;
		const auto FirstControl = *Value.Client->GetControl(Value.ClientConnection);
		Check(Value.Server->RevokeControl(Value.SourceCharacter, Value.Tick), "server revokes a live control lifetime");
		Value.Cycle();
		Check(!Value.Client->GetControl(Value.ClientConnection), "reliable unbind destroys client prediction lineage");
		Check(
			Value.Server->BindControl(Value.ServerConnection, Value.SourceCharacter, Value.Tick).has_value(),
			"same Character receives a fresh control epoch after reassignment"
		);
		Value.Cycle();
		const auto SecondControl = *Value.Client->GetControl(Value.ClientConnection);
		Check(
			SecondControl.ControlEpoch.IsNewerThan(FirstControl.ControlEpoch),
			"control epochs advance independently of Character ObjectId generation"
		);

		CharacterInputCommand Stale{
			Value.SourceCharacter,
			FirstControl.ControlEpoch,
			CharacterInputSequence(50),
			Value.Tick,
			1.0f / 60.0f,
			{1.0f, 0.0f},
			0.0f,
			0
		};
		auto Bytes = EncodeCharacterMessage(CharacterMessage(Stale));
		ReceivedMessageEvent Forged{
			Value.ServerConnection,
			DeliveryMode::UnreliableSequenced,
			TrafficClass::RealtimeState,
			RealtimeStateOrder{StateChannelId(700), RealtimeStateSequence(50)},
			Bytes ? std::move(*Bytes) : std::vector<std::byte>{}
		};
		Check(
			!Value.Server->HandleTransportEvent(TransportEvent(Forged)),
			"old control-epoch command is rejected after reassignment"
		);

		const ConnectionId WrongPeer{81, 2};
		Check(Value.Server->AddPeer(WrongPeer), "adversarial peer is registered without Character authority");
		CharacterInputCommand WrongPeerCommand{
			Value.SourceCharacter,
			SecondControl.ControlEpoch,
			CharacterInputSequence(60),
			Value.Tick,
			1.0f / 60.0f,
			{1.0f, 0.0f},
			0.0f,
			0
		};
		auto WrongPeerBytes = EncodeCharacterMessage(CharacterMessage(WrongPeerCommand));
		ReceivedMessageEvent WrongPeerEvent{
			WrongPeer,
			DeliveryMode::UnreliableSequenced,
			TrafficClass::RealtimeState,
			RealtimeStateOrder{StateChannelId(700), RealtimeStateSequence(60)},
			WrongPeerBytes ? std::move(*WrongPeerBytes) : std::vector<std::byte>{}
		};
		Check(
			!Value.Server->HandleTransportEvent(TransportEvent(WrongPeerEvent)),
			"a different peer cannot submit input for another peer's Character"
		);

		CharacterInputCommand WrongCharacter{
			ObjectId{999'999, 4},
			SecondControl.ControlEpoch,
			CharacterInputSequence(61),
			Value.Tick,
			1.0f / 60.0f,
			{1.0f, 0.0f},
			0.0f,
			0
		};
		auto WrongCharacterBytes = EncodeCharacterMessage(CharacterMessage(WrongCharacter));
		ReceivedMessageEvent WrongCharacterEvent{
			Value.ServerConnection,
			DeliveryMode::UnreliableSequenced,
			TrafficClass::RealtimeState,
			RealtimeStateOrder{StateChannelId(700), RealtimeStateSequence(61)},
			WrongCharacterBytes ? std::move(*WrongCharacterBytes) : std::vector<std::byte>{}
		};
		Check(
			!Value.Server->HandleTransportEvent(TransportEvent(WrongCharacterEvent)),
			"a controlling peer cannot substitute another Character ObjectId"
		);

		for (std::uint64_t Index = 0; Index < MaximumCharacterCommandsPerTick + 1; ++Index) {
			CharacterInputCommand Flood{
				Value.SourceCharacter,
				SecondControl.ControlEpoch,
				CharacterInputSequence(100 + Index),
				Value.Tick + Index,
				1.0f / 60.0f,
				{1.0f, 0.0f},
				0.0f,
				0
			};
			auto FloodBytes = EncodeCharacterMessage(CharacterMessage(Flood));
			ReceivedMessageEvent FloodEvent{
				Value.ServerConnection,
				DeliveryMode::UnreliableSequenced,
				TrafficClass::RealtimeState,
				RealtimeStateOrder{StateChannelId(700), RealtimeStateSequence(100 + Index)},
				FloodBytes ? std::move(*FloodBytes) : std::vector<std::byte>{}
			};
			(void)Value.Server->HandleTransportEvent(TransportEvent(FloodEvent));
		}
		Check(
			Value.Server->GetMetrics().RateLimitedCommands > 0,
			"command flooding is bounded by the server tick rather than client-reported tick values"
		);

		auto NpcServer = std::make_shared<KinematicCharacter>();
		auto NpcClient = std::make_shared<KinematicCharacter>();
		NpcServer->SetPosition({20.0f, 3.0f, 0.0f});
		NpcClient->SetPosition({20.0f, 3.0f, 0.0f});
		NpcServer->SetParent(Value.ServerWorld);
		NpcClient->SetParent(Value.ClientWorld);
		const auto NpcId = NpcServer->GetObjectId();
		Check(
			Value.Server->RegisterCharacter(NpcServer) &&
				Value.Server->MarkMaterialized(Value.ServerConnection, NpcId, StateChannelId(701)) &&
				Value.Client->MarkMaterialized(NpcId, NpcClient),
			"NPC uses the same relevance-gated Character channel without Player"
		);
		Check(
			Value.Server->StartServerAction(NpcId, 1, Value.Tick),
			"server starts an NPC authoritative action without fake client ownership"
		);
		Value.Cycle();
		Value.Cycle();
		Check(
			Value.Client->GetAuthoritativeAction(NpcId).has_value(),
			"reliable authoritative action identity is exposed for remote animation policy"
		);
		for (int Index = 0; Index < 3; ++Index)
			Value.Cycle();
		Check(
			NpcServer->GetPosition().x > 20.0f && Near(NpcClient->GetPosition(), NpcServer->GetPosition(), 0.05f),
			"NPC root motion publishes newest authoritative state to its client replica"
		);

		Check(
			Value.Server->MarkUnmaterialized(Value.ServerConnection, NpcId),
			"structural relevance withdrawal removes the NPC realtime channel"
		);
		const auto HiddenPosition = NpcClient->GetPosition();
		Check(Value.Server->StartServerAction(NpcId, 1, Value.Tick), "hidden NPC may continue server simulation");
		for (int Index = 0; Index < 4; ++Index)
			Value.Cycle();
		Check(
			Near(NpcClient->GetPosition(), HiddenPosition),
			"hidden Character realtime state cannot probe or update a materialized client object"
		);
	}

	void TestControlDeferralReconnectAndReplay() {
		RecordingScheduler ClientScheduler;
		const ConnectionId ClientConnection{41, 3};
		PredictedCharacterNetwork Client(ClientScheduler, TestLimits(), Movement);
		const ObjectId Source{9'001, 4};
		Check(Client.AddPeer(ClientConnection), "deferred-control client registers its connection generation");
		auto ControlEvent = [&](const CharacterControlTransition &Transition) {
			auto Bytes = EncodeCharacterMessage(CharacterMessage(Transition));
			return ReceivedMessageEvent{
				ClientConnection,
				DeliveryMode::ReliableOrdered,
				TrafficClass::Control,
				{},
				Bytes ? std::move(*Bytes) : std::vector<std::byte>{}
			};
		};
		const CharacterControlTransition DeferredBind{Source, CharacterControlEpoch(7), StateChannelId(19), 1, true};
		auto DeferredBindEvent = ControlEvent(DeferredBind);
		Check(
			Client.HandleTransportEvent(TransportEvent(DeferredBindEvent)) && !Client.GetControl(ClientConnection),
			"control bind remains bounded and deferred until structural materialization"
		);
		auto DeferredUnbindEvent = ControlEvent({Source, CharacterControlEpoch(7), {}, 2, false});
		Check(
			Client.HandleTransportEvent(TransportEvent(DeferredUnbindEvent)),
			"reliable unbind cancels a bind that is still awaiting materialization"
		);
		auto Replica = std::make_shared<KinematicCharacter>();
		Check(
			Client.MarkMaterialized(Source, Replica) && !Client.GetControl(ClientConnection),
			"late materialization cannot resurrect a revoked control lifetime"
		);
		Check(
			!Client.HandleTransportEvent(TransportEvent(DeferredBindEvent)),
			"a replayed bind cannot restore a revoked control epoch"
		);
		auto FreshBindEvent = ControlEvent({Source, CharacterControlEpoch(8), StateChannelId(20), 3, true});
		Check(
			Client.HandleTransportEvent(TransportEvent(FreshBindEvent)) &&
				Client.GetControl(ClientConnection)->ControlEpoch == CharacterControlEpoch(8),
			"a newer reliable bind establishes the replacement control lifetime"
		);
		Check(
			!Client.HandleTransportEvent(TransportEvent(DeferredBindEvent)) &&
				Client.GetControl(ClientConnection)->ControlEpoch == CharacterControlEpoch(8),
			"an old reliable bind cannot roll a live control epoch backward"
		);

		RecordingScheduler ServerScheduler;
		const ConnectionId OldConnection{51, 1};
		const ConnectionId NewConnection{51, 2};
		bool SawJump = false;
		CharacterMovementPolicy JumpPolicy = [&](const CharacterInputCommand &Command,
												 const KinematicCharacter &CharacterValue) {
			SawJump = Command.JumpRequested();
			return Movement(Command, CharacterValue);
		};
		AuthoritativeCharacterNetwork Server(ServerScheduler, TestLimits(), JumpPolicy);
		WorldRoot World;
		auto Character = std::make_shared<KinematicCharacter>();
		const auto CharacterId = Character->GetObjectId();
		Check(
			Server.AddPeer(OldConnection) && Server.RegisterCharacter(Character) &&
				Server.MarkMaterialized(OldConnection, CharacterId, StateChannelId(30)) &&
				Server.RegisterAction(Action(1, 21)),
			"reconnect fixture registers one generation-safe Character and action"
		);
		const auto OldEpoch = Server.BindControl(OldConnection, CharacterId, 1);
		Check(OldEpoch.has_value(), "initial connection receives a control epoch");
		Check(
			Server.RemovePeer(OldConnection, 2) && Server.AddPeer(NewConnection) &&
				Server.MarkMaterialized(NewConnection, CharacterId, StateChannelId(31)),
			"disconnect removes old peer state before the replacement connection is admitted"
		);
		const auto NewEpoch = Server.BindControl(NewConnection, CharacterId, 3);
		Check(
			NewEpoch && NewEpoch->IsNewerThan(*OldEpoch),
			"reconnect receives a new control epoch independent of connection generation"
		);
		auto InputEvent = [&](ConnectionId Connection, ObjectId Id, CharacterControlEpoch Epoch) {
			CharacterInputCommand Command{
				Id,
				Epoch,
				CharacterInputSequence(1),
				4,
				1.0f / 60.0f,
				{1.0f, 0.0f},
				0.0f,
				static_cast<std::uint8_t>(CharacterInputFlag::JumpRequested)
			};
			auto Bytes = EncodeCharacterMessage(CharacterMessage(Command));
			return ReceivedMessageEvent{
				Connection,
				DeliveryMode::UnreliableSequenced,
				TrafficClass::RealtimeState,
				RealtimeStateOrder{StateChannelId(31), RealtimeStateSequence(1)},
				Bytes ? std::move(*Bytes) : std::vector<std::byte>{}
			};
		};
		auto OldConnectionInput = InputEvent(OldConnection, CharacterId, *OldEpoch);
		auto OldEpochInput = InputEvent(NewConnection, CharacterId, *OldEpoch);
		const ObjectId ReusedSlot{CharacterId.Slot, CharacterId.Generation + 1};
		auto ReplacementInput = InputEvent(NewConnection, ReusedSlot, *NewEpoch);
		Check(
			!Server.HandleTransportEvent(TransportEvent(OldConnectionInput)) &&
				!Server.HandleTransportEvent(TransportEvent(OldEpochInput)) &&
				!Server.HandleTransportEvent(TransportEvent(ReplacementInput)),
			"old connection, old epoch, and mismatched ObjectId generation all fail closed after reconnect"
		);
		auto FreshInput = InputEvent(NewConnection, CharacterId, *NewEpoch);
		Check(
			Server.HandleTransportEvent(TransportEvent(FreshInput)) &&
				!Server.HandleTransportEvent(TransportEvent(FreshInput)),
			"fresh input is admitted once and duplicate input sequence replay is rejected"
		);
		Server.Step(World, 4);
		Check(SawJump, "the bounded Jump bit reaches replaceable server policy rather than native gameplay state");
		CharacterActionRequest Request{
			CharacterId, *NewEpoch, CharacterActionSequence(1), CharacterInputSequence(1), 1
		};
		auto ActionBytes = EncodeCharacterMessage(CharacterMessage(Request));
		ReceivedMessageEvent ActionEvent{
			NewConnection,
			DeliveryMode::ReliableOrdered,
			TrafficClass::ReliableApplication,
			{},
			ActionBytes ? std::move(*ActionBytes) : std::vector<std::byte>{}
		};
		Check(
			Server.HandleTransportEvent(TransportEvent(ActionEvent)) &&
				!Server.HandleTransportEvent(TransportEvent(ActionEvent)),
			"reliable semantic action is accepted once and replayed action sequence is rejected"
		);
	}

	void TestClientPolicyCannotChangeAuthority() {
		CharacterMovementPolicy ModifiedClientPolicy = [](const CharacterInputCommand &Command,
														  const KinematicCharacter &) {
			constexpr float ModifiedWalkSpeed = 60.0f;
			return CharacterMotionRequest{
				.Translation =
					{Command.MoveIntent.x * ModifiedWalkSpeed * Command.DeltaSeconds,
					 0.0f,
					 Command.MoveIntent.y * ModifiedWalkSpeed * Command.DeltaSeconds},
				.Velocity = {Command.MoveIntent.x * ModifiedWalkSpeed, 0.0f, Command.MoveIntent.y * ModifiedWalkSpeed},
			};
		};
		Fixture Value({}, ModifiedClientPolicy);
		const auto ServerStart = Value.ServerCharacter->GetPosition();
		Check(
			Value.Client->SubmitInput(
				Value.ClientConnection, *Value.ClientWorld, Value.Tick, 1.0f / 60.0f, {1.0f, 0.0f}, 0.0f, false
			),
			"modified local walk policy may only affect speculative prediction"
		);
		const auto ModifiedPrediction = Value.ClientCharacter->GetPosition().x;
		for (int Index = 0; Index < 5; ++Index)
			Value.Cycle();
		const auto ServerDistance = Value.ServerCharacter->GetPosition().x - ServerStart.x;
		Check(
			Value.Client->SubmitInput(
				Value.ClientConnection, *Value.ClientWorld, Value.Tick, 1.0f / 60.0f, {}, 0.0f, false
			),
			"a neutral intent ends the authoritative continuous movement window"
		);
		for (int Index = 0; Index < 8; ++Index)
			Value.Cycle();
		Check(
			ModifiedPrediction > ServerStart.x + 0.5f && ServerDistance <= 0.5f + 0.001f &&
				Near(Value.ClientCharacter->GetPosition(), Value.ServerCharacter->GetPosition(), 0.05f),
			"client WalkSpeed modification cannot increase server motion and is reconciled"
		);
	}

	void TestLatestIntentFreshnessTimeout() {
		Fixture Value;
		Check(
			Value.Client->SubmitInput(
				Value.ClientConnection, *Value.ClientWorld, Value.Tick, 1.0f / 60.0f, {1.0f, 0.0f}, 0.0f, false
			),
			"freshness fixture submits one bounded movement intent"
		);
		for (std::uint64_t Index = 0; Index < DefaultCharacterInputFreshnessTicks + 4; ++Index)
			Value.Cycle();
		const auto TimedOutPosition = Value.ServerCharacter->GetPosition();
		for (int Index = 0; Index < 4; ++Index)
			Value.Cycle();
		Check(
			Near(Value.ServerCharacter->GetPosition(), TimedOutPosition) &&
				Value.Server->GetMetrics().InputFreshnessTimeouts == 1,
			"latest intent is reused continuously, then neutralized exactly once after the bounded freshness window"
		);
	}

	void TestPredictionBoundsAndContentMismatch() {
		RecordingScheduler Scheduler;
		const ConnectionId Connection{22, 7};
		PredictedCharacterNetwork Client(Scheduler, TestLimits(), Movement);
		WorldRoot World;
		auto Replica = std::make_shared<KinematicCharacter>();
		const ObjectId Source{5000, 9};
		Check(
			Client.AddPeer(Connection) && Client.MarkMaterialized(Source, Replica) &&
				Client.RegisterAction(Action(1, 11)),
			"bounded prediction fixture registers one replica and pinned action"
		);
		CharacterControlTransition Bind{Source, CharacterControlEpoch(3), StateChannelId(88), 1, true};
		auto BindBytes = EncodeCharacterMessage(CharacterMessage(Bind));
		ReceivedMessageEvent BindEvent{
			Connection,
			DeliveryMode::ReliableOrdered,
			TrafficClass::Control,
			{},
			BindBytes ? std::move(*BindBytes) : std::vector<std::byte>{}
		};
		Check(
			Client.HandleTransportEvent(TransportEvent(BindEvent)), "bounded prediction fixture accepts reliable bind"
		);
		for (std::size_t Index = 0; Index < MaximumCharacterPredictionHistory; ++Index)
			Check(
				Client.SubmitInput(Connection, World, Index + 2, 1.0f / 60.0f, {1.0f, 0.0f}, 0.0f, false),
				"prediction history admits each entry through its fixed ceiling"
			);
		Check(
			!Client.SubmitInput(
				Connection, World, MaximumCharacterPredictionHistory + 2, 1.0f / 60.0f, {1.0f, 0.0f}, 0.0f, false
			) && Client.GetPredictionHistorySize(Connection) == 0 &&
				Client.GetMetrics().HistoryOverflows == 1,
			"history overflow clears bounded replay state and suspends further prediction"
		);
		Check(
			!Client.RequestAction(Connection, 1, MaximumCharacterPredictionHistory + 3),
			"a suspended predictor cannot accumulate an action beyond its history bound"
		);

		CharacterAuthoritativeState Baseline{
			.Character = Source,
			.ControlEpoch = CharacterControlEpoch(3),
			.StateSequence = RealtimeStateSequence(1),
			.AuthoritativeTick = 80,
			.Transform = CFrame(0.0f, 6.0f, 0.0f),
			.Flags = static_cast<std::uint8_t>(CharacterStateFlag::Teleport)
		};
		auto BaselineBytes = EncodeCharacterMessage(CharacterMessage(Baseline));
		ReceivedMessageEvent BaselineEvent{
			Connection,
			DeliveryMode::UnreliableSequenced,
			TrafficClass::RealtimeState,
			RealtimeStateOrder{StateChannelId(88), RealtimeStateSequence(1)},
			BaselineBytes ? std::move(*BaselineBytes) : std::vector<std::byte>{}
		};
		Check(
			Client.HandleTransportEvent(TransportEvent(BaselineEvent)), "authoritative teleport baseline is admitted"
		);
		Client.Reconcile(World);
		Check(
			Client.SubmitInput(Connection, World, 81, 1.0f / 60.0f, {0.0f, 0.0f}, 0.0f, false),
			"authoritative baseline resumes prediction after the conservative overflow reset"
		);

		CharacterAuthoritativeState CompletedAction{
			.Character = Source,
			.ControlEpoch = CharacterControlEpoch(3),
			.StateSequence = RealtimeStateSequence(3),
			.ResolvedAction = CharacterActionSequence(1),
			.AuthoritativeTick = 84,
			.Transform = CFrame(0.0f, 6.0f, 0.0f)
		};
		auto CompletedBytes = EncodeCharacterMessage(CharacterMessage(CompletedAction));
		ReceivedMessageEvent CompletedEvent{
			Connection,
			DeliveryMode::UnreliableSequenced,
			TrafficClass::RealtimeState,
			RealtimeStateOrder{StateChannelId(88), RealtimeStateSequence(3)},
			CompletedBytes ? std::move(*CompletedBytes) : std::vector<std::byte>{}
		};
		Check(
			Client.HandleTransportEvent(TransportEvent(CompletedEvent)),
			"a newer sequenced snapshot may arrive before an older reliable action decision"
		);
		Client.Reconcile(World);
		const auto Matching = Action(1, 11);
		CharacterAuthoritativeState LateReliableAction{
			.Character = Source,
			.ControlEpoch = CharacterControlEpoch(3),
			.StateSequence = RealtimeStateSequence(2),
			.ResolvedAction = CharacterActionSequence(1),
			.AuthoritativeTick = 82,
			.Transform = CFrame(0.0f, 6.0f, 0.0f),
			.ActiveAction = CharacterActionState{
				CharacterActionSequence(1), 1, Matching.Animation, Matching.ContentRevision, 82, 4
			}
		};
		auto LateActionBytes = EncodeCharacterMessage(CharacterMessage(LateReliableAction));
		ReceivedMessageEvent LateActionEvent{
			Connection,
			DeliveryMode::ReliableOrdered,
			TrafficClass::Control,
			{},
			LateActionBytes ? std::move(*LateActionBytes) : std::vector<std::byte>{}
		};
		Check(
			Client.HandleTransportEvent(TransportEvent(LateActionEvent)) && Client.GetAuthoritativeAction(Source),
			"reliable one-shot action identity survives cross-lane reorder without rolling back Character state"
		);

		const auto Mismatched = Action(1, 99);
		CharacterAuthoritativeState RevisionMismatch{
			.Character = Source,
			.ControlEpoch = CharacterControlEpoch(3),
			.StateSequence = RealtimeStateSequence(4),
			.ResolvedAction = CharacterActionSequence(2),
			.AuthoritativeTick = 85,
			.Transform = CFrame(0.0f, 6.0f, 0.0f),
			.ActiveAction = CharacterActionState{
				CharacterActionSequence(2), 1, Mismatched.Animation, Mismatched.ContentRevision, 85, 4
			}
		};
		auto MismatchBytes = EncodeCharacterMessage(CharacterMessage(RevisionMismatch));
		ReceivedMessageEvent MismatchEvent{
			Connection,
			DeliveryMode::ReliableOrdered,
			TrafficClass::Control,
			{},
			MismatchBytes ? std::move(*MismatchBytes) : std::vector<std::byte>{}
		};
		Check(
			Client.HandleTransportEvent(TransportEvent(MismatchEvent)),
			"server-authenticated state reaches reconciliation even when local action content differs"
		);
		Client.Reconcile(World);
		Check(
			!Client.GetAuthoritativeAction(Source) && Client.GetMetrics().HardResets >= 2,
			"pinned animation revision mismatch disables root prediction and hard-resets its lineage"
		);
		Check(
			!Client.HandleTransportEvent(TransportEvent(BaselineEvent)),
			"duplicated or reordered authoritative state is rejected by its independent sequence"
		);
	}

	void TestLossReorderAndLifecycle() {
		Fixture Value({
			.Seed = 0x3b,
			.BaseLatency = 50ms,
			.MaximumJitter = 20ms,
			.MaximumReorderDelay = 20ms,
			.UnreliableLossProbability = 0.2,
			.UnreliableDuplicationProbability = 0.2,
			.UnreliableReorderProbability = 0.5,
		});
		for (int Index = 0; Index < 20; ++Index) {
			if (Value.Client->GetControl(Value.ClientConnection))
				(void)Value.Client->SubmitInput(
					Value.ClientConnection, *Value.ClientWorld, Value.Tick, 1.0f / 60.0f, {1.0f, 0.0f}, 0.0f, false
				);
			Value.Cycle(16ms);
		}
		for (int Index = 0; Index < 30; ++Index)
			Value.Cycle(16ms);
		Check(
			Near(Value.ClientCharacter->GetPosition(), Value.ServerCharacter->GetPosition(), 0.25f),
			"sequenced state converges after deterministic latency, loss, duplication, and reorder"
		);
		Check(
			Value.Server->RemovePeer(Value.ServerConnection, Value.Tick),
			"disconnect removes the controlling connection and pending bounded state"
		);
		Check(
			!Value.Server->PublishState(Value.SourceCharacter, Value.Tick),
			"disconnected peer receives no Character side-channel state"
		);
		Check(Value.Client->RemovePeer(Value.ClientConnection), "client teardown destroys prediction history");
		Value.ServerCharacter->Destroy();
		Value.Server->Step(*Value.ServerWorld, Value.Tick);
		Value.ClientCharacter->Destroy();
		Value.Client->Reconcile(*Value.ClientWorld);
		Check(true, "destroyed Characters and managers with active network history tear down without stale mutation");
	}

	void TestRemotePresentationFaultMatrix() {
		std::size_t Scenarios = 0;
		for (const auto Latency : {0ms, 50ms, 100ms, 200ms})
			for (const auto Jitter : {0ms, 10ms, 30ms})
				for (const auto Loss : {0.0, 0.01, 0.05, 0.10}) {
					Fixture Value({
						.Seed = 0x3c + Scenarios,
						.BaseLatency = Latency,
						.MaximumJitter = Jitter,
						.MaximumReorderDelay = Jitter,
						.UnreliableLossProbability = Loss,
						.UnreliableDuplicationProbability = Jitter == 30ms ? 0.05 : 0.0,
						.UnreliableReorderProbability = Jitter == 0ms ? 0.0 : (Jitter == 10ms ? 0.10 : 0.30),
					});
					++Scenarios;
					auto ServerNpc = std::make_shared<KinematicCharacter>();
					auto ClientNpc = std::make_shared<KinematicCharacter>();
					ServerNpc->SetPosition({0.0f, 6.0f, 0.0f});
					ClientNpc->SetPosition({0.0f, 6.0f, 0.0f});
					ServerNpc->SetParent(Value.ServerWorld);
					ClientNpc->SetParent(Value.ClientWorld);
					const auto NpcId = ServerNpc->GetObjectId();
					Check(
						Value.Server->RegisterCharacter(ServerNpc) &&
							Value.Server->MarkMaterialized(Value.ServerConnection, NpcId, StateChannelId(701)) &&
							Value.Client->MarkMaterialized(NpcId, ClientNpc),
						"fault-matrix remote NPC materializes without a Player"
					);
					float MaximumPresentationError = 0.0f;
					for (int Tick = 0; Tick < 60; ++Tick) {
						auto Transform = ServerNpc->GetCFrame();
						Transform.Position.x += 0.05f;
						ServerNpc->ApplyRuntimeTransform(Transform);
						Value.Cycle(16ms);
						MaximumPresentationError = std::max(
							MaximumPresentationError,
							glm::distance(ClientNpc->GetPresentationCFrame().Position, ClientNpc->GetPosition())
						);
					}
					for (int Tick = 0; Tick < 100; ++Tick)
						Value.Cycle(16ms);
					Check(
						MaximumPresentationError < MaximumHardCorrectionDistance &&
							Near(ClientNpc->GetPosition(), ServerNpc->GetPosition(), 0.1f) &&
							Near(ClientNpc->GetPresentationCFrame().Position, ServerNpc->GetPosition(), 0.1f),
						"bounded interpolation converges under the latency/jitter/loss/reorder matrix"
					);
				}
		Check(Scenarios == 48, "remote presentation covers all 4x3x4 deterministic fault combinations");
	}

	void TestRepeatedManagerChurn() {
		for (std::uint64_t Cycle = 1; Cycle <= 10; ++Cycle) {
			RecordingScheduler Scheduler;
			const ConnectionId Connection{static_cast<std::uint32_t>(70 + Cycle), 1};
			WorldRoot World;
			auto Character = std::make_shared<KinematicCharacter>();
			AuthoritativeCharacterNetwork Server(Scheduler, TestLimits(), Movement);
			Check(
				Server.AddPeer(Connection) && Server.RegisterCharacter(Character) &&
					Server.MarkMaterialized(Connection, Character->GetObjectId(), StateChannelId(100 + Cycle)) &&
					Server.RegisterAction(Action(1, static_cast<std::uint8_t>(Cycle))) &&
					Server.BindControl(Connection, Character->GetObjectId(), Cycle).has_value() &&
					Server.StartServerAction(Character->GetObjectId(), 1, Cycle),
				"manager churn cycle establishes active authoritative state"
			);
			Character->Destroy();
			Server.Step(World, Cycle + 1);
		}
		Check(true, "ten active-action Character destruction and manager teardown cycles remain generation-safe");
	}

	void TestStateBatchingCadenceAndSuppression() {
		RecordingScheduler Scheduler;
		const ConnectionId Connection{40, 1};
		AuthoritativeCharacterNetwork Server(Scheduler, TestLimits(), Movement);
		WorldRoot World;
		Check(Server.AddPeer(Connection), "3C batching fixture registers one peer");
		std::vector<std::shared_ptr<KinematicCharacter>> Characters;
		for (std::size_t Index = 0; Index < 32; ++Index) {
			auto Character = std::make_shared<KinematicCharacter>();
			Character->SetPosition({static_cast<float>(Index), 6.0f, 0.0f});
			Check(
				Server.RegisterCharacter(Character) &&
					Server.MarkMaterialized(Connection, Character->GetObjectId(), StateChannelId(Index + 1)),
				"3C batching fixture materializes each NPC without a Player"
			);
			Characters.push_back(std::move(Character));
		}
		Server.Step(World, 1);
		Check(Scheduler.Messages.size() == 3, "32 Character states become three scheduler submissions, not 32");
		std::array<std::uint16_t, 3> Counts{};
		for (std::size_t Index = 0; Index < Scheduler.Messages.size(); ++Index) {
			auto Decoded = DecodeCharacterMessage(Scheduler.Messages[Index].Payload());
			auto *Frame = Decoded ? std::get_if<CharacterStateFrame>(&*Decoded) : nullptr;
			Counts[Index] = Frame ? Frame->StateCount : 0;
			Check(
				Frame && Scheduler.Messages[Index].Payload().size() <= MaximumCharacterStateFrameBytes,
				"every scheduler submission is one bounded GCHR v3 state frame"
			);
		}
		Check(Counts == std::array<std::uint16_t, 3>{15, 15, 2}, "ObjectId ordering splits 32 states as 15/15/2");
		Scheduler.Messages.clear();
		Server.Step(World, 2);
		Server.Step(World, 3);
		Server.Step(World, 4);
		Check(Scheduler.Messages.empty(), "20 Hz cadence and stationary suppression emit no redundant state frame");
		auto Metrics = Server.GetMetrics();
		Check(
			Metrics.StatesConsidered == 64 && Metrics.StatesSuppressedUnchanged == 32 &&
				Metrics.StateFramesEmitted == 3 && Metrics.BatchSplits == 2,
			"bounded 3C metrics expose consideration, suppression, batching, and split counts"
		);
		Server.Step(World, 61);
		Check(Scheduler.Messages.size() == 3, "one-second periodic absolute refresh recovers stationary streams");
		Scheduler.Messages.clear();
		Characters[20]->ApplyRuntimeTransform(CFrame(100.0f, 6.0f, 0.0f));
		Server.Step(World, 64);
		Check(Scheduler.Messages.size() == 1, "one changed Character emits one compact batch at the next 20 Hz tick");
		if (!Scheduler.Messages.empty()) {
			auto Decoded = DecodeCharacterMessage(Scheduler.Messages.front().Payload());
			auto *Frame = Decoded ? std::get_if<CharacterStateFrame>(&*Decoded) : nullptr;
			Check(
				Frame && Frame->StateCount == 1 && Frame->States[0].Character == Characters[20]->GetObjectId(),
				"stationary suppression retains deterministic per-Character newest state"
			);
			const auto QueuedCharacter = Characters[20]->GetObjectId();
			Characters[20]->Destroy();
			Characters[20].reset();
			Decoded = DecodeCharacterMessage(Scheduler.Messages.front().Payload());
			Frame = Decoded ? std::get_if<CharacterStateFrame>(&*Decoded) : nullptr;
			Check(
				Frame && Frame->StateCount == 1 && Frame->States[0].Character == QueuedCharacter,
				"queued state batches own their bytes across source Character destruction"
			);
		}

		RecordingScheduler SmallScheduler;
		CharacterNetworkConfiguration SmallFrames;
		SmallFrames.MaximumStateFrameBytes = 250;
		AuthoritativeCharacterNetwork SmallServer(SmallScheduler, TestLimits(), Movement, {}, SmallFrames);
		Check(SmallServer.AddPeer(Connection), "low-datagram fixture registers one peer");
		std::vector<std::shared_ptr<KinematicCharacter>> SmallCharacters;
		for (std::size_t Index = 0; Index < 10; ++Index) {
			auto Character = std::make_shared<KinematicCharacter>();
			SmallServer.RegisterCharacter(Character);
			SmallServer.MarkMaterialized(Connection, Character->GetObjectId(), StateChannelId(100 + Index));
			SmallCharacters.push_back(std::move(Character));
		}
		SmallServer.Step(World, 1);
		Check(SmallScheduler.Messages.size() == 4, "250-byte datagram policy splits ten states into four frames");
		for (const auto &Message : SmallScheduler.Messages)
			Check(Message.Payload().size() <= 250, "low negotiated datagram frames never rely on fragmentation");
	}

	void TestBatchLossAndRemoteInterpolation() {
		RecordingScheduler Scheduler;
		const ConnectionId Connection{41, 1};
		PredictedCharacterNetwork Client(Scheduler, TestLimits(), Movement);
		Check(Client.AddPeer(Connection), "remote interpolation fixture registers one server peer");
		const auto ReentryAction = Action(7, 44);
		Check(Client.RegisterAction(ReentryAction), "remote interpolation fixture registers its action content");
		const ObjectId SourceA{1000, 1};
		const ObjectId SourceB{1001, 1};
		auto ReplicaA = std::make_shared<KinematicCharacter>();
		auto ReplicaB = std::make_shared<KinematicCharacter>();
		Client.MarkMaterialized(SourceA, ReplicaA);
		Client.MarkMaterialized(SourceB, ReplicaB);
		WorldRoot World;
		auto MakeState = [](ObjectId Character, std::uint64_t Sequence, std::uint64_t Tick, float X) {
			return CharacterAuthoritativeState{
				.Character = Character,
				.ControlEpoch = CharacterControlEpoch(1),
				.StateSequence = RealtimeStateSequence(Sequence),
				.AuthoritativeTick = Tick,
				.Transform = CFrame(X, 6.0f, 0.0f),
			};
		};
		CharacterStateFrame Dropped{
			.ServerTick = 1,
			.FrameSequence = CharacterStateFrameSequence(1),
			.MaterializationEpoch = CharacterMaterializationEpoch(2),
			.StateCount = 1,
		};
		Dropped.States[0] = MakeState(SourceA, 1, 1, 1.0f);
		CharacterStateFrame Delivered{
			.ServerTick = 1,
			.FrameSequence = CharacterStateFrameSequence(2),
			.MaterializationEpoch = CharacterMaterializationEpoch(2),
			.StateCount = 1,
		};
		Delivered.States[0] = MakeState(SourceB, 1, 1, 2.0f);
		Check(
			Client.HandleTransportEvent(TransportEvent(StateFrameEvent(Connection, StateChannelId(2), Delivered))),
			"a later independently delivered batch is accepted when an earlier batch is lost"
		);
		Client.Reconcile(World);
		Check(
			Near(ReplicaA->GetPosition(), {0.0f, 6.0f, 0.0f}) && Near(ReplicaB->GetPosition(), {2.0f, 6.0f, 0.0f}),
			"batch loss affects only Characters in the lost batch"
		);

		CharacterStateFrame Mixed{
			.ServerTick = 4,
			.FrameSequence = CharacterStateFrameSequence(3),
			.MaterializationEpoch = CharacterMaterializationEpoch(2),
			.StateCount = 2,
		};
		Mixed.States[0] = MakeState(SourceA, 1, 4, 4.0f);
		Mixed.States[1] = MakeState(SourceB, 1, 4, 99.0f);
		Check(
			Client.HandleTransportEvent(TransportEvent(StateFrameEvent(Connection, StateChannelId(1), Mixed))),
			"a valid frame remains independently applicable when one enclosed Character state is stale"
		);
		Client.Reconcile(World);
		Check(
			Near(ReplicaA->GetPosition(), {4.0f, 6.0f, 0.0f}) && Near(ReplicaB->GetPosition(), {2.0f, 6.0f, 0.0f}),
			"per-Character sequence validation prevents a newer frame from reviving stale Character state"
		);

		for (const auto [Sequence, Tick, X] : std::array<std::array<std::uint64_t, 3>, 2>{
				 std::array<std::uint64_t, 3>{2, 7, 7}, std::array<std::uint64_t, 3>{3, 10, 10}
			 }) {
			CharacterStateFrame Frame{
				.ServerTick = Tick,
				.FrameSequence = CharacterStateFrameSequence(Sequence + 2),
				.MaterializationEpoch = CharacterMaterializationEpoch(2),
				.StateCount = 1,
			};
			Frame.States[0] = MakeState(SourceA, Sequence, Tick, static_cast<float>(X));
			Client.HandleTransportEvent(TransportEvent(StateFrameEvent(Connection, StateChannelId(1), Frame)));
			Client.Reconcile(World);
		}
		Check(
			Client.GetPresentationSnapshotCount(SourceA) == 3, "remote interpolation retains at most bounded samples"
		);
		Client.UpdatePresentation(10);
		Check(
			Near(ReplicaA->GetPosition(), {10.0f, 6.0f, 0.0f}) &&
				Near(ReplicaA->GetPresentationCFrame().Position, {4.0f, 6.0f, 0.0f}),
			"remote semantic state is newest immediately while presentation renders 100 ms behind"
		);
		Client.UpdatePresentation(13);
		Check(
			Near(ReplicaA->GetPresentationCFrame().Position, {7.0f, 6.0f, 0.0f}),
			"remote presentation interpolates between authoritative samples without moving semantic authority"
		);
		Client.UpdatePresentation(100);
		Check(
			Near(ReplicaA->GetPresentationCFrame().Position, {10.0f, 6.0f, 0.0f}),
			"remote presentation holds the newest state instead of extrapolating indefinitely"
		);
		Check(
			Client.MarkUnmaterialized(SourceA) && Client.GetPresentationSnapshotCount(SourceA) == 0,
			"unmaterialization clears remote interpolation and presentation state"
		);
		Check(Client.MarkMaterialized(SourceA, ReplicaA), "same ObjectId can begin a fresh materialization lifetime");
		CharacterStateFrame StaleMaterialization{
			.ServerTick = 13,
			.FrameSequence = CharacterStateFrameSequence(6),
			.MaterializationEpoch = CharacterMaterializationEpoch(2),
			.StateCount = 1,
		};
		StaleMaterialization.States[0] = MakeState(SourceA, 4, 13, 99.0f);
		const auto StaleBefore = Client.GetMetrics().StaleStatesDropped;
		Check(
			Client.HandleTransportEvent(
				TransportEvent(StateFrameEvent(Connection, StateChannelId(1), StaleMaterialization))
			),
			"delayed state from an earlier materialization lifetime is safely consumed"
		);
		Client.Reconcile(World);
		Check(
			Near(ReplicaA->GetPosition(), {10.0f, 6.0f, 0.0f}) &&
				Client.GetMetrics().StaleStatesDropped == StaleBefore + 1,
			"stale materialization epoch cannot mutate a reentered Character replica"
		);
		CharacterStateFrame ReentryState{
			.ServerTick = 16,
			.FrameSequence = CharacterStateFrameSequence(7),
			.MaterializationEpoch = CharacterMaterializationEpoch(4),
			.StateCount = 1,
		};
		ReentryState.States[0] = MakeState(SourceA, 5, 16, 16.0f);
		ReentryState.States[0].ResolvedAction = CharacterActionSequence(1);
		ReentryState.States[0].ActiveAction = CharacterActionState{
			CharacterActionSequence(1),
			ReentryAction.Token,
			ReentryAction.Animation,
			ReentryAction.ContentRevision,
			14,
			ReentryAction.DurationTicks,
		};
		Check(
			Client.HandleTransportEvent(TransportEvent(StateFrameEvent(Connection, StateChannelId(1), ReentryState))),
			"current state is accepted for the fresh materialization epoch"
		);
		Client.Reconcile(World);
		const auto ReenteredAction = Client.GetAuthoritativeAction(SourceA);
		const auto ReenteredPresentation = Client.GetPresentationAction(SourceA);
		Check(
			ReenteredAction && ReenteredAction->StartTick == 14 && ReenteredPresentation &&
				ReenteredPresentation->PhaseTick == 16 && !ReenteredPresentation->Predicted,
			"reentry establishes the authoritative action at its current phase instead of replaying its start"
		);
	}

	void TestLocalPresentationCorrectionMatrix() {
		struct Case {
			float Divergence;
			bool Teleport;
			bool Smooth;
			const char *Name;
		};
		const std::array Cases{
			Case{0.0f, false, false, "exact"},
			Case{0.01f, false, true, "one centimetre"},
			Case{0.05f, false, true, "five centimetres"},
			Case{0.25f, false, true, "collision divergence"},
			Case{1.0f, false, true, "one metre"},
			Case{8.0f, false, false, "eight metre reset"},
			Case{0.05f, true, false, "teleport"},
		};
		for (std::size_t Index = 0; Index < Cases.size(); ++Index) {
			const auto &Value = Cases[Index];
			RecordingScheduler Scheduler;
			const ConnectionId Connection{static_cast<std::uint32_t>(50 + Index), 1};
			PredictedCharacterNetwork Client(Scheduler, TestLimits(), Movement);
			Client.AddPeer(Connection);
			const ObjectId Source{static_cast<std::uint32_t>(2000 + Index), 1};
			auto Replica = std::make_shared<KinematicCharacter>();
			Replica->SetPosition({Value.Divergence, 6.0f, 0.0f});
			auto Root = std::make_shared<Part>();
			Root->SetParent(Replica);
			Replica->SetRootPart(Root);
			auto Anchor = std::make_shared<Attachment>();
			Anchor->SetParent(Root);
			Client.MarkMaterialized(Source, Replica);
			CharacterControlTransition Bind{Source, CharacterControlEpoch(2), StateChannelId(90), 1, true};
			auto BindBytes = EncodeCharacterMessage(CharacterMessage(Bind));
			Client.HandleTransportEvent(TransportEvent(
				ReceivedMessageEvent{
					Connection,
					DeliveryMode::ReliableOrdered,
					TrafficClass::Control,
					{},
					BindBytes ? std::move(*BindBytes) : std::vector<std::byte>{},
				}
			));
			CharacterStateFrame Frame{
				.ServerTick = 10,
				.FrameSequence = CharacterStateFrameSequence(1),
				.StateCount = 1,
			};
			Frame.States[0] = {
				.Character = Source,
				.ControlEpoch = CharacterControlEpoch(2),
				.StateSequence = RealtimeStateSequence(1),
				.AuthoritativeTick = 10,
				.Transform = CFrame(0.0f, 6.0f, 0.0f),
				.Flags = static_cast<std::uint8_t>(
					Value.Teleport ? CharacterStateFlag::Teleport : static_cast<CharacterStateFlag>(0)
				),
			};
			const auto Before = Client.GetMetrics();
			ChangeJournal::Get().Clear();
			Client.HandleTransportEvent(TransportEvent(StateFrameEvent(Connection, StateChannelId(90), Frame)));
			WorldRoot World;
			Client.Reconcile(World);
			const auto After = Client.GetMetrics();
			Check(Near(Replica->GetPosition(), {0.0f, 6.0f, 0.0f}), "semantic correction applies immediately");
			Check(
				Near(Root->GetCFrame().Position, {0.0f, 6.0f, 0.0f}) &&
					Near(Anchor->GetWorldCFrame().Position, {0.0f, 6.0f, 0.0f}),
				"RootPart and Attachment gameplay semantics remain at the corrected Character transform"
			);
			Check(
				Value.Smooth ? Near(Root->GetRenderCFrame().Position, {Value.Divergence, 6.0f, 0.0f})
							 : Near(Root->GetRenderCFrame().Position, {0.0f, 6.0f, 0.0f}),
				Value.Name
			);
			Check(
				After.LocalSmoothCorrections - Before.LocalSmoothCorrections == (Value.Smooth ? 1u : 0u),
				"only eligible correction magnitudes create presentation smoothing state"
			);
			Client.UpdatePresentation(10);
			Client.UpdatePresentation(16);
			Check(
				Near(Root->GetRenderCFrame().Position, {0.0f, 6.0f, 0.0f}),
				"local visual correction converges to semantic authority within 100 ms"
			);
			Check(
				ChangeJournal::Get().ReadSince(0).empty(),
				"semantic correction and presentation offsets create no authoring journal records"
			);
			Check(Client.MarkUnmaterialized(Source), "unmaterialization clears local correction presentation state");
		}
	}

	void TestCustomLuauPolicy() {
		auto Game = std::make_shared<DataModel>();
		ScriptEngine Engine(Game);
		Check(
			RunLua(Engine, R"(
function CharacterMovement(MoveX, MoveZ, DeltaSeconds)
	local WalkSpeed = 3
	return MoveX * WalkSpeed * DeltaSeconds, MoveZ * WalkSpeed * DeltaSeconds
end
function CharacterAction(RequestedToken)
	if RequestedToken == 9 then
		return 9
	end
	return nil
end
)") == LUA_OK,
			"custom game Luau Character policy compiles independently of DefaultLocomotion"
		);

		CharacterMovementPolicy LuauMovement = [&](const CharacterInputCommand &Command, const KinematicCharacter &) {
			lua_getglobal(Engine.L, "CharacterMovement");
			lua_pushnumber(Engine.L, Command.MoveIntent.x);
			lua_pushnumber(Engine.L, Command.MoveIntent.y);
			lua_pushnumber(Engine.L, Command.DeltaSeconds);
			if (lua_pcall(Engine.L, 3, 2, 0) != LUA_OK) throw std::runtime_error("custom movement policy failed");
			const auto Z = static_cast<float>(lua_tonumber(Engine.L, -1));
			const auto X = static_cast<float>(lua_tonumber(Engine.L, -2));
			lua_pop(Engine.L, 2);
			return CharacterMotionRequest{
				.Translation = {X, 0.0f, Z}, .Velocity = {X / Command.DeltaSeconds, 0.0f, Z / Command.DeltaSeconds}
			};
		};
		CharacterActionPolicy LuauAction = [&](ConnectionId, const CharacterActionRequest &Request) {
			lua_getglobal(Engine.L, "CharacterAction");
			lua_pushinteger(Engine.L, Request.RequestedActionToken);
			if (lua_pcall(Engine.L, 1, 1, 0) != LUA_OK) return std::optional<std::uint32_t>{};
			std::optional<std::uint32_t> Result;
			if (lua_isnumber(Engine.L, -1)) Result = static_cast<std::uint32_t>(lua_tointeger(Engine.L, -1));
			lua_pop(Engine.L, 1);
			return Result;
		};

		RecordingScheduler Scheduler;
		const ConnectionId Connection{33, 4};
		AuthoritativeCharacterNetwork Server(Scheduler, TestLimits(), LuauMovement, LuauAction);
		WorldRoot World;
		auto Character = std::make_shared<KinematicCharacter>();
		Character->SetPosition({0.0f, 3.0f, 0.0f});
		Check(
			Server.AddPeer(Connection) && Server.RegisterCharacter(Character) &&
				Server.MarkMaterialized(Connection, Character->GetObjectId(), StateChannelId(900)),
			"custom Luau fixture establishes native identity/relevance without default runtime assumptions"
		);
		Server.RegisterAction(
			{.Token = 9,
			 .Animation = AssetId::FromBuiltInName("CustomLuauLunge"),
			 .ContentRevision = Content(90),
			 .DurationTicks = 3,
			 .EvaluateRootMotion = [](std::uint64_t From, std::uint64_t To) -> std::optional<RootMotionDelta> {
				 return To > From ? std::optional(RootMotionDelta{.Translation = {0.25f, 0.0f, 0.0f}}) : std::nullopt;
			 }}
		);
		auto Epoch = Server.BindControl(Connection, Character->GetObjectId(), 1);
		Check(Epoch.has_value(), "custom Luau fixture receives native control bookkeeping");
		CharacterInputCommand Command{
			Character->GetObjectId(),
			Epoch.value_or(CharacterControlEpoch{}),
			CharacterInputSequence(1),
			2,
			1.0f / 60.0f,
			{1.0f, 0.0f},
			0.0f,
			0
		};
		auto InputBytes = EncodeCharacterMessage(CharacterMessage(Command));
		ReceivedMessageEvent InputEvent{
			Connection,
			DeliveryMode::UnreliableSequenced,
			TrafficClass::RealtimeState,
			RealtimeStateOrder{StateChannelId(900), RealtimeStateSequence(1)},
			InputBytes ? std::move(*InputBytes) : std::vector<std::byte>{}
		};
		Check(Server.HandleTransportEvent(TransportEvent(InputEvent)), "validated input reaches custom Luau policy");
		Server.Step(World, 2);
		Check(
			std::abs(Character->GetPosition().x - 0.05f) < 0.001f,
			"custom Luau policy, not native protocol code, selects authoritative walk speed"
		);

		CharacterActionRequest Request{
			Character->GetObjectId(), *Epoch, CharacterActionSequence(1), CharacterInputSequence(1), 9
		};
		auto ActionBytes = EncodeCharacterMessage(CharacterMessage(Request));
		ReceivedMessageEvent ActionEvent{
			Connection,
			DeliveryMode::ReliableOrdered,
			TrafficClass::ReliableApplication,
			{},
			ActionBytes ? std::move(*ActionBytes) : std::vector<std::byte>{}
		};
		Check(
			Server.HandleTransportEvent(TransportEvent(ActionEvent)),
			"reliable action request reaches custom Luau policy"
		);
		Server.Step(World, 3);
		Server.Step(World, 4);
		Check(
			Character->GetPosition().x > 0.2f,
			"custom Luau action selection starts server-known pinned root motion through native authority"
		);
	}
}

int main() {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
		TestCodec();
		TestPredictionAuthorityAndActions();
		TestServerAuthoritativePredictionMode();
		TestStaleControlRelevanceAndNpc();
		TestControlDeferralReconnectAndReplay();
		TestClientPolicyCannotChangeAuthority();
		TestLatestIntentFreshnessTimeout();
		TestPredictionBoundsAndContentMismatch();
		TestLossReorderAndLifecycle();
		TestRemotePresentationFaultMatrix();
		TestRepeatedManagerChurn();
		TestStateBatchingCadenceAndSuppression();
		TestBatchLossAndRemoteInterpolation();
		TestLocalPresentationCorrectionMatrix();
		TestCustomLuauPolicy();
	} catch (const std::exception &Error) {
		std::cerr << "UNCAUGHT: " << Error.what() << '\n';
		return 1;
	}
	if (Failures != 0) {
		std::cerr << Failures << " Character networking checks failed\n";
		return 1;
	}
	std::cout << "Character networking tests passed\n";
	return 0;
}
