#include "gargantuan/network/GameplayPose.hpp"
#include "gargantuan/network/Transport.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numbers>

namespace {
	using namespace gargantuan;
	using namespace gargantuan::network;

	constexpr ConnectionId Server{1, 1};
	constexpr ObjectId Part{10, 1};
	constexpr ObjectId Replacement{10, 2};
	const auto Limits = NetworkLimits::NativeCeilings();

	void Check(bool Condition, const char *Message) {
		if (Condition) return;
		std::cerr << "[Gameplay:PoseTests] " << Message << '\n';
		std::exit(1);
	}

	GameplayPoseControl Publish(ObjectId Object = Part, std::uint64_t Epoch = 1) {
		return {.Part = Object, .MaterializationEpoch = Epoch, .Published = true, .RequiredStructuralSequence = 7};
	}

	GameplayPoseSnapshot Pose(std::uint64_t Tick, float X, ObjectId Object = Part, std::uint64_t Epoch = 1) {
		return {.Part = Object, .MaterializationEpoch = Epoch, .ServerTick = Tick, .Transform = CFrame(X, 0, 0)};
	}

	ReceivedMessageEvent Encode(const GameplayPoseMessage &Message) {
		const auto Intent = MakeGameplayPoseIntent(Server, Message, Limits);
		Check(Intent.has_value() && IsGameplayPoseFrame(Intent->Payload()), "Encoder did not produce a GPOS frame");
		return {Intent->Destination(), Intent->Delivery(), Intent->Traffic(), Intent->Order(), Intent->Payload()};
	}

	void TestCodecAndChannelIsolation() {
		const auto ControlEvent = Encode(Publish());
		const auto ControlMessage = DecodeGameplayPose(ControlEvent);
		const auto *Control = ControlMessage ? std::get_if<GameplayPoseControl>(&*ControlMessage) : nullptr;
		Check(Control && Control->Part == Part && Control->MaterializationEpoch == 1 && Control->Published &&
			Control->RequiredStructuralSequence == 7, "Publish lost its generation or structural watermark");
		Check(!MakeGameplayPoseIntent(Server, GameplayPoseControl{.Part = Part, .MaterializationEpoch = 1,
			.Published = true}, Limits), "Publish was sent without its structural watermark");
		auto Snapshot = Pose(103, 3);
		Snapshot.Transform.Rotation = CFrame::Angles(0, std::numbers::pi_v<float> / 2.0f, 0).Rotation;
		Snapshot.Teleport = true;
		const auto State = Encode(Snapshot);
		const auto StateMessage = DecodeGameplayPose(State);
		const auto *Decoded = StateMessage ? std::get_if<GameplayPoseSnapshot>(&*StateMessage) : nullptr;
		Check(Decoded && Decoded->Part == Part && Decoded->ServerTick == Snapshot.ServerTick && Decoded->Teleport &&
			Decoded->Transform.FuzzyEq(Snapshot.Transform), "Sequenced pose did not round-trip an authoritative transform");
		const auto Other = Encode(Pose(103, 6, ObjectId{11, 1}));
		const auto &Order = std::get<RealtimeStateOrder>(State.Order);
		Check((Order.Channel.Value() >> 32) == 0 && Order.Channel != std::get<RealtimeStateOrder>(Other.Order).Channel,
			"Pose channel collided with another part or the stock channel namespace");
		const auto Reused = Encode(Pose(104, 3, Replacement, 2));
		Check(std::get<RealtimeStateOrder>(Reused.Order).Channel == Order.Channel &&
			std::get<RealtimeStateOrder>(Reused.Order).Sequence.IsNewerThan(Order.Sequence),
			"Recycled pose slot did not preserve monotonic scheduler order");
		auto InvalidControl = ControlEvent;
		InvalidControl.Delivery = DeliveryMode::UnreliableUnordered;
		Check(!DecodeGameplayPose(InvalidControl), "Unreliable publish was accepted");
		auto Invalid = State;
		Invalid.Order = RealtimeStateOrder{Order.Channel, RealtimeStateSequence(102)};
		Check(!DecodeGameplayPose(Invalid), "Snapshot accepted mismatched order metadata");
		Invalid = State;
		Invalid.Payload[7] = std::byte{2};
		Check(!DecodeGameplayPose(Invalid), "Unrecognized snapshot flag was accepted");
		Invalid = State;
		Invalid.Payload.pop_back();
		Check(!DecodeGameplayPose(Invalid), "Truncated pose was accepted");
		Snapshot.Transform.Position.x = std::numeric_limits<float>::quiet_NaN();
		Check(!MakeGameplayPoseIntent(Server, Snapshot, Limits), "Nonfinite pose was encoded");
		Snapshot.Transform.Position.x = 0.0f;
		Snapshot.Transform.Rotation[0] *= -1.0f;
		Check(!IsValidGameplayPoseTransform(Snapshot.Transform), "A reflected basis was accepted as orientation");
	}

