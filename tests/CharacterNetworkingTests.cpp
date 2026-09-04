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

#include <algorithm>
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
		std::vector<SchedulerSubmitResult> SubmissionResults;
		std::size_t SubmissionIndex = 0;
		SchedulerSubmitResult NextSubmission{SchedulerSubmitStatus::Accepted};
		bool RegisterConnection(ConnectionId, const NetworkLimits &) override {
			return true;
		}
		SchedulerSubmitResult Submit(NetworkMessageIntent Intent) override {
			const auto Result = SubmissionIndex < SubmissionResults.size() ? SubmissionResults[SubmissionIndex++]
																		   : NextSubmission;
			if (Result.Accepted()) Messages.push_back(std::move(Intent));
			return Result;
		}
		void ScriptSubmissions(std::vector<SchedulerSubmitResult> Results) {
			SubmissionResults = std::move(Results);
			SubmissionIndex = 0;
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
		std::array<CharacterMessage, 6> Messages{
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
			CharacterActionResult{
				.Character = Character,
				.ControlEpoch = CharacterControlEpoch(2),
				.ActionSequence = CharacterActionSequence(4),
				.RequestedActionToken = 12,
				.Accepted = true,
				.AuthoritativeAction =
					CharacterActionState{
						CharacterActionSequence(4), 12, Definition.Animation, Definition.ContentRevision, 40, 4
					},
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
			.MaterializationEpoch = CharacterMaterializationEpoch(0x1'0001),
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
			"GCHR v4 batches two compact absolute states with a widened generation"
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
					"every GCHR v4 truncation boundary fails closed"
				);
			auto Duplicate = *FrameBytes;
			std::copy_n(Duplicate.begin() + 34, 8, Duplicate.begin() + 108);
			Check(!DecodeCharacterMessage(Duplicate), "GCHR v4 rejects duplicate Character identities");
			auto BadCount = *FrameBytes;
			BadCount[24] = std::byte{16};
			BadCount[25] = std::byte{0};
			Check(!DecodeCharacterMessage(BadCount), "GCHR v4 rejects excessive state counts before decode");
			auto BadQuantizedRotation = *FrameBytes;
			BadQuantizedRotation[86] = std::byte{0};
			BadQuantizedRotation[87] = std::byte{0x80};
			Check(!DecodeCharacterMessage(BadQuantizedRotation), "reserved int16 minimum quantized values fail closed");
			auto BadFlags = *FrameBytes;
			BadFlags[106] = std::byte{0x80};
			Check(!DecodeCharacterMessage(BadFlags), "undefined compact Character state flags fail closed");
			auto Trailing = *FrameBytes;
			Trailing.push_back(std::byte{0});
			Check(!DecodeCharacterMessage(Trailing), "GCHR v4 rejects trailing bytes");
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
			MaximumBytes && MaximumBytes->size() == 1144 && MaximumBytes->size() <= MaximumCharacterStateFrameBytes,
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
		for (int Index = 0; Index < 4; ++Index)
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
				"every scheduler submission is one bounded GCHR v4 state frame"
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
				Metrics.StateFramesEmitted == 3 && Metrics.BatchSplits == 2 && Metrics.PublicationStatesDeferred == 0,
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
		Check(SmallScheduler.Messages.size() == 5, "250-byte datagram policy splits ten states into five frames");
		for (const auto &Message : SmallScheduler.Messages)
			Check(Message.Payload().size() <= 250, "low negotiated datagram frames never rely on fragmentation");
	}

	void TestImportanceCadenceAndLifecycle() {
		CharacterNetworkConfiguration InvalidCadence;
		InvalidCadence.ReducedStateUpdatesPerSecond = 30;
		Check(!InvalidCadence.IsValid(), "reduced cadence cannot exceed the full-rate cadence");
		CharacterNetworkConfiguration InvalidExtrapolation;
		InvalidExtrapolation.RemoteExtrapolationLimitTicks = MaximumRemoteCharacterSnapshotWindowTicks + 1;
		Check(!InvalidExtrapolation.IsValid(), "remote extrapolation is bounded by the snapshot lifetime");
		CharacterNetworkConfiguration InvalidBands;
		InvalidBands.ImportanceHysteresis = InvalidBands.FullRateDistance;
		Check(!InvalidBands.IsValid(), "importance bands require a non-overlapping hysteresis policy");
		RecordingScheduler OfflineScheduler;
		AuthoritativeCharacterNetwork OfflineServer(OfflineScheduler, TestLimits(), Movement);
		WorldRoot OfflineWorld;
		auto OfflineCharacter = std::make_shared<KinematicCharacter>();
		Check(
			OfflineServer.RegisterCharacter(OfflineCharacter), "offline fixture registers an authoritative Character"
		);
		for (std::uint64_t Tick = 1; Tick <= 60; ++Tick) {
			auto Transform = OfflineCharacter->GetCFrame();
			Transform.Position.x += 0.1f;
			OfflineCharacter->ApplyRuntimeTransform(Transform);
			OfflineServer.Step(OfflineWorld, Tick);
		}
		const auto OfflineMetrics = OfflineServer.GetMetrics();
		Check(
			OfflineScheduler.Messages.empty() && OfflineMetrics.ImportanceEvaluations == 0 &&
				OfflineMetrics.DueStates == 0 && OfflineMetrics.StateSnapshotsBuilt == 0 &&
				OfflineMetrics.ImportanceEvaluationCpuNanoseconds == 0 && OfflineMetrics.DueSetCpuNanoseconds == 0,
			"zero-peer offline simulation creates no importance, due-set, snapshot, or network work"
		);

		RecordingScheduler FullSimulationScheduler;
		RecordingScheduler LowSimulationScheduler;
		AuthoritativeCharacterNetwork FullSimulationServer(FullSimulationScheduler, TestLimits(), Movement);
		AuthoritativeCharacterNetwork LowSimulationServer(LowSimulationScheduler, TestLimits(), Movement);
		const ConnectionId FullSimulationPeer{138, 1};
		const ConnectionId LowSimulationPeer{139, 1};
		auto FullSimulationCharacter = std::make_shared<KinematicCharacter>();
		auto LowSimulationCharacter = std::make_shared<KinematicCharacter>();
		WorldRoot FullSimulationWorld;
		WorldRoot LowSimulationWorld;
		Check(
			FullSimulationServer.AddPeer(FullSimulationPeer) &&
				FullSimulationServer.RegisterCharacter(FullSimulationCharacter) &&
				FullSimulationServer.MarkMaterialized(
					FullSimulationPeer, FullSimulationCharacter->GetObjectId(), StateChannelId(138)
				) &&
				FullSimulationServer.SetPeerPublicationFocus(FullSimulationPeer, std::array{glm::vec3{}}) &&
				LowSimulationServer.AddPeer(LowSimulationPeer) &&
				LowSimulationServer.RegisterCharacter(LowSimulationCharacter) &&
				LowSimulationServer.MarkMaterialized(
					LowSimulationPeer, LowSimulationCharacter->GetObjectId(), StateChannelId(139)
				) &&
				LowSimulationServer.SetPeerPublicationFocus(
					LowSimulationPeer, std::array{glm::vec3{-1000.0f, 0.0f, 0.0f}}
				),
			"simulation-separation fixture creates equivalent full-rate and low-rate NPCs"
		);
		Check(
			FullSimulationServer.RegisterAction(Action(12, 56, 0.125f)) &&
				LowSimulationServer.RegisterAction(Action(12, 56, 0.125f)) &&
				FullSimulationServer.StartServerAction(FullSimulationCharacter->GetObjectId(), 12, 1) &&
				LowSimulationServer.StartServerAction(LowSimulationCharacter->GetObjectId(), 12, 1),
			"simulation-separation fixture starts identical authoritative root motion"
		);
		for (std::uint64_t Tick = 1; Tick <= 18; ++Tick) {
			FullSimulationServer.Step(FullSimulationWorld, Tick);
			LowSimulationServer.Step(LowSimulationWorld, Tick);
			Check(
				Near(FullSimulationCharacter->GetPosition(), LowSimulationCharacter->GetPosition()),
				"publication tier never changes authoritative Character simulation"
			);
		}
		Check(
			FullSimulationServer.GetPublicationTier(FullSimulationPeer, FullSimulationCharacter->GetObjectId()) ==
					CharacterPublicationTier::FullRate &&
				LowSimulationServer.GetPublicationTier(LowSimulationPeer, LowSimulationCharacter->GetObjectId()) ==
					CharacterPublicationTier::LowRate,
			"identical authoritative transforms remain independent of different final publication tiers"
		);
		RecordingScheduler Scheduler;
		const ConnectionId PeerA{140, 1};
		const ConnectionId PeerB{141, 1};
		AuthoritativeCharacterNetwork Server(Scheduler, TestLimits(), Movement);
		WorldRoot World;
		Check(Server.AddPeer(PeerA) && Server.AddPeer(PeerB), "3F importance fixture registers two peers");

		auto MakeCharacter = [](float X) {
			auto Character = std::make_shared<KinematicCharacter>();
			Character->SetPosition({X, 6.0f, 0.0f});
			return Character;
		};
		auto NearCharacter = MakeCharacter(24.0f);
		auto ReducedCharacter = MakeCharacter(100.0f);
		auto LowCharacter = MakeCharacter(220.0f);
		auto OwnerCharacter = MakeCharacter(400.0f);
		const std::array Characters{NearCharacter, ReducedCharacter, LowCharacter, OwnerCharacter};
		for (std::size_t Index = 0; Index < Characters.size(); ++Index)
			Check(
				Server.RegisterCharacter(Characters[Index]) &&
					Server.MarkMaterialized(PeerA, Characters[Index]->GetObjectId(), StateChannelId(200 + Index)),
				"3F importance fixture materializes each relationship"
			);
		Check(
			Server.MarkMaterialized(PeerB, LowCharacter->GetObjectId(), StateChannelId(300)) &&
				Server.SetPeerPublicationFocus(PeerA, std::array{glm::vec3{0.0f, 6.0f, 0.0f}}) &&
				Server.SetPeerPublicationFocus(PeerB, std::array{glm::vec3{220.0f, 6.0f, 0.0f}}) &&
				Server.BindControl(PeerA, OwnerCharacter->GetObjectId(), 1).has_value(),
			"3F uses trusted multi-peer focus while preserving the owner override"
		);
		Check(
			!Server.SetPeerPublicationFocus(
				PeerA,
				std::array{
					glm::vec3{},
					glm::vec3{},
					glm::vec3{},
					glm::vec3{},
					glm::vec3{},
				}
			),
			"publication focus input is capped independently of materialized character count"
		);
		Check(
			!Server.SetPeerPublicationFocus(
				PeerA, std::array{glm::vec3{std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f}}
			),
			"non-finite trusted publication focus fails closed"
		);
		Check(Server.RegisterAction(Action(11, 55, 0.25f)), "3F importance fixture registers semantic action content");

		for (std::uint64_t Tick = 1; Tick <= 18; ++Tick) {
			if (Tick > 1)
				for (const auto &Character : Characters) {
					auto Transform = Character->GetCFrame();
					Transform.Position.x += 0.01f;
					Character->ApplyRuntimeTransform(Transform);
				}
			Server.Step(World, Tick);
		}
		Check(
			Server.GetPublicationTier(PeerA, NearCharacter->GetObjectId()) == CharacterPublicationTier::FullRate &&
				Server.GetPublicationTier(PeerA, ReducedCharacter->GetObjectId()) ==
					CharacterPublicationTier::ReducedRate &&
				Server.GetPublicationTier(PeerA, LowCharacter->GetObjectId()) == CharacterPublicationTier::LowRate &&
				Server.GetPublicationTier(PeerA, OwnerCharacter->GetObjectId()) == CharacterPublicationTier::FullRate &&
				Server.GetPublicationTier(PeerB, LowCharacter->GetObjectId()) == CharacterPublicationTier::FullRate,
			"importance is private per peer, distance-tiered, and owner authoritative"
		);

		Scheduler.Messages.clear();
		for (std::uint64_t Tick = 19; Tick <= 78; ++Tick) {
			for (const auto &Character : Characters) {
				auto Transform = Character->GetCFrame();
				Transform.Position.x += 0.01f;
				Character->ApplyRuntimeTransform(Transform);
			}
			Server.Step(World, Tick);
		}
		std::map<std::pair<ConnectionId, ObjectId>, std::uint64_t> StateCounts;
		for (const auto &Message : Scheduler.Messages) {
			auto Decoded = DecodeCharacterMessage(Message.Payload());
			auto *Frame = Decoded ? std::get_if<CharacterStateFrame>(&*Decoded) : nullptr;
			if (!Frame) continue;
			for (const auto &State : Frame->GetStates())
				++StateCounts[{Message.Destination(), State.Character}];
		}
		const bool CadenceCountsMatch = StateCounts[{PeerA, NearCharacter->GetObjectId()}] == 20 &&
										StateCounts[{PeerA, ReducedCharacter->GetObjectId()}] == 10 &&
										StateCounts[{PeerA, LowCharacter->GetObjectId()}] == 5 &&
										StateCounts[{PeerA, OwnerCharacter->GetObjectId()}] == 20 &&
										StateCounts[{PeerB, LowCharacter->GetObjectId()}] == 20;
		if (!CadenceCountsMatch)
			std::cerr << "[Character:NetworkTest] cadence=" << StateCounts[{PeerA, NearCharacter->GetObjectId()}] << ','
					  << StateCounts[{PeerA, ReducedCharacter->GetObjectId()}] << ','
					  << StateCounts[{PeerA, LowCharacter->GetObjectId()}] << ','
					  << StateCounts[{PeerA, OwnerCharacter->GetObjectId()}] << ','
					  << StateCounts[{PeerB, LowCharacter->GetObjectId()}] << '\n';
		Check(CadenceCountsMatch, "deterministic phases deliver exact 20/10/5 Hz moving-state cadence over one second");

		Check(
			Server.SetPeerPublicationFocus(PeerA, std::array{glm::vec3{70.0f, 6.0f, 0.0f}}),
			"hysteresis fixture moves trusted focus without exposing a client tier API"
		);
		Server.Step(World, 79);
		Check(
			Server.GetPublicationTier(PeerA, LowCharacter->GetObjectId()) == CharacterPublicationTier::LowRate,
			"low tier remains stable inside the sixteen-unit hysteresis band"
		);
		Scheduler.Messages.clear();
		Server.SetPeerPublicationFocus(PeerA, std::array{glm::vec3{77.0f, 6.0f, 0.0f}});
		auto Rotated = LowCharacter->GetCFrame();
		Rotated.Rotation = CFrame::Angles(0.0f, 0.1f, 0.0f).Rotation;
		LowCharacter->ApplyRuntimeTransform(Rotated);
		for (std::uint64_t Tick = 80; Tick <= 85; ++Tick)
			Server.Step(World, Tick);
		const bool ReducedStateRepublished = std::ranges::any_of(
			Scheduler.Messages, [PeerA, Low = LowCharacter->GetObjectId()](const NetworkMessageIntent &Message) {
				if (Message.Destination() != PeerA) return false;
				auto Decoded = DecodeCharacterMessage(Message.Payload());
				auto *Frame = Decoded ? std::get_if<CharacterStateFrame>(&*Decoded) : nullptr;
				return Frame &&
					   std::ranges::any_of(Frame->GetStates(), [Low](const CharacterAuthoritativeState &State) {
						   return State.Character == Low;
					   });
			}
		);
		if (!ReducedStateRepublished)
			std::cerr << "[Character:NetworkTest] promotion_messages=" << Scheduler.Messages.size() << '\n';
		Check(
			Server.GetPublicationTier(PeerA, LowCharacter->GetObjectId()) == CharacterPublicationTier::ReducedRate &&
				ReducedStateRepublished,
			"crossing the hysteresis boundary republishes changed state by the next reduced-rate phase"
		);
		Server.SetPeerPublicationFocus(PeerA, std::array{glm::vec3{-1000.0f, 6.0f, 0.0f}, LowCharacter->GetPosition()});
		Server.Step(World, 86);
		Check(
			Server.GetPublicationTier(PeerA, LowCharacter->GetObjectId()) == CharacterPublicationTier::FullRate,
			"the minimum of multiple trusted focus points determines importance"
		);

		Server.SetPeerPublicationFocus(PeerA, std::array{glm::vec3{-1000.0f, 6.0f, 0.0f}});
		Server.Step(World, 87);
		Check(
			Server.GetPublicationTier(PeerA, LowCharacter->GetObjectId()) == CharacterPublicationTier::LowRate,
			"a relationship can demote directly from full to low when safely beyond both bands"
		);
		Scheduler.Messages.clear();
		Check(Server.StartServerAction(LowCharacter->GetObjectId(), 11, 88), "low-tier action begins authoritatively");
		Server.Step(World, 89);
		Check(
			std::ranges::any_of(
				Scheduler.Messages,
				[PeerA](const NetworkMessageIntent &Message) {
					if (Message.Destination() != PeerA || Message.Delivery() != DeliveryMode::ReliableOrdered)
						return false;
					auto Decoded = DecodeCharacterMessage(Message.Payload());
					auto *Frame = Decoded ? std::get_if<CharacterStateFrame>(&*Decoded) : nullptr;
					return Frame && Frame->StateCount == 1 && Frame->States[0].ActiveAction.has_value();
				}
			),
			"reliable action semantics bypass low-rate publication cadence"
		);
		Scheduler.Messages.clear();
		auto Teleport = LowCharacter->GetCFrame();
		Teleport.Position.x += 100.0f;
		LowCharacter->ApplyRuntimeTransform(Teleport);
		Server.Step(World, 90);
		Check(
			std::ranges::any_of(
				Scheduler.Messages,
				[PeerA](const NetworkMessageIntent &Message) {
					if (Message.Destination() != PeerA || Message.Delivery() != DeliveryMode::ReliableOrdered)
						return false;
					auto Decoded = DecodeCharacterMessage(Message.Payload());
					auto *Frame = Decoded ? std::get_if<CharacterStateFrame>(&*Decoded) : nullptr;
					return Frame && Frame->StateCount == 1 && Frame->States[0].Teleport();
				}
			),
			"authoritative discontinuities bypass cadence and carry the existing teleport semantic"
		);

		Check(
			Server.MarkUnmaterialized(PeerA, LowCharacter->GetObjectId()) &&
				!Server.GetPublicationTier(PeerA, LowCharacter->GetObjectId()),
			"unmaterialization destroys private publication history"
		);
		Scheduler.Messages.clear();
		Check(
			Server.MarkMaterialized(PeerA, LowCharacter->GetObjectId(), StateChannelId(333)),
			"reentry creates a fresh bounded relationship"
		);
		Server.Step(World, 91);
		Check(
			std::ranges::any_of(
				Scheduler.Messages,
				[PeerA](const NetworkMessageIntent &Message) { return Message.Destination() == PeerA; }
			),
			"reentry promptly establishes a fresh authoritative baseline"
		);
		const auto Metrics = Server.GetMetrics();
		if (!(Metrics.ImportanceEvaluations != 0 && Metrics.ImportanceTierTransitions != 0 &&
			  Metrics.FullRateStatesSent != 0 && Metrics.ReducedRateStatesSent != 0 && Metrics.LowRateStatesSent != 0 &&
			  Metrics.ForcedSemanticPublications >= 2 &&
			  Metrics.MaximumStateAgeTicks <= DefaultCharacterAbsoluteRefreshTicks))
			std::cerr << "[Character:NetworkTest] metrics=" << Metrics.FullRateStatesSent << ','
					  << Metrics.ReducedRateStatesSent << ',' << Metrics.LowRateStatesSent << ','
					  << Metrics.ForcedSemanticPublications << ',' << Metrics.MaximumStateAgeTicks << '\n';
		Check(
			Metrics.ImportanceEvaluations != 0 && Metrics.ImportanceTierTransitions != 0 &&
				Metrics.FullRateStatesSent != 0 && Metrics.ReducedRateStatesSent != 0 &&
				Metrics.LowRateStatesSent != 0 && Metrics.ForcedSemanticPublications >= 2 &&
				Metrics.MaximumStateAgeTicks <= DefaultCharacterAbsoluteRefreshTicks,
			"3F diagnostics expose tier work, bytes, state age, promotions, and semantic bypasses"
		);

		RecordingScheduler ChurnScheduler;
		AuthoritativeCharacterNetwork ChurnServer(ChurnScheduler, TestLimits(), Movement);
		auto ChurnCharacter = MakeCharacter(200.0f);
		Check(ChurnServer.RegisterCharacter(ChurnCharacter), "100-peer churn fixture registers its shared Character");
		for (std::uint32_t Index = 1; Index <= 100; ++Index) {
			const ConnectionId Connection{400 + Index, 1};
			Check(
				ChurnServer.AddPeer(Connection) &&
					ChurnServer.SetPeerPublicationFocus(Connection, std::array{glm::vec3{}}) &&
					ChurnServer.MarkMaterialized(Connection, ChurnCharacter->GetObjectId(), StateChannelId(Index)),
				"100-peer churn creates bounded publication state"
			);
			ChurnServer.Step(World, 100 + Index);
			Check(ChurnServer.RemovePeer(Connection, 100 + Index), "100-peer churn destroys publication state");
			ChurnScheduler.Messages.clear();
		}
		const auto ChurnMetrics = ChurnServer.GetMetrics();
		Check(
			ChurnMetrics.FullRateRelationships == 0 && ChurnMetrics.ReducedRateRelationships == 0 &&
				ChurnMetrics.LowRateRelationships == 0,
			"one hundred connect/disconnect lifetimes leave no peer-tier relationship residue"
		);
	}

	void TestBoundedPublicationScheduling() {
		CharacterNetworkConfiguration InvalidPerPeer;
		InvalidPerPeer.MaximumPublicationStatesPerPeerTick = 0;
		Check(!InvalidPerPeer.IsValid(), "a zero production per-peer Character publication budget fails closed");
		CharacterNetworkConfiguration InvalidGlobal;
		InvalidGlobal.MaximumPublicationStatesPerTick = 0;
		Check(!InvalidGlobal.IsValid(), "a zero production global Character publication budget fails closed");
		CharacterNetworkConfiguration ContradictoryBudgets;
		ContradictoryBudgets.MaximumPublicationStatesPerTick =
			ContradictoryBudgets.MaximumPublicationStatesPerPeerTick - 1;
		Check(
			!ContradictoryBudgets.IsValid(),
			"the per-peer Character publication bound cannot exceed the global bound"
		);
		CharacterNetworkConfiguration InvalidQuantum;
		InvalidQuantum.PublicationPeerQuantum = InvalidQuantum.MaximumPublicationStatesPerPeerTick + 1;
		Check(!InvalidQuantum.IsValid(), "the peer quantum cannot exceed its per-peer publication bound");
		CharacterNetworkConfiguration OversizedBudget;
		OversizedBudget.MaximumPublicationStatesPerTick = MaximumCharacterPublicationStatesPerTick + 1;
		Check(!OversizedBudget.IsValid(), "an oversized Character publication budget fails closed");

		CharacterNetworkConfiguration Configuration;
		Configuration.MaximumPublicationStatesPerPeerTick = 3;
		Configuration.MaximumPublicationStatesPerTick = 6;
		Configuration.PublicationPeerQuantum = 2;
		RecordingScheduler Scheduler;
		AuthoritativeCharacterNetwork Server(Scheduler, TestLimits(), Movement, {}, Configuration);
		WorldRoot World;
		const std::array Connections{ConnectionId{600, 1}, ConnectionId{900, 1}, ConnectionId{1'200, 1}};
		const std::array<std::size_t, 3> GroupSizes{9, 2, 4};
		std::array<std::vector<std::shared_ptr<KinematicCharacter>>, 3> Groups;
		std::vector<std::shared_ptr<KinematicCharacter>> Characters;
		std::vector<std::pair<ConnectionId, ObjectId>> Relationships;
		for (std::size_t Group = 0; Group < Groups.size(); ++Group) {
			Check(Server.AddPeer(Connections[Group]), "the constrained fairness fixture registers each peer");
			for (std::size_t Index = 0; Index < GroupSizes[Group]; ++Index) {
				auto Character = std::make_shared<KinematicCharacter>();
				Character->SetPosition({static_cast<float>(Group * 100 + Index), 6.0f, 0.0f});
				const auto Id = Character->GetObjectId();
				Check(
					Server.RegisterCharacter(Character) &&
						Server.MarkMaterialized(Connections[Group], Id, StateChannelId(100 + Group * 20 + Index)),
					"the constrained fairness fixture creates each sparse relationship"
				);
				Groups[Group].push_back(Character);
				Characters.push_back(std::move(Character));
				Relationships.emplace_back(Connections[Group], Id);
			}
			Check(
				Server.BindControl(Connections[Group], Groups[Group].front()->GetObjectId(), 1).has_value(),
				"each constrained peer binds one authoritative owner relationship"
			);
		}
		Server.Step(World, 1);
		std::size_t InitialStates = 0;
		for (const auto &Message : Scheduler.Messages) {
			auto Decoded = DecodeCharacterMessage(Message.Payload());
			if (auto *Frame = Decoded ? std::get_if<CharacterStateFrame>(&*Decoded) : nullptr)
				InitialStates += Frame->StateCount;
		}
		Check(
			InitialStates == Relationships.size(),
			"materialization and control baselines bypass the ordinary six-state publication budget"
		);

		std::map<std::pair<ConnectionId, ObjectId>, std::uint64_t> LastPublication;
		std::map<std::pair<ConnectionId, ObjectId>, std::uint64_t> MaximumGap;
		std::map<std::pair<ConnectionId, ObjectId>, std::uint64_t> PublicationCount;
		for (const auto &Relationship : Relationships) {
			LastPublication[Relationship] = 1;
			PublicationCount[Relationship] = 1;
		}
		Scheduler.Messages.clear();
		for (std::uint64_t Tick = 2; Tick <= 72; ++Tick) {
			for (const auto &Character : Characters) {
				auto Transform = Character->GetCFrame();
				Transform.Position.z += 0.02f;
				Character->ApplyRuntimeTransform(Transform);
			}
			Scheduler.Messages.clear();
			Server.Step(World, Tick);
			std::size_t StatesThisTick = 0;
			std::map<ConnectionId, std::size_t> PeerStatesThisTick;
			for (const auto &Message : Scheduler.Messages) {
				if (Message.Delivery() != DeliveryMode::UnreliableSequenced) continue;
				auto Decoded = DecodeCharacterMessage(Message.Payload());
				auto *Frame = Decoded ? std::get_if<CharacterStateFrame>(&*Decoded) : nullptr;
				if (!Frame) continue;
				StatesThisTick += Frame->StateCount;
				PeerStatesThisTick[Message.Destination()] += Frame->StateCount;
				for (const auto &State : Frame->GetStates()) {
					const auto Key = std::pair{Message.Destination(), State.Character};
					auto Previous = LastPublication.find(Key);
					Check(
						Previous != LastPublication.end(),
						"publication never escapes its materialized peer relationship"
					);
					if (Previous == LastPublication.end()) continue;
					MaximumGap[Key] = std::max(MaximumGap[Key], Tick - Previous->second);
					Previous->second = Tick;
					++PublicationCount[Key];
				}
			}
			Check(StatesThisTick <= 6, "expensive recurring publication work obeys the global state-count cap");
			for (const auto &[Connection, Count] : PeerStatesThisTick) {
				(void)Connection;
				Check(Count <= 3, "one dense peer cannot exceed its per-peer publication cap");
			}
		}
		for (const auto &Relationship : Relationships)
			Check(PublicationCount[Relationship] > 1, "long-running overload does not starve any relationship");
		for (std::size_t Group = 0; Group < Groups.size(); ++Group) {
			const auto Owner = std::pair{Connections[Group], Groups[Group].front()->GetObjectId()};
			Check(MaximumGap[Owner] <= 3, "owner correction keeps its desired three-tick bound when feasible");
		}
		Check(
			MaximumGap[std::pair{Connections[1], Groups[1].back()->GetObjectId()}] <= 6 &&
				MaximumGap[std::pair{Connections[2], Groups[2].back()->GetObjectId()}] <= 6,
			"sparse later-ID peers retain the documented one-interval overload defer bound"
		);
		const auto FairnessMetrics = Server.GetMetrics();
		Check(
			FairnessMetrics.PublicationStatesDeferred != 0 && FairnessMetrics.PublicationGlobalBudgetExhaustions != 0 &&
				FairnessMetrics.PublicationOfferedStates > FairnessMetrics.PublicationStatesSelected &&
				FairnessMetrics.PublicationOwnerDeferrals == 0 && FairnessMetrics.MaximumPublicationLatencyTicks <= 3 &&
				FairnessMetrics.PublicationStatesSelected == FairnessMetrics.PublicationStatesAccepted,
			"overload metrics distinguish budget deferral while accepted ordinary work commits atomically"
		);

		CharacterNetworkConfiguration RotationConfiguration;
		RotationConfiguration.MaximumPublicationStatesPerPeerTick = 1;
		RotationConfiguration.MaximumPublicationStatesPerTick = 1;
		RotationConfiguration.PublicationPeerQuantum = 1;
		RecordingScheduler RotationScheduler;
		AuthoritativeCharacterNetwork RotationServer(
			RotationScheduler, TestLimits(), Movement, {}, RotationConfiguration
		);
		std::array<std::shared_ptr<KinematicCharacter>, 3> RotationCharacters;
		const std::array RotationPeers{ConnectionId{10, 1}, ConnectionId{500, 1}, ConnectionId{2'000, 1}};
		for (std::size_t Index = 0; Index < RotationPeers.size(); ++Index) {
			RotationCharacters[Index] = std::make_shared<KinematicCharacter>();
			Check(
				RotationServer.AddPeer(RotationPeers[Index]) &&
					RotationServer.RegisterCharacter(RotationCharacters[Index]) &&
					RotationServer.MarkMaterialized(
						RotationPeers[Index], RotationCharacters[Index]->GetObjectId(), StateChannelId(250 + Index)
					),
				"the under-capacity rotation fixture creates adversarial early and late peer identities"
			);
		}
		RotationServer.Step(World, 1);
		std::map<ConnectionId, std::size_t> RotationPublications;
		for (std::uint64_t Tick = 2; Tick <= 12; ++Tick) {
			for (const auto &Character : RotationCharacters) {
				auto Transform = Character->GetCFrame();
				Transform.Position.y += 0.01f;
				Character->ApplyRuntimeTransform(Transform);
			}
			RotationScheduler.Messages.clear();
			RotationServer.Step(World, Tick);
			for (const auto &Message : RotationScheduler.Messages)
				if (Message.Delivery() == DeliveryMode::UnreliableSequenced)
					++RotationPublications[Message.Destination()];
		}
		Check(
			std::ranges::all_of(
				RotationPeers, [&](ConnectionId Connection) { return RotationPublications[Connection] != 0; }
			),
			"a global budget smaller than the peer count rotates the skipped peer instead of fixing winners"
		);

		CharacterNetworkConfiguration LifecycleConfiguration;
		LifecycleConfiguration.MaximumPublicationStatesPerPeerTick = 1;
		LifecycleConfiguration.MaximumPublicationStatesPerTick = 1;
		LifecycleConfiguration.PublicationPeerQuantum = 1;
		RecordingScheduler LifecycleScheduler;
		AuthoritativeCharacterNetwork LifecycleServer(
			LifecycleScheduler, TestLimits(), Movement, {}, LifecycleConfiguration
		);
		const ConnectionId LifecyclePeer{1'300, 1};
		std::array<std::shared_ptr<KinematicCharacter>, 3> LifecycleCharacters;
		Check(LifecycleServer.AddPeer(LifecyclePeer), "the deferred lifecycle fixture registers its peer");
		for (std::size_t Index = 0; Index < LifecycleCharacters.size(); ++Index) {
			LifecycleCharacters[Index] = std::make_shared<KinematicCharacter>();
			Check(
				LifecycleServer.RegisterCharacter(LifecycleCharacters[Index]) &&
					LifecycleServer.MarkMaterialized(
						LifecyclePeer, LifecycleCharacters[Index]->GetObjectId(), StateChannelId(300 + Index)
					),
				"the deferred lifecycle fixture materializes each relationship"
			);
		}
		LifecycleServer.Step(World, 1);
		for (std::uint64_t Tick = 2; Tick <= 3; ++Tick) {
			for (const auto &Character : LifecycleCharacters) {
				auto Transform = Character->GetCFrame();
				Transform.Position.x += 0.01f;
				Character->ApplyRuntimeTransform(Transform);
			}
			LifecycleServer.Step(World, Tick);
		}
		std::vector<ObjectId> Deferred;
		for (const auto &Character : LifecycleCharacters)
			if (auto State = LifecycleServer.GetPublicationSchedulingState(LifecyclePeer, Character->GetObjectId());
				State && State->Due)
				Deferred.push_back(Character->GetObjectId());
		Check(Deferred.size() == 2, "a one-state cap leaves exactly two compact overdue relationships");
		if (Deferred.size() == 2) {
			Check(
				LifecycleServer.MarkUnmaterialized(LifecyclePeer, Deferred[0]),
				"relevance leave removes a deferred relationship directly from its intrusive queue"
			);
			for (const auto &Character : LifecycleCharacters)
				if (Character->GetObjectId() == Deferred[1]) Character->Destroy();
			LifecycleScheduler.Messages.clear();
			LifecycleServer.Step(World, 4);
			Check(
				!LifecycleServer.GetPublicationSchedulingState(LifecyclePeer, Deferred[0]) &&
					!LifecycleServer.GetPublicationSchedulingState(LifecyclePeer, Deferred[1]),
				"relevance leave and destruction invalidate deferred work without a stale publication"
			);
			Check(
				LifecycleServer.MarkMaterialized(LifecyclePeer, Deferred[0], StateChannelId(399)),
				"re-entry creates a fresh materialization lifetime without inherited deadline debt"
			);
			LifecycleServer.Step(World, 5);
			auto Reentered = LifecycleServer.GetPublicationSchedulingState(LifecyclePeer, Deferred[0]);
			Check(
				Reentered && Reentered->HasPublished && Reentered->LastAcceptedPublicationTick == 5,
				"re-entry receives a required current baseline before ordinary scheduling resumes"
			);
		}
		Check(
			LifecycleServer.RemovePeer(LifecyclePeer, 6) &&
				!LifecycleServer.GetPublicationSchedulingState(
					LifecyclePeer, LifecycleCharacters.front()->GetObjectId()
				),
			"disconnect clears all peer-local wheel, due, defer, and rotation state"
		);

		RecordingScheduler RejectionScheduler;
		CharacterNetworkConfiguration RejectionConfiguration;
		RejectionConfiguration.MaximumPublicationStatesPerPeerTick = 16;
		RejectionConfiguration.MaximumPublicationStatesPerTick = 16;
		RejectionConfiguration.PublicationPeerQuantum = 16;
		AuthoritativeCharacterNetwork RejectionServer(
			RejectionScheduler, TestLimits(), Movement, {}, RejectionConfiguration
		);
		const ConnectionId RejectionPeer{1'400, 1};
		std::vector<std::shared_ptr<KinematicCharacter>> RejectionCharacters;
		Check(RejectionServer.AddPeer(RejectionPeer), "the batch-rejection fixture registers its peer");
		for (std::size_t Index = 0; Index < 16; ++Index) {
			auto Character = std::make_shared<KinematicCharacter>();
			Check(
				RejectionServer.RegisterCharacter(Character) &&
					RejectionServer.MarkMaterialized(
						RejectionPeer, Character->GetObjectId(), StateChannelId(500 + Index)
					),
				"the batch-rejection fixture materializes each state"
			);
			RejectionCharacters.push_back(std::move(Character));
		}
		RejectionServer.Step(World, 1);
		RejectionScheduler.Messages.clear();
		for (std::uint64_t Tick = 2; Tick <= 3; ++Tick) {
			for (const auto &Character : RejectionCharacters) {
				auto Transform = Character->GetCFrame();
				Transform.Position.x += 0.01f;
				Character->ApplyRuntimeTransform(Transform);
			}
			if (Tick == 3)
				RejectionScheduler.ScriptSubmissions(
					{{SchedulerSubmitStatus::Accepted}, {SchedulerSubmitStatus::DroppedUnreliable}}
				);
			RejectionServer.Step(World, Tick);
		}
		Check(
			RejectionScheduler.Messages.size() == 1,
			"the first GCHR batch can commit while a later batch is rejected independently"
		);
		ObjectId RejectedId;
		std::optional<CharacterPublicationSchedulingState> RejectedState;
		for (const auto &Character : RejectionCharacters) {
			auto Candidate = RejectionServer.GetPublicationSchedulingState(RejectionPeer, Character->GetObjectId());
			if (Candidate && Candidate->LastAcceptedPublicationTick == 1 && Candidate->Due) {
				RejectedId = Character->GetObjectId();
				RejectedState = Candidate;
				break;
			}
		}
		Check(
			RejectedState && RejectedState->LastAcceptedPublicationTick == 1 && RejectedState->Due,
			"selected-but-rejected state keeps its prior accepted history and remains compactly due"
		);
		RejectionScheduler.Messages.clear();
		RejectionServer.Step(World, 4);
		RejectedState = RejectionServer.GetPublicationSchedulingState(RejectionPeer, RejectedId);
		Check(
			RejectedState && RejectedState->LastAcceptedPublicationTick == 4 && RejectionScheduler.Messages.size() == 1,
			"the retry publishes only the newest current state on the next safe opportunity"
		);
		const auto RejectionMetrics = RejectionServer.GetMetrics();
		Check(
			RejectionMetrics.PublicationSchedulerRejections == 1 &&
				RejectionMetrics.PublicationStatesSelected == RejectionMetrics.PublicationStatesAccepted + 1,
			"metrics distinguish scheduler rejection from pre-admission budget deferral"
		);
		std::size_t TerminalCallbacks = 0;
		RejectionServer.SetTerminalHandler([&](ConnectionId Failed, const DisconnectInfo &) {
			if (Failed == RejectionPeer) ++TerminalCallbacks;
		});
		for (std::uint64_t Tick = 5; Tick <= 6; ++Tick) {
			for (const auto &Character : RejectionCharacters) {
				auto Transform = Character->GetCFrame();
				Transform.Position.x += 0.01f;
				Character->ApplyRuntimeTransform(Transform);
			}
			if (Tick == 6)
				RejectionScheduler.ScriptSubmissions({{
					SchedulerSubmitStatus::ReliableBacklogExhausted,
					DisconnectInfo{DisconnectReason::ResourceExhaustion, "injected terminal state rejection"},
				}});
			RejectionServer.Step(World, Tick);
		}
		Check(TerminalCallbacks == 1, "terminal unreliable submission failure still enters the FailPeer seam");
		Check(
			RejectionServer.RemovePeer(RejectionPeer, 6),
			"FailPeer cleanup can remove every deferred relation idempotently"
		);

		RecordingScheduler SharedScheduler;
		AuthoritativeCharacterNetwork SharedServer(SharedScheduler, TestLimits(), Movement);
		const ConnectionId SharedPeerA{1'500, 1};
		const ConnectionId SharedPeerB{1'501, 1};
		auto SharedCharacter = std::make_shared<KinematicCharacter>();
		const auto SharedId = SharedCharacter->GetObjectId();
		Check(
			SharedServer.AddPeer(SharedPeerA) && SharedServer.AddPeer(SharedPeerB) &&
				SharedServer.RegisterCharacter(SharedCharacter) &&
				SharedServer.MarkMaterialized(SharedPeerA, SharedId, StateChannelId(600)) &&
				SharedServer.MarkMaterialized(SharedPeerB, SharedId, StateChannelId(601)),
			"one authoritative Character forms two independent peer publication relationships"
		);
		SharedServer.Step(World, 1);
		SharedScheduler.Messages.clear();
		for (std::uint64_t Tick = 2; Tick <= 3; ++Tick) {
			auto Transform = SharedCharacter->GetCFrame();
			Transform.Position.x += 0.1f;
			SharedCharacter->ApplyRuntimeTransform(Transform);
			if (Tick == 3)
				SharedScheduler.ScriptSubmissions(
					{{SchedulerSubmitStatus::Accepted}, {SchedulerSubmitStatus::DroppedUnreliable}}
				);
			SharedServer.Step(World, Tick);
		}
		auto SharedA = SharedServer.GetPublicationSchedulingState(SharedPeerA, SharedId);
		auto SharedB = SharedServer.GetPublicationSchedulingState(SharedPeerB, SharedId);
		Check(
			SharedA && SharedA->LastAcceptedPublicationTick == 3 && SharedB &&
				SharedB->LastAcceptedPublicationTick == 1 && SharedB->Due,
			"shared snapshot acceptance commits per peer and leaves only the rejected peer overdue"
		);
		Check(
			SharedServer.GetMetrics().StateSnapshotsBuilt == 2,
			"one snapshot is captured per Character tick and reused across accepting and rejecting peers"
		);
		SharedScheduler.Messages.clear();
		auto LatestTransform = SharedCharacter->GetCFrame();
		LatestTransform.Position.x += 1.0f;
		SharedCharacter->ApplyRuntimeTransform(LatestTransform);
		SharedServer.Step(World, 4);
		SharedB = SharedServer.GetPublicationSchedulingState(SharedPeerB, SharedId);
		Check(
			SharedB && SharedB->LastAcceptedPublicationTick == 4,
			"a rejected peer later receives the current state without replaying a historical snapshot"
		);

		RecordingScheduler JumpScheduler;
		AuthoritativeCharacterNetwork JumpServer(JumpScheduler, TestLimits(), Movement, {}, LifecycleConfiguration);
		const ConnectionId JumpPeer{1'600, 1};
		std::array<std::shared_ptr<KinematicCharacter>, 3> JumpCharacters;
		Check(JumpServer.AddPeer(JumpPeer), "the absolute-tick wheel fixture registers its peer");
		for (std::size_t Index = 0; Index < JumpCharacters.size(); ++Index) {
			JumpCharacters[Index] = std::make_shared<KinematicCharacter>();
			Check(
				JumpServer.RegisterCharacter(JumpCharacters[Index]) &&
					JumpServer.MarkMaterialized(
						JumpPeer, JumpCharacters[Index]->GetObjectId(), StateChannelId(700 + Index)
					),
				"the absolute-tick wheel fixture creates each relationship"
			);
		}
		JumpServer.Step(World, 1);
		JumpScheduler.Messages.clear();
		JumpServer.Step(World, 1'000'000);
		Check(
			JumpServer.GetMetrics().PublicationLargeTickRebuilds == 1 && JumpScheduler.Messages.size() == 1,
			"a debugger-sized forward jump scans the fixed wheel once and still obeys the work cap"
		);
		JumpServer.Step(World, 999'999);
		JumpServer.Step(World, 1'000'064);
		Check(
			JumpServer.GetMetrics().PublicationLargeTickRebuilds == 3,
			"backward ticks and wheel-index reuse rebuild boundedly from absolute deadlines"
		);

		CharacterNetworkConfiguration LowConfiguration;
		LowConfiguration.MaximumPublicationStatesPerPeerTick = 1;
		LowConfiguration.MaximumPublicationStatesPerTick = 1;
		LowConfiguration.PublicationPeerQuantum = 1;
		RecordingScheduler LowScheduler;
		AuthoritativeCharacterNetwork LowServer(LowScheduler, TestLimits(), Movement, {}, LowConfiguration);
		const ConnectionId LowPeer{1'700, 1};
		std::array<std::shared_ptr<KinematicCharacter>, 7> LowCharacters;
		Check(
			LowServer.AddPeer(LowPeer) &&
				LowServer.SetPeerPublicationFocus(LowPeer, std::array{glm::vec3{0.0f, 6.0f, 0.0f}}),
			"the tier-starvation fixture registers trusted focus"
		);
		for (std::size_t Index = 0; Index < LowCharacters.size(); ++Index) {
			LowCharacters[Index] = std::make_shared<KinematicCharacter>();
			LowCharacters[Index]->SetPosition({Index + 1 == LowCharacters.size() ? 240.0f : float(Index), 6.0f, 0.0f});
			Check(
				LowServer.RegisterCharacter(LowCharacters[Index]) &&
					LowServer.MarkMaterialized(
						LowPeer, LowCharacters[Index]->GetObjectId(), StateChannelId(800 + Index)
					),
				"the tier-starvation fixture materializes each Character"
			);
		}
		const auto LowId = LowCharacters.back()->GetObjectId();
		LowServer.Step(World, 1);
		std::vector<std::uint64_t> LowPublicationTicks;
		for (std::uint64_t Tick = 2; Tick <= 120; ++Tick) {
			for (const auto &Character : LowCharacters) {
				auto Transform = Character->GetCFrame();
				Transform.Position.z += 0.01f;
				Character->ApplyRuntimeTransform(Transform);
			}
			LowScheduler.Messages.clear();
			LowServer.Step(World, Tick);
			for (const auto &Message : LowScheduler.Messages) {
				auto Decoded = DecodeCharacterMessage(Message.Payload());
				auto *Frame = Decoded ? std::get_if<CharacterStateFrame>(&*Decoded) : nullptr;
				if (Frame && std::ranges::any_of(Frame->GetStates(), [LowId](const auto &State) {
						return State.Character == LowId;
					}))
					LowPublicationTicks.push_back(Tick);
			}
		}
		Check(
			LowServer.GetPublicationTier(LowPeer, LowId) == CharacterPublicationTier::LowRate &&
				LowPublicationTicks.size() >= 3,
			"absolute-deadline escalation prevents LowRate starvation behind continuous FullRate work"
		);
		Check(
			LowServer.GetMetrics().PublicationDeadlineEscalations != 0 &&
				LowServer.GetMetrics().PublicationDeadlineMisses ==
					LowServer.GetMetrics().PublicationFullRateDeadlineMisses +
						LowServer.GetMetrics().PublicationReducedRateDeadlineMisses +
						LowServer.GetMetrics().PublicationLowRateDeadlineMisses,
			"sustained overload reports deterministic hard-deadline escalation"
		);

		RecordingScheduler UnlimitedScheduler;
		RecordingScheduler ConstrainedScheduler;
		AuthoritativeCharacterNetwork UnlimitedServer(UnlimitedScheduler, TestLimits(), Movement);
		AuthoritativeCharacterNetwork ConstrainedServer(
			ConstrainedScheduler, TestLimits(), Movement, {}, LifecycleConfiguration
		);
		const ConnectionId UnlimitedPeer{1'800, 1};
		const ConnectionId ConstrainedPeer{1'801, 1};
		std::array<std::shared_ptr<KinematicCharacter>, 3> UnlimitedCharacters;
		std::array<std::shared_ptr<KinematicCharacter>, 3> ConstrainedCharacters;
		Check(
			UnlimitedServer.AddPeer(UnlimitedPeer) && ConstrainedServer.AddPeer(ConstrainedPeer) &&
				UnlimitedServer.RegisterAction(Action(90, 90, 0.125f)) &&
				ConstrainedServer.RegisterAction(Action(90, 90, 0.125f)),
			"publication-equivalence fixtures register matching semantic policy"
		);
		for (std::size_t Index = 0; Index < UnlimitedCharacters.size(); ++Index) {
			UnlimitedCharacters[Index] = std::make_shared<KinematicCharacter>();
			ConstrainedCharacters[Index] = std::make_shared<KinematicCharacter>();
			Check(
				UnlimitedServer.RegisterCharacter(UnlimitedCharacters[Index]) &&
					UnlimitedServer.MarkMaterialized(
						UnlimitedPeer, UnlimitedCharacters[Index]->GetObjectId(), StateChannelId(900 + Index)
					) &&
					ConstrainedServer.RegisterCharacter(ConstrainedCharacters[Index]) &&
					ConstrainedServer.MarkMaterialized(
						ConstrainedPeer, ConstrainedCharacters[Index]->GetObjectId(), StateChannelId(910 + Index)
					),
				"publication-equivalence fixtures create matching authoritative Characters"
			);
		}
		Check(
			UnlimitedServer.StartServerAction(UnlimitedCharacters[0]->GetObjectId(), 90, 1) &&
				ConstrainedServer.StartServerAction(ConstrainedCharacters[0]->GetObjectId(), 90, 1),
			"matching root-motion actions begin at the same authoritative tick"
		);
		for (std::uint64_t Tick = 1; Tick <= 24; ++Tick) {
			for (std::size_t Index = 1; Index < UnlimitedCharacters.size(); ++Index) {
				auto UnlimitedTransform = UnlimitedCharacters[Index]->GetCFrame();
				auto ConstrainedTransform = ConstrainedCharacters[Index]->GetCFrame();
				UnlimitedTransform.Position.z += 0.01f;
				ConstrainedTransform.Position.z += 0.01f;
				UnlimitedCharacters[Index]->ApplyRuntimeTransform(UnlimitedTransform);
				ConstrainedCharacters[Index]->ApplyRuntimeTransform(ConstrainedTransform);
			}
			UnlimitedServer.Step(World, Tick);
			ConstrainedServer.Step(World, Tick);
			for (std::size_t Index = 0; Index < UnlimitedCharacters.size(); ++Index)
				Check(
					Near(
						UnlimitedCharacters[Index]->GetPosition(), ConstrainedCharacters[Index]->GetPosition(), 0.0001f
					),
					"publication budget never changes authoritative transform or root-motion results"
				);
		}
		Check(
			ConstrainedServer.GetMetrics().PublicationStatesDeferred != 0,
			"the simulation-equivalence proof actually exercises constrained publication"
		);
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
		auto MakeState =
			[](ObjectId Character, std::uint64_t Sequence, std::uint64_t Tick, float X, float Velocity = 0.0f) {
				return CharacterAuthoritativeState{
					.Character = Character,
					.ControlEpoch = CharacterControlEpoch(1),
					.StateSequence = RealtimeStateSequence(Sequence),
					.AuthoritativeTick = Tick,
					.Transform = CFrame(X, 6.0f, 0.0f),
					.Velocity = {Velocity, 0.0f, 0.0f},
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
		CharacterStateFrame Extrapolation{
			.ServerTick = 100,
			.FrameSequence = CharacterStateFrameSequence(6),
			.MaterializationEpoch = CharacterMaterializationEpoch(2),
			.StateCount = 1,
		};
		Extrapolation.States[0] = MakeState(SourceB, 2, 100, 100.0f, 60.0f);
		Check(
			Client.HandleTransportEvent(TransportEvent(StateFrameEvent(Connection, StateChannelId(2), Extrapolation))),
			"sparse remote state with velocity is accepted without treating expected travel as a teleport"
		);
		Client.Reconcile(World);
		Client.UpdatePresentation(109);
		Check(
			Near(ReplicaB->GetPresentationCFrame().Position, {103.0f, 6.0f, 0.0f}),
			"remote presentation extrapolates velocity for the bounded six-tick loss window"
		);
		Client.UpdatePresentation(120);
		Check(
			Near(ReplicaB->GetPresentationCFrame().Position, {100.0f, 6.0f, 0.0f}) &&
				Client.GetMetrics().RemoteExtrapolations != 0 && Client.GetMetrics().RemoteExtrapolationHolds != 0,
			"remote presentation holds the newest sample after the extrapolation budget expires"
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

	void TestVariableCadenceMotionQuality() {
		auto RunProfile =
			[](const char *QualityMessage, auto StateAt, float MaximumAllowedError, bool DropEveryFifthSample = false) {
				RecordingScheduler Scheduler;
				const ConnectionId Connection{242, 1};
				PredictedCharacterNetwork Client(Scheduler, TestLimits(), Movement);
				const ObjectId Source{4242, 1};
				auto Replica = std::make_shared<KinematicCharacter>();
				WorldRoot World;
				Check(
					Client.AddPeer(Connection) && Client.MarkMaterialized(Source, Replica),
					"variable-cadence quality fixture materializes one remote Character"
				);
				std::uint64_t FrameSequence = 1;
				std::uint64_t StateSequence = 1;
				std::size_t SampleIndex = 0;
				std::vector<float> Errors;
				for (std::uint64_t Tick = 1; Tick <= 180; ++Tick) {
					if ((Tick - 1) % 12 == 0) {
						const bool Drop = DropEveryFifthSample && SampleIndex % 5 == 4;
						++SampleIndex;
						if (!Drop) {
							const auto [Position, Velocity] = StateAt(Tick);
							CharacterStateFrame Frame{
								.ServerTick = Tick,
								.FrameSequence = CharacterStateFrameSequence(FrameSequence),
								.MaterializationEpoch = CharacterMaterializationEpoch(1),
								.StateCount = 1,
							};
							Frame.States[0] = {
								.Character = Source,
								.ControlEpoch = CharacterControlEpoch(1),
								.StateSequence = RealtimeStateSequence(StateSequence),
								.AuthoritativeTick = Tick,
								.Transform = CFrame(Position),
								.Velocity = Velocity,
							};
							Check(
								Client.HandleTransportEvent(
									TransportEvent(StateFrameEvent(Connection, StateChannelId(91), Frame))
								),
								"variable-cadence sample is accepted"
							);
							++FrameSequence;
							++StateSequence;
						}
					}
					Client.Reconcile(World);
					Client.UpdatePresentation(Tick);
					if (Tick >= 25) {
						const auto TargetTick = Tick - DefaultRemoteInterpolationDelayTicks;
						const auto [ExpectedPosition, ExpectedVelocity] = StateAt(TargetTick);
						(void)ExpectedVelocity;
						Errors.push_back(glm::distance(Replica->GetPresentationCFrame().Position, ExpectedPosition));
					}
				}
				const auto MaximumError = *std::ranges::max_element(Errors);
				if (MaximumError > MaximumAllowedError)
					std::cerr << "[Character:NetworkTest] quality=" << QualityMessage << " max=" << MaximumError
							  << " bound=" << MaximumAllowedError << '\n';
				Check(MaximumError <= MaximumAllowedError, QualityMessage);
				Check(Client.GetMetrics().InterpolationResets == 0, "variable cadence does not invent teleport resets");
				if (DropEveryFifthSample)
					Check(
						Client.GetMetrics().RemoteExtrapolationHolds != 0,
						"loss beyond the six-tick extrapolation horizon deterministically holds"
					);
			};

		RunProfile(
			"5 Hz constant motion stays within five centimetres",
			[](std::uint64_t Tick) {
				const auto Seconds = static_cast<float>(Tick) / 60.0f;
				return std::pair{glm::vec3{6.0f * Seconds, 6.0f, 0.0f}, glm::vec3{6.0f, 0.0f, 0.0f}};
			},
			0.05f
		);
		RunProfile(
			"5 Hz acceleration, stop, and reverse stay within fifteen centimetres",
			[](std::uint64_t Tick) {
				const auto Time = static_cast<float>(Tick) / 60.0f;
				float X = 0.0f;
				float Velocity = 0.0f;
				if (Time <= 1.0f) {
					X = 3.0f * Time * Time;
					Velocity = 6.0f * Time;
				} else if (Time <= 1.5f) {
					X = 3.0f + 6.0f * (Time - 1.0f);
					Velocity = 6.0f;
				} else if (Time <= 2.0f) {
					const auto Delta = Time - 1.5f;
					X = 6.0f + 6.0f * Delta - 6.0f * Delta * Delta;
					Velocity = 6.0f - 12.0f * Delta;
				} else {
					X = 7.5f - 6.0f * (Time - 2.0f);
					Velocity = -6.0f;
				}
				return std::pair{glm::vec3{X, 6.0f, 0.0f}, glm::vec3{Velocity, 0.0f, 0.0f}};
			},
			0.15f
		);
		RunProfile(
			"5 Hz sharp turns stay within ten centimetres",
			[](std::uint64_t Tick) {
				const auto Time = static_cast<float>(Tick) / 60.0f;
				const auto Angle = 0.5f * Time;
				return std::pair{
					glm::vec3{10.0f * std::cos(Angle), 6.0f, 10.0f * std::sin(Angle)},
					glm::vec3{-5.0f * std::sin(Angle), 0.0f, 5.0f * std::cos(Angle)},
				};
			},
			0.10f
		);
		RunProfile(
			"5 Hz jump and gravity motion stays within the measured fifty-five-centimetre landing bound",
			[](std::uint64_t Tick) {
				const auto Time = static_cast<float>(Tick) / 60.0f;
				const auto AirTime = 16.0f / 9.8f;
				const auto Height = Time < AirTime ? 6.0f + 8.0f * Time - 4.9f * Time * Time : 6.0f;
				const auto VerticalVelocity = Time < AirTime ? 8.0f - 9.8f * Time : 0.0f;
				return std::pair{glm::vec3{2.0f * Time, Height, 0.0f}, glm::vec3{2.0f, VerticalVelocity, 0.0f}};
			},
			0.55f
		);
		RunProfile(
			"5 Hz 120-unit-per-second motion stays within ten centimetres",
			[](std::uint64_t Tick) {
				const auto Seconds = static_cast<float>(Tick) / 60.0f;
				return std::pair{glm::vec3{120.0f * Seconds, 6.0f, 0.0f}, glm::vec3{120.0f, 0.0f, 0.0f}};
			},
			0.10f
		);
		RunProfile(
			"5 Hz constant motion with deterministic twenty-percent sample loss stays within two metres",
			[](std::uint64_t Tick) {
				const auto Seconds = static_cast<float>(Tick) / 60.0f;
				return std::pair{glm::vec3{6.0f * Seconds, 6.0f, 0.0f}, glm::vec3{6.0f, 0.0f, 0.0f}};
			},
			2.0f,
			true
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

	void TestSchedulerAdmissionPrecedesPrediction() {
		RecordingScheduler Scheduler;
		const ConnectionId Connection{77, 2};
		const ObjectId Source{7'701, 3};
		WorldRoot World;
		auto Character = std::make_shared<KinematicCharacter>();
		Character->SetPosition({0.0f, 6.0f, 0.0f});
		PredictedCharacterNetwork Client(Scheduler, TestLimits(), Movement);
		Check(
			Client.AddPeer(Connection) && Client.MarkMaterialized(Source, Character),
			"prediction-admission fixture materializes a controlled replica"
		);
		CharacterControlTransition Bind{Source, CharacterControlEpoch(9), StateChannelId(77), 1, true};
		auto BindBytes = EncodeCharacterMessage(CharacterMessage(Bind));
		Check(
			Client.HandleTransportEvent(TransportEvent(
				ReceivedMessageEvent{
					Connection,
					DeliveryMode::ReliableOrdered,
					TrafficClass::Control,
					{},
					BindBytes ? std::move(*BindBytes) : std::vector<std::byte>{},
				}
			)),
			"prediction-admission fixture accepts its reliable control bind"
		);
		const auto Initial = Character->GetCFrame();
		Scheduler.NextSubmission = {SchedulerSubmitStatus::DroppedUnreliable};
		Check(
			!Client.SubmitInput(Connection, World, 2, 1.0f / 60.0f, {1.0f, 0.0f}, 0.0f, false) &&
				Character->GetCFrame().FuzzyEq(Initial) && Client.GetPredictionHistorySize(Connection) == 0 &&
				Scheduler.Messages.empty(),
			"a scheduler-dropped input mutates no transform, history, or admitted-message sequence"
		);
		Scheduler.NextSubmission = {SchedulerSubmitStatus::Accepted};
		Check(
			Client.SubmitInput(Connection, World, 3, 1.0f / 60.0f, {1.0f, 0.0f}, 0.0f, false),
			"prediction resumes only after scheduler admission"
		);
		auto Decoded = Scheduler.Messages.empty()
						   ? SerializationResult<CharacterMessage>(SerializationFailure(
								 SerializationErrorCode::InternalFailure, "missing admitted Character input"
							 ))
						   : DecodeCharacterMessage(Scheduler.Messages.back().Payload());
		auto *Input = Decoded ? std::get_if<CharacterInputCommand>(&*Decoded) : nullptr;
		Check(
			Input && Input->InputSequence == CharacterInputSequence(1) &&
				Character->GetPosition().x > Initial.Position.x && Client.GetPredictionHistorySize(Connection) == 1,
			"the first admitted input owns sequence one and then records local prediction"
		);
	}

	void TestReliableCharacterFailureAndActionResults() {
		RecordingScheduler Scheduler;
		const ConnectionId Connection{88, 4};
		WorldRoot World;
		auto Character = std::make_shared<KinematicCharacter>();
		AuthoritativeCharacterNetwork Server(Scheduler, TestLimits(), Movement);
		std::size_t TerminalCallbacks = 0;
		Server.SetTerminalHandler([&](ConnectionId Failed, const DisconnectInfo &Information) {
			if (Failed == Connection && Information.Reason == DisconnectReason::ResourceExhaustion) ++TerminalCallbacks;
		});
		Check(
			Server.AddPeer(Connection) && Server.RegisterCharacter(Character) &&
				Server.MarkMaterialized(Connection, Character->GetObjectId(), StateChannelId(88)),
			"reliable-failure fixture registers an authoritative Character"
		);
		Scheduler.NextSubmission = {
			SchedulerSubmitStatus::ReliableBacklogExhausted,
			DisconnectInfo{DisconnectReason::ResourceExhaustion, "injected reliable backlog exhaustion"},
		};
		Check(
			!Server.BindControl(Connection, Character->GetObjectId(), 1) && TerminalCallbacks == 1,
			"rejected reliable Character control reports a terminal owning-peer failure"
		);

		Fixture Value;
		Check(
			Value.Client->RequestAction(Value.ClientConnection, 1, Value.Tick) &&
				Value.Client->RequestAction(Value.ClientConnection, 2, Value.Tick),
			"two action requests coexist within the bounded pending window"
		);
		for (int Index = 0; Index < 3; ++Index)
			Value.Cycle();
		auto Resolutions = Value.Client->DrainActionResolutions();
		auto Active = Value.Client->GetAuthoritativeAction(Value.SourceCharacter);
		Check(
			Resolutions.size() == 2 && Resolutions[0].RequestedActionToken == 1 && Resolutions[0].Accepted &&
				Resolutions[1].RequestedActionToken == 2 && !Resolutions[1].Accepted,
			"accepted action A and rejected action B preserve two ordered semantic results"
		);
		Check(
			Active && Active->ActionToken == 1,
			"rejecting replacement action B does not cancel still-active accepted action A"
		);
	}

	void TestCompleteRootMotionIntervals() {
		for (const std::uint32_t Duration : {1u, 2u, 4u}) {
			auto Definition = Action(100 + Duration, static_cast<std::uint8_t>(100 + Duration), 0.0f);
			Definition.DurationTicks = Duration;
			Definition.EvaluateRootMotion = [Duration](std::uint64_t From, std::uint64_t To) {
				return To > From
						   ? std::optional(
								 RootMotionDelta{
									 .Translation =
										 {static_cast<float>(To - From) / static_cast<float>(Duration), 0.0f, 0.0f},
								 }
							 )
						   : std::nullopt;
			};

			RecordingScheduler ServerScheduler;
			const ConnectionId ServerConnection{90 + Duration, 1};
			WorldRoot ServerWorld;
			auto ServerCharacter = std::make_shared<KinematicCharacter>();
			ServerCharacter->SetPosition({0.0f, 6.0f, 0.0f});
			AuthoritativeCharacterNetwork Server(ServerScheduler, TestLimits(), Movement);
			Check(
				Server.AddPeer(ServerConnection) && Server.RegisterCharacter(ServerCharacter) &&
					Server.MarkMaterialized(
						ServerConnection, ServerCharacter->GetObjectId(), StateChannelId(90 + Duration)
					) &&
					Server.RegisterAction(Definition) &&
					Server.StartServerAction(ServerCharacter->GetObjectId(), Definition.Token, 10),
				"root-motion server fixture starts a bounded action"
			);
			Server.Step(ServerWorld, 10);
			Server.Step(ServerWorld, 10 + Duration + 3);
			const auto ServerFinal = ServerCharacter->GetPosition();
			Server.Step(ServerWorld, 10 + Duration + 4);
			Check(
				std::abs(ServerFinal.x - 1.0f) < 0.001f && Near(ServerCharacter->GetPosition(), ServerFinal, 0.001f),
				"late authoritative steps clamp to and integrate the exact registered action duration"
			);

			RecordingScheduler ClientScheduler;
			const ConnectionId ClientConnection{100 + Duration, 1};
			const ObjectId Source{static_cast<std::uint32_t>(8'000 + Duration), 2};
			WorldRoot ClientWorld;
			auto ClientCharacter = std::make_shared<KinematicCharacter>();
			ClientCharacter->SetPosition({0.0f, 6.0f, 0.0f});
			PredictedCharacterNetwork Client(ClientScheduler, TestLimits(), Movement);
			Check(
				Client.AddPeer(ClientConnection) && Client.MarkMaterialized(Source, ClientCharacter) &&
					Client.RegisterAction(Definition),
				"root-motion prediction fixture registers matching content"
			);
			CharacterControlTransition Bind{Source, CharacterControlEpoch(12), StateChannelId(100 + Duration), 1, true};
			auto BindBytes = EncodeCharacterMessage(CharacterMessage(Bind));
			Check(
				Client.HandleTransportEvent(TransportEvent(
					ReceivedMessageEvent{
						ClientConnection,
						DeliveryMode::ReliableOrdered,
						TrafficClass::Control,
						{},
						BindBytes ? std::move(*BindBytes) : std::vector<std::byte>{},
					}
				)) &&
					Client.RequestAction(ClientConnection, Definition.Token, 10),
				"root-motion prediction starts from an admitted semantic action"
			);
			const auto ConfirmationOffset = std::max(1u, Duration / 2);
			for (std::uint32_t Offset = 1; Offset <= Duration; ++Offset) {
				Check(
					Client.SubmitInput(ClientConnection, ClientWorld, 10 + Offset, 1.0f / 60.0f, {}, 0.0f, false),
					"predicted action advances through each registered interval"
				);
				if (Offset == ConfirmationOffset) {
					auto ResultBytes = EncodeCharacterMessage(CharacterMessage(
						CharacterActionResult{
							.Character = Source,
							.ControlEpoch = CharacterControlEpoch(12),
							.ActionSequence = CharacterActionSequence(1),
							.RequestedActionToken = Definition.Token,
							.Accepted = true,
							.AuthoritativeAction = CharacterActionState{
								CharacterActionSequence(1),
								Definition.Token,
								Definition.Animation,
								Definition.ContentRevision,
								10,
								Duration,
							},
						}
					));
					Check(
						Client.HandleTransportEvent(TransportEvent(
							ReceivedMessageEvent{
								ClientConnection,
								DeliveryMode::ReliableOrdered,
								TrafficClass::ReliableApplication,
								{},
								ResultBytes ? std::move(*ResultBytes) : std::vector<std::byte>{},
							}
						)),
						"authoritative action confirmation preserves the monotonic predicted phase"
					);
				}
			}
			const auto ClientFinal = ClientCharacter->GetPosition();
			Check(
				Client.SubmitInput(ClientConnection, ClientWorld, 11 + Duration, 1.0f / 60.0f, {}, 0.0f, false) &&
					std::abs(ClientFinal.x - 1.0f) < 0.001f &&
					Near(ClientCharacter->GetPosition(), ClientFinal, 0.001f),
				"prediction uses the same complete interval convention with no post-completion root motion"
			);
		}
		Check(
			!CharacterActionState{
				CharacterActionSequence(1),
				1,
				AssetId::FromBuiltInName("overflow-action"),
				Content(1),
				std::numeric_limits<std::uint64_t>::max(),
				1,
			}
					.IsValid() &&
				!CharacterMaterializationEpoch(std::numeric_limits<std::uint64_t>::max()).TryNext(),
			"action-end arithmetic and 64-bit materialization generations reject wrap explicitly"
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
		TestImportanceCadenceAndLifecycle();
		TestBoundedPublicationScheduling();
		TestBatchLossAndRemoteInterpolation();
		TestVariableCadenceMotionQuality();
		TestLocalPresentationCorrectionMatrix();
		TestCustomLuauPolicy();
		TestSchedulerAdmissionPrecedesPrediction();
		TestReliableCharacterFailureAndActionResults();
		TestCompleteRootMotionIntervals();
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
