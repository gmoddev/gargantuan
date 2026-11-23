#include "gargantuan/network/GameplayPose.hpp"

#include "gargantuan/network/BinaryCodec.hpp"
#include "gargantuan/network/Transport.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace gargantuan::network {
	namespace {
		constexpr std::uint32_t GameplayPoseMagic = 0x534f5047; // GPOS, little endian.
		constexpr std::uint16_t GameplayPoseVersion = 1;
		constexpr std::size_t ControlFrameBytes = 32;
		constexpr std::size_t SnapshotFrameBytes = 60;
		constexpr std::uint8_t TeleportFlag = 1;
		constexpr float RotationTolerance = 1.0e-3f;
		constexpr float MaximumInterpolationGap = 8.0f;

		enum class FrameKind : std::uint8_t { Publish, Unpublish, Snapshot };

		// Every GCHR state channel includes a nonzero generation in its upper
		// half. GPOS uses one slot-based channel per server-published part and
		// the server tick as sequence so the channel never restarts on slot reuse.
		StateChannelId PoseChannel(ObjectId Part) { return StateChannelId(Part.Slot); }

		bool Finite(glm::vec3 Value) {
			return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
		}

		bool Normalized(glm::quat Rotation) {
			return std::isfinite(Rotation.x) && std::isfinite(Rotation.y) && std::isfinite(Rotation.z) &&
				std::isfinite(Rotation.w) && std::abs(glm::length(Rotation) - 1.0f) <= RotationTolerance;
		}

		bool ValidControl(const GameplayPoseControl &Control) {
			return Control.Part.IsValid() && Control.MaterializationEpoch != 0 &&
				Control.Published == (Control.RequiredStructuralSequence != 0);
		}

		bool ValidSnapshot(const GameplayPoseSnapshot &Snapshot) {
			return Snapshot.Part.IsValid() && Snapshot.MaterializationEpoch != 0 && Snapshot.ServerTick != 0 &&
				IsValidGameplayPoseTransform(Snapshot.Transform);
		}

		void WriteHeader(GameBinaryWriter &Output, FrameKind Kind, std::uint8_t Flags, ObjectId Part,
			std::uint64_t MaterializationEpoch) {
			Output.Integer(GameplayPoseMagic);
			Output.Integer(GameplayPoseVersion);
			Output.Integer(static_cast<std::uint8_t>(Kind));
			Output.Integer(Flags);
			WriteBinaryObjectId(Output, Part);
			Output.Integer(MaterializationEpoch);
		}

		void WriteTransform(GameBinaryWriter &Output, const CFrame &Transform) {
			const auto Rotation = glm::normalize(Transform.ToQuaternion());
			Output.Float(Transform.Position.x);
			Output.Float(Transform.Position.y);
			Output.Float(Transform.Position.z);
			Output.Float(Rotation.x);
			Output.Float(Rotation.y);
			Output.Float(Rotation.z);
			Output.Float(Rotation.w);
		}

		bool ReadTransform(GameBinaryReader &Input, CFrame &Transform) {
			glm::vec3 Position;
			glm::quat Rotation;
			if (!Input.Float(Position.x) || !Input.Float(Position.y) || !Input.Float(Position.z) ||
				!Input.Float(Rotation.x) || !Input.Float(Rotation.y) || !Input.Float(Rotation.z) ||
				!Input.Float(Rotation.w) || !Finite(Position) || !Normalized(Rotation)) return false;
			Transform = CFrame(Position, glm::mat3_cast(glm::normalize(Rotation)));
			return true;
		}
	}

	bool IsValidGameplayPoseTransform(const CFrame &Transform) {
		const auto &X = Transform.Rotation[0];
		const auto &Y = Transform.Rotation[1];
		const auto &Z = Transform.Rotation[2];
		return Finite(Transform.Position) && Finite(X) && Finite(Y) && Finite(Z) &&
			std::abs(glm::dot(X, X) - 1.0f) <= RotationTolerance &&
			std::abs(glm::dot(Y, Y) - 1.0f) <= RotationTolerance &&
			std::abs(glm::dot(Z, Z) - 1.0f) <= RotationTolerance &&
			std::abs(glm::dot(X, Y)) <= RotationTolerance && std::abs(glm::dot(X, Z)) <= RotationTolerance &&
			std::abs(glm::dot(Y, Z)) <= RotationTolerance && glm::dot(glm::cross(X, Y), Z) > 0.0f &&
			Normalized(Transform.ToQuaternion());
	}

	bool IsGameplayPoseFrame(std::span<const std::byte> Bytes) {
		return Bytes.size() >= 4 && Bytes[0] == std::byte{'G'} && Bytes[1] == std::byte{'P'} &&
			Bytes[2] == std::byte{'O'} && Bytes[3] == std::byte{'S'};
	}

	std::optional<NetworkMessageIntent> MakeGameplayPoseIntent(ConnectionId Connection, const GameplayPoseMessage &Message,
		const NetworkLimits &Limits) {
		if (const auto *Control = std::get_if<GameplayPoseControl>(&Message)) {
			if (!ValidControl(*Control)) return std::nullopt;
			GameBinaryWriter Output(ControlFrameBytes);
			WriteHeader(Output, Control->Published ? FrameKind::Publish : FrameKind::Unpublish, 0, Control->Part,
				Control->MaterializationEpoch);
			Output.Integer(Control->RequiredStructuralSequence);
			if (!Output.Succeeded()) return std::nullopt;
			return MakeNetworkMessageIntent(Connection, DeliveryMode::ReliableOrdered, TrafficClass::Control,
				{}, std::move(Output.Bytes), Limits);
		}
		const auto &Snapshot = std::get<GameplayPoseSnapshot>(Message);
		if (!ValidSnapshot(Snapshot)) return std::nullopt;
		GameBinaryWriter Output(SnapshotFrameBytes);
		WriteHeader(Output, FrameKind::Snapshot, Snapshot.Teleport ? TeleportFlag : std::uint8_t{0}, Snapshot.Part,
			Snapshot.MaterializationEpoch);
		Output.Integer(Snapshot.ServerTick);
		WriteTransform(Output, Snapshot.Transform);
		if (!Output.Succeeded()) return std::nullopt;
		return MakeNetworkMessageIntent(Connection, DeliveryMode::UnreliableSequenced, TrafficClass::RealtimeState,
			RealtimeStateOrder{PoseChannel(Snapshot.Part), RealtimeStateSequence(Snapshot.ServerTick)},
			std::move(Output.Bytes), Limits);
	}

	std::optional<GameplayPoseMessage> DecodeGameplayPose(const ReceivedMessageEvent &Event) {
		if (!Event.Connection.IsValid() ||
			(Event.Payload.size() != ControlFrameBytes && Event.Payload.size() != SnapshotFrameBytes)) return std::nullopt;
		GameBinaryReader Input(Event.Payload, "Gameplay pose");
		std::uint32_t Magic;
		std::uint16_t Version;
		std::uint8_t Kind;
		std::uint8_t Flags;
		ObjectId Part;
		std::uint64_t MaterializationEpoch;
		if (!Input.Integer(Magic) || !Input.Integer(Version) || !Input.Integer(Kind) || !Input.Integer(Flags) ||
			!ReadBinaryObjectId(Input, Part) || !Input.Integer(MaterializationEpoch) || Magic != GameplayPoseMagic ||
			Version != GameplayPoseVersion || !Part.IsValid() || MaterializationEpoch == 0 ||
			Kind > static_cast<std::uint8_t>(FrameKind::Snapshot)) return std::nullopt;
		const auto Frame = static_cast<FrameKind>(Kind);
		if (Frame != FrameKind::Snapshot) {
			GameplayPoseControl Control{.Part = Part, .MaterializationEpoch = MaterializationEpoch,
				.Published = Frame == FrameKind::Publish};
			if (Event.Payload.size() != ControlFrameBytes || Event.Delivery != DeliveryMode::ReliableOrdered ||
				Event.Traffic != TrafficClass::Control || !std::holds_alternative<std::monostate>(Event.Order) || Flags != 0 ||
				!Input.Integer(Control.RequiredStructuralSequence) || !Input.Complete() || !ValidControl(Control))
				return std::nullopt;
			return GameplayPoseMessage(Control);
		}
		const auto *Order = std::get_if<RealtimeStateOrder>(&Event.Order);
		GameplayPoseSnapshot Snapshot{.Part = Part, .MaterializationEpoch = MaterializationEpoch,
			.Teleport = Flags == TeleportFlag};
		if (Event.Payload.size() != SnapshotFrameBytes || Event.Delivery != DeliveryMode::UnreliableSequenced ||
			Event.Traffic != TrafficClass::RealtimeState || !Order || Order->Channel != PoseChannel(Part) ||
			Flags > TeleportFlag || !Input.Integer(Snapshot.ServerTick) || Snapshot.ServerTick == 0 ||
			Order->Sequence.Value() != Snapshot.ServerTick || !ReadTransform(Input, Snapshot.Transform) || !Input.Complete())
			return std::nullopt;
		return GameplayPoseMessage(Snapshot);
	}

	bool GameplayPosePresentation::ApplyControl(const GameplayPoseControl &Control) {
		if (!ValidControl(Control)) return false;
		const auto Found = Replicas.find(Control.Part);
		if (Control.Published) {
			if (Control.MaterializationEpoch <= HighestPublishedEpoch || Found != Replicas.end() ||
				Replicas.size() >= MaximumGameplayPoseReplicas) return false;
			Replicas.emplace(Control.Part, Replica{.MaterializationEpoch = Control.MaterializationEpoch});
			HighestPublishedEpoch = Control.MaterializationEpoch;
			return true;
		}
		if (Found == Replicas.end()) return Control.MaterializationEpoch <= HighestPublishedEpoch;
		if (Found->second.MaterializationEpoch != Control.MaterializationEpoch) return false;
		Replicas.erase(Found);
		return true;
	}

	void GameplayPosePresentation::Forget(ObjectId Part) {
		Replicas.erase(Part);
	}

	void GameplayPosePresentation::Clear() {
		Replicas.clear();
		HighestPublishedEpoch = 0;
	}

	bool GameplayPosePresentation::Accept(const GameplayPoseSnapshot &Snapshot, std::uint64_t LocalTick) {
		const auto Found = Replicas.find(Snapshot.Part);
		if (Found == Replicas.end() || Snapshot.MaterializationEpoch != Found->second.MaterializationEpoch ||
			!ValidSnapshot(Snapshot)) return false;
		auto &State = Found->second;
		if (State.Count != 0) {
			const auto &Latest = State.History[State.Count - 1];
			if (Snapshot.ServerTick <= Latest.ServerTick || LocalTick < State.LastReceiptTick) return false;
			const auto Jump = Snapshot.Transform.Position - Latest.Transform.Position;
			if (Snapshot.Teleport || glm::dot(Jump, Jump) >= MaximumInterpolationGap * MaximumInterpolationGap ||
				Snapshot.ServerTick - Latest.ServerTick > MaximumGameplayPoseWindowTicks) State.Count = 0;
		}
		std::size_t First = State.Count == State.History.size() ? 1 : 0;
		while (First < State.Count &&
			Snapshot.ServerTick - State.History[First].ServerTick > MaximumGameplayPoseWindowTicks) ++First;
		if (First != 0) {
			std::move(State.History.begin() + First, State.History.begin() + State.Count, State.History.begin());
			State.Count -= First;
		}
		State.History[State.Count++] = Snapshot;
		State.LastReceiptTick = LocalTick;
		return true;
	}

	std::optional<GameplayPoseSnapshot> GameplayPosePresentation::GetLatest(ObjectId Part) const {
		const auto Found = Replicas.find(Part);
		if (Found == Replicas.end() || Found->second.Count == 0) return std::nullopt;
		return Found->second.History[Found->second.Count - 1];
	}

	std::optional<CFrame> GameplayPosePresentation::Sample(ObjectId Part, std::uint64_t LocalTick) {
		const auto Found = Replicas.find(Part);
		if (Found == Replicas.end() || Found->second.Count == 0) return std::nullopt;
		auto &State = Found->second;
		const auto &Oldest = State.History[0];
		const auto &Newest = State.History[State.Count - 1];
		const auto Elapsed = LocalTick >= State.LastReceiptTick ? LocalTick - State.LastReceiptTick : 0;
		const auto RemainingDelay = GameplayPoseInterpolationDelayTicks - std::min(Elapsed, GameplayPoseInterpolationDelayTicks);
		const auto DelayedTick = Newest.ServerTick - std::min(Newest.ServerTick, RemainingDelay);
		const auto Target = std::max({DelayedTick, State.LastPresentedServerTick, Oldest.ServerTick});
		State.LastPresentedServerTick = Target;
		if (Target == Oldest.ServerTick) return Oldest.Transform;
		for (std::size_t Index = 1; Index < State.Count; ++Index) {
			const auto &Right = State.History[Index];
			if (Target > Right.ServerTick) continue;
			const auto &Left = State.History[Index - 1];
			const auto Alpha = static_cast<double>(Target - Left.ServerTick) /
				static_cast<double>(Right.ServerTick - Left.ServerTick);
			return Left.Transform.Lerp(Right.Transform, Alpha);
		}
		return Newest.Transform;
	}
}