	void TestInterpolationAndDiscontinuity() {
		GameplayPosePresentation History;
		Check(History.ApplyControl(Publish()) && !History.GetLatest(Part), "A fresh relation already had a sample");
		Check(History.Accept(Pose(100, 0), 1) && History.Accept(Pose(106, 6), 7), "Valid pose pair was rejected");
		const auto Start = History.Sample(Part, 7);
		const auto Middle = History.Sample(Part, 10);
		const auto End = History.Sample(Part, 13);
		Check(Start && Middle && End && Start->Position.x == 0 && std::abs(Middle->Position.x - 3) < 1.0e-5f &&
			End->Position.x == 6, "Samples were not interpolated on the authoritative clock");
		const auto Latest = History.GetLatest(Part);
		Check(Latest && Latest->ServerTick == 106 && Latest->Transform.Position.x == 6,
			"Presentation altered the latest authoritative transform");
		Check(!History.Accept(Pose(105, 4), 13) && !History.Accept(Pose(106, 1), 14),
			"Reordered or duplicate sample rewrote presentation history");
		Check(History.Accept(Pose(109, 7), 16), "Late-but-new sample was rejected");
		const auto BeforeJitter = History.Sample(Part, 16);
		Check(BeforeJitter && BeforeJitter->Position.x == 6, "Newest receipt moved the presentation clock backward");
		const auto Held = History.Sample(Part, 1000);
		Check(Held && Held->Position.x == 7, "Presentation extrapolated when packets stopped");
		auto Teleport = Pose(110, 5);
		Teleport.Teleport = true;
		Check(History.Accept(Teleport, 1001), "Teleport sample was rejected");
		const auto Snapped = History.Sample(Part, 1001);
		Check(Snapped && Snapped->Position.x == 5, "Teleport retained stale interpolation samples");
		Check(History.Accept(Pose(111, 20), 1002), "A finite discontinuity was rejected");
		const auto LargeJump = History.Sample(Part, 1002);
		Check(LargeJump && LargeJump->Position.x == 20, "Discontinuity was interpolated through intermediate geometry");
	}

	void TestMaterializationAndCapacity() {
		GameplayPosePresentation History;
		Check(!History.Accept(Pose(100, 0), 1), "Pose allocated without a reliable publication");
		Check(History.ApplyControl(Publish()) && History.Accept(Pose(100, 0), 1), "Initial relationship failed");
		const GameplayPoseControl Leave{.Part = Part, .MaterializationEpoch = 1};
		const auto LeaveMessage = DecodeGameplayPose(Encode(Leave));
		Check(LeaveMessage && !std::get<GameplayPoseControl>(*LeaveMessage).Published && History.ApplyControl(Leave),
			"Reliable unpublish failed");
		Check(!History.Sample(Part, 1) && !History.Accept(Pose(101, 1), 2) && !History.ApplyControl(Publish()),
			"Late state or recycled publication reopened the previous relationship");
		Check(History.ApplyControl(Publish(Part, 2)) && !History.Accept(Pose(102, 3), 2) &&
			!History.ApplyControl(Leave), "Old epoch applied to the same source object's new materialization");
		Check(History.Accept(Pose(103, 2, Part, 2), 2), "Reentered object did not accept its current epoch");
		History.Forget(Part);
		Check(!History.GetLatest(Part) && History.ApplyControl({.Part = Part, .MaterializationEpoch = 2}),
			"Structural removal did not retire value history before its reliable leave");
		Check(History.ApplyControl(Publish(Replacement, 3)) &&
			!History.Accept(Pose(104, 4, Part, 3), 2) && History.Accept(Pose(104, 4, Replacement, 3), 2),
			"Recycled ObjectId accepted a snapshot from the previous generation");
		History.Clear();
		for (std::uint32_t Index = 1; Index <= MaximumGameplayPoseReplicas; ++Index)
			Check(History.ApplyControl(Publish({Index, 1}, Index)), "Presentation rejected an eligible binding");
		constexpr auto Extra = static_cast<std::uint32_t>(MaximumGameplayPoseReplicas + 1);
		Check(!History.ApplyControl(Publish({Extra, 1}, Extra)), "Presentation exceeded its relationship limit");
		History.Forget({1, 1});
		Check(History.ApplyControl(Publish({Extra, 1}, Extra)), "Structural removal did not free presentation capacity");
	}
}

int main() {
	TestCodecAndChannelIsolation();
	TestInterpolationAndDiscontinuity();
	TestMaterializationAndCapacity();
	std::cout << "[Gameplay:PoseTests] Passed\n";
}
