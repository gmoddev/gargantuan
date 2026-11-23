#include "gargantuan/network/GameplayInput.hpp"

#include "gargantuan/network/BinaryCodec.hpp"
#include "gargantuan/network/Scheduler.hpp"
#include "gargantuan/network/Transport.hpp"
#include "../runtime/RuntimeWorkDiagnostics.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numbers>
#include <utility>

namespace gargantuan::network {
	namespace {
		constexpr std::uint32_t GameplayInputMagic = 0x4e495047; // GPIN, little endian.
		constexpr std::uint16_t GameplayInputVersion = 1;
		constexpr std::size_t ControlFrameBytes = 32;
		constexpr std::size_t InputFrameBytes = 52;
		constexpr float MaximumMovementMagnitude = 1.001f;
		// Stock client input uses a channel with a nonzero ObjectId generation
		// in its upper 32 bits. This channel belongs only to client GPIN input.
		constexpr StateChannelId InputChannel{1};
		constexpr std::uint8_t JumpFlag = 1;

		enum class FrameKind : std::uint8_t { Bind, Unbind, Input };

		void SaturatingAdd(std::uint64_t &Value, std::uint64_t Amount = 1) {
			Value += std::min(Amount, std::numeric_limits<std::uint64_t>::max() - Value);
		}

		struct AdmissionTimer {
			std::uint64_t &Total;
			std::chrono::steady_clock::time_point Start = std::chrono::steady_clock::now();

			~AdmissionTimer() {
				SaturatingAdd(Total, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - Start).count()));
			}
		};

		struct FrameHeader {
			FrameKind Kind{};
			std::uint8_t Flags = 0;
			ObjectId Entity;
			GameplayInputLeaseEpoch Epoch;
		};

		void WriteHeader(GameBinaryWriter &Output, FrameHeader Header) {
			Output.Integer(GameplayInputMagic);
			Output.Integer(GameplayInputVersion);
			Output.Integer(static_cast<std::uint8_t>(Header.Kind));
			Output.Integer(Header.Flags);
			WriteBinaryObjectId(Output, Header.Entity);
			Output.Integer(Header.Epoch.Value());
		}

		bool ReadHeader(GameBinaryReader &Input, FrameHeader &Header) {
			std::uint32_t Magic;
			std::uint16_t Version;
			std::uint8_t Kind;
			std::uint64_t Epoch;
			if (!Input.Integer(Magic) || !Input.Integer(Version) || !Input.Integer(Kind) ||
				!Input.Integer(Header.Flags) || !ReadBinaryObjectId(Input, Header.Entity) || !Input.Integer(Epoch) ||
				Magic != GameplayInputMagic || Version != GameplayInputVersion ||
				Kind > static_cast<std::uint8_t>(FrameKind::Input) || !Header.Entity.IsValid() || Epoch == 0)
				return false;
			Header.Kind = static_cast<FrameKind>(Kind);
			Header.Epoch = GameplayInputLeaseEpoch(Epoch);
			return Header.Kind == FrameKind::Input ? Header.Flags <= JumpFlag : Header.Flags == 0;
		}

		bool ValidMovement(glm::vec2 Move, float Facing) {
			return std::isfinite(Move.x) && std::isfinite(Move.y) &&
				Move.x * Move.x + Move.y * Move.y <= MaximumMovementMagnitude * MaximumMovementMagnitude &&
				std::isfinite(Facing) && std::abs(Facing) <= std::numbers::pi_v<float>;
		}

		bool SendControl(INetworkScheduler &Scheduler, ConnectionId Connection, FrameKind Kind, ObjectId Entity,
			GameplayInputLeaseEpoch Epoch, std::uint64_t Tick, const NetworkLimits &Limits) {
			GameBinaryWriter Output(ControlFrameBytes);
			WriteHeader(Output, {.Kind = Kind, .Entity = Entity, .Epoch = Epoch});
			Output.Integer(Tick);
			if (!Output.Succeeded()) return false;
			auto Message = MakeNetworkMessageIntent(Connection, DeliveryMode::ReliableOrdered, TrafficClass::Control,
				{}, std::move(Output.Bytes), Limits);
			return Message && Scheduler.Submit(std::move(*Message)).Accepted();
		}
	}

	bool IsGameplayInputFrame(std::span<const std::byte> Bytes) {
		return Bytes.size() >= 4 && Bytes[0] == std::byte{'G'} && Bytes[1] == std::byte{'P'} &&
			Bytes[2] == std::byte{'I'} && Bytes[3] == std::byte{'N'};
	}

	bool GameplayInputServer::SetPossession(ConnectionId Connection, ObjectId Entity, std::uint64_t ServerTick,
		const NetworkLimits &Limits, std::uint64_t PossessionRevision) {
		if (!Connection.IsValid() || ServerTick == 0 || (Entity != ObjectId{} && !Entity.IsValid())) return false;
		auto Current = Leases.find(Connection);
		if (Current != Leases.end()) {
			if (ServerTick < Current->second.BoundTick || ServerTick < Current->second.LastConsumedTick) return false;
			if (Current->second.Entity == Entity && Current->second.PossessionRevision == PossessionRevision)
				return true;
		}
		if (Entity.IsValid() && std::ranges::any_of(Leases, [&](const auto &Entry) {
				return Entry.first != Connection && Entry.second.Entity == Entity;
			})) return false;
		if (Current != Leases.end()) {
			const auto Previous = Current->second;
			RemovePeer(Connection);
			if (!SendControl(Scheduler, Connection, FrameKind::Unbind, Previous.Entity, Previous.Epoch, ServerTick, Limits))
				return false;
		}
		if (!Entity.IsValid()) return true;
		if (!NextEpoch.IsValid() || Leases.size() >= MaximumGameplayInputLeases) return false;
		const auto Epoch = NextEpoch;
		Leases.emplace(Connection, Lease{.Entity = Entity, .Epoch = Epoch,
			.PossessionRevision = PossessionRevision, .BoundTick = ServerTick});
		if (!SendControl(Scheduler, Connection, FrameKind::Bind, Entity, Epoch, ServerTick, Limits)) {
			Leases.erase(Connection);
			return false;
		}
		NextEpoch = NextEpoch.TryNext().value_or(GameplayInputLeaseEpoch{});
		SaturatingAdd(Metrics.Bindings);
		return true;
	}

	void GameplayInputServer::RemovePeer(ConnectionId Connection) {
		if (Leases.erase(Connection) > 0) SaturatingAdd(Metrics.Revocations);
	}

	bool GameplayInputServer::HandleReceived(const ReceivedMessageEvent &Event, std::uint64_t ServerTick) {
		runtime_detail::WorkScope Work(runtime_detail::WorkPhase::GameplayInputAdmission);
		AdmissionTimer Timing{Metrics.AdmissionNanoseconds};
		SaturatingAdd(Metrics.InputBytesReceived, Event.Payload.size());
		auto Current = Leases.find(Event.Connection);
		if (Current == Leases.end()) {
			SaturatingAdd(Metrics.UnauthorizedInputs);
			return false;
		}
		auto &Lease = Current->second;
		if (ServerTick < Lease.BoundTick || ServerTick < Lease.RateTick || ServerTick < Lease.LastConsumedTick) {
			SaturatingAdd(Metrics.StaleInputs);
			return false;
		}
		if (Lease.RateTick != ServerTick) {
			Lease.RateTick = ServerTick;
			Lease.AttemptsAtRateTick = 0;
		}
		if (Lease.AttemptsAtRateTick == MaximumGameplayInputsPerTick) {
			SaturatingAdd(Metrics.RateLimitedInputs);
			return false;
		}
		++Lease.AttemptsAtRateTick;
		const auto *Order = std::get_if<RealtimeStateOrder>(&Event.Order);
		if (Event.Delivery != DeliveryMode::UnreliableSequenced || Event.Traffic != TrafficClass::RealtimeState ||
			!Order || Order->Channel != InputChannel || Event.Payload.size() != InputFrameBytes) {
			SaturatingAdd(Metrics.MalformedInputs);
			return false;
		}
		GameBinaryReader Input(Event.Payload, "Gameplay input");
		FrameHeader Header;
		std::uint64_t Sequence;
		std::uint64_t ClientTick;
		glm::vec2 Movement;
		float FacingYawRadians;
		if (!ReadHeader(Input, Header) || Header.Kind != FrameKind::Input || !Input.Integer(Sequence) || Sequence == 0 ||
			Order->Sequence.Value() != Sequence || !Input.Integer(ClientTick) || ClientTick == 0 ||
			!Input.Float(Movement.x) || !Input.Float(Movement.y) || !Input.Float(FacingYawRadians) ||
			!Input.Complete() || !ValidMovement(Movement, FacingYawRadians)) {
			SaturatingAdd(Metrics.MalformedInputs);
			return false;
		}
		if (Lease.Entity != Header.Entity || Lease.Epoch != Header.Epoch) {
			SaturatingAdd(Metrics.UnauthorizedInputs);
			return false;
		}
		// Host frame counters advance independently. ClientTick orders that
		// sender's reports; only receipt ticks govern server rate and freshness.
		if (Sequence <= Lease.LastInput.Value() || ClientTick <= Lease.LastClientTick) {
			SaturatingAdd(Metrics.StaleInputs);
			return false;
		}
		if (Header.Flags == JumpFlag) Lease.JumpArrivalTick = ServerTick;
		if (Lease.ArrivalTick && Lease.LastInput != Lease.AcknowledgedInput)
			SaturatingAdd(Metrics.InputsCoalesced);
		Lease.LastInput = GameplayInputSequence(Sequence);
		Lease.LastClientTick = ClientTick;
		Lease.ArrivalTick = ServerTick;
		Lease.Movement = Movement;
		Lease.FacingYawRadians = FacingYawRadians;
		SaturatingAdd(Metrics.InputsAccepted);
		return true;
	}

	std::optional<GameplayInputSample> GameplayInputServer::Consume(ConnectionId Connection, ObjectId CurrentEntity,
		std::uint64_t ServerTick, std::uint64_t PossessionRevision) {
		auto Current = Leases.find(Connection);
		if (Current == Leases.end() || Current->second.Entity != CurrentEntity ||
			Current->second.PossessionRevision != PossessionRevision) return std::nullopt;
		auto &Lease = Current->second;
		if (ServerTick < Lease.BoundTick || ServerTick <= Lease.LastConsumedTick || ServerTick < Lease.ArrivalTick)
			return std::nullopt;
		Lease.LastConsumedTick = ServerTick;
		GameplayInputSample Sample{.AuthoritativeTick = ServerTick, .AcknowledgedInput = Lease.AcknowledgedInput};
		if (!Lease.LastInput.IsValid() || Lease.ArrivalTick == 0) return Sample;
		const auto Age = ServerTick - Lease.ArrivalTick;
		if (Age > GameplayInputFreshnessTicks) {
			Lease.ArrivalTick = 0;
			Lease.JumpArrivalTick = 0;
			SaturatingAdd(Metrics.InputsExpired);
			return Sample;
		}
		const bool New = Lease.LastInput != Lease.AcknowledgedInput;
		if (New) {
			Lease.AcknowledgedInput = Lease.LastInput;
			SaturatingAdd(Metrics.InputsConsumed);
			Metrics.MaximumConsumedInputAgeTicks = std::max(Metrics.MaximumConsumedInputAgeTicks, Age);
		}
		const auto JumpTick = std::exchange(Lease.JumpArrivalTick, 0);
		return GameplayInputSample{
			.Movement = Lease.Movement,
			.FacingYawRadians = Lease.FacingYawRadians,
			.JumpRequested = JumpTick != 0 && ServerTick - JumpTick <= GameplayInputFreshnessTicks,
			.Fresh = true,
			.AuthoritativeTick = ServerTick,
			.AcknowledgedInput = Lease.AcknowledgedInput,
			.AgeTicks = Age,
		};
	}

	bool GameplayInputClient::HandleReceived(const ReceivedMessageEvent &Event) {
		if (!Event.Connection.IsValid() || (ServerConnection.IsValid() && Event.Connection != ServerConnection) ||
			Event.Delivery != DeliveryMode::ReliableOrdered || Event.Traffic != TrafficClass::Control ||
			!std::holds_alternative<std::monostate>(Event.Order) || Event.Payload.size() != ControlFrameBytes)
			return false;
		GameBinaryReader Input(Event.Payload, "Gameplay input control");
		FrameHeader Header;
		std::uint64_t ServerTick;
		if (!ReadHeader(Input, Header) || Header.Kind == FrameKind::Input || !Input.Integer(ServerTick) ||
			ServerTick == 0 || ServerTick < LastControlTick || !Input.Complete()) return false;
		if (Header.Kind == FrameKind::Bind) {
			if (Entity.IsValid() || Header.Epoch <= Epoch) return false;
			ServerConnection = Event.Connection;
			Entity = Header.Entity;
			Epoch = Header.Epoch;
		} else {
			if (Header.Entity != Entity || Header.Epoch != Epoch) return false;
			Entity = {};
		}
		LastControlTick = ServerTick;
		return true;
	}

	bool GameplayInputClient::Submit(ConnectionId Connection, ObjectId CurrentEntity, std::uint64_t ClientTick,
		glm::vec2 Movement, float FacingYawRadians, bool JumpRequested, const NetworkLimits &Limits) {
		runtime_detail::WorkScope Work(runtime_detail::WorkPhase::GameplayInputProduce);
		if (Connection != ServerConnection || CurrentEntity != Entity || !Entity.IsValid() || !NextInput.IsValid() ||
			ClientTick <= LastSubmittedTick || !ValidMovement(Movement, FacingYawRadians)) return false;
		GameBinaryWriter Output(InputFrameBytes);
		WriteHeader(Output, {.Kind = FrameKind::Input, .Flags = JumpRequested ? JumpFlag : std::uint8_t{0},
			.Entity = Entity, .Epoch = Epoch});
		Output.Integer(NextInput.Value());
		Output.Integer(ClientTick);
		Output.Float(Movement.x);
		Output.Float(Movement.y);
		Output.Float(FacingYawRadians);
		if (!Output.Succeeded()) return false;
		auto Message = MakeNetworkMessageIntent(Connection, DeliveryMode::UnreliableSequenced, TrafficClass::RealtimeState,
			RealtimeStateOrder{InputChannel, RealtimeStateSequence(NextInput.Value())}, std::move(Output.Bytes), Limits);
		if (!Message || !Scheduler.Submit(std::move(*Message)).Accepted()) return false;
		NextInput = NextInput.TryNext().value_or(GameplayInputSequence{});
		LastSubmittedTick = ClientTick;
		SaturatingAdd(InputsProduced);
		return true;
	}

	void GameplayInputClient::Reset() {
		ServerConnection = {};
		Entity = {};
		Epoch = {};
		NextInput = GameplayInputSequence(1);
		LastControlTick = 0;
		LastSubmittedTick = 0;
		InputsProduced = 0;
	}
}
