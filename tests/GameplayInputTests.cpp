#include "gargantuan/network/GameplayInput.hpp"
#include "gargantuan/network/Scheduler.hpp"
#include "gargantuan/network/Transport.hpp"

#include <cstdlib>
#include <deque>
#include <iostream>
#include <limits>
#include <utility>

namespace {
	using namespace gargantuan;
	using namespace gargantuan::network;

	constexpr ConnectionId First{1, 1};
	constexpr ConnectionId Second{2, 1};
	constexpr ObjectId FirstEntity{11, 1};
	constexpr ObjectId SecondEntity{12, 1};
	const NetworkLimits Limits = NetworkLimits::NativeCeilings();

	void Check(bool Condition, const char *Message) {
		if (Condition) return;
		std::cerr << "[Gameplay:InputTests] " << Message << '\n';
		std::exit(1);
	}

	class RecordingScheduler final : public INetworkScheduler {
	  public:
		bool RejectNext = false;
		std::deque<NetworkMessageIntent> Queued;

		bool RegisterConnection(ConnectionId, const NetworkLimits &) override { return true; }
		SchedulerSubmitResult Submit(NetworkMessageIntent Intent) override {
			if (std::exchange(RejectNext, false)) return {SchedulerSubmitStatus::IntentRejected, std::nullopt};
			Queued.push_back(std::move(Intent));
			return {SchedulerSubmitStatus::Accepted, std::nullopt};
		}
		SchedulerFlushResult Flush(ConnectionId, SchedulerTickBudget) override { return {}; }
		bool CancelConnection(ConnectionId) override { return true; }
		std::optional<SchedulerStatistics> GetStatistics(ConnectionId) const override { return std::nullopt; }

		ReceivedMessageEvent Take() {
			Check(!Queued.empty(), "Expected a queued packet");
			auto Intent = std::move(Queued.front());
			Queued.pop_front();
			return {Intent.Destination(), Intent.Delivery(), Intent.Traffic(), Intent.Order(), Intent.Payload()};
		}
	};

	struct Fixture {
		RecordingScheduler ServerScheduler;
		RecordingScheduler ClientScheduler;
		GameplayInputServer Server{ServerScheduler};
		GameplayInputClient Client{ClientScheduler};

		void Bind() {
			Check(Server.SetPossession(First, FirstEntity, 50, Limits, 1), "First entity lease could not bind");
			auto Bind = ServerScheduler.Take();
			Check(IsGameplayInputFrame(Bind.Payload) && Client.HandleReceived(Bind), "Invalid reliable lease bind");
			Check(!Client.HandleReceived(Bind), "Duplicate control bind was accepted");
			Check(Client.GetPossessedEntity() == FirstEntity, "Client resolved the wrong generic possession");
		}

		ReceivedMessageEvent Submit(std::uint64_t Tick, glm::vec2 Movement = {0.5f, 0.0f}, bool Jump = false) {
			Check(Client.Submit(First, Client.GetPossessedEntity(), Tick, Movement, 0.25f, Jump, Limits),
				"Valid input could not submit");
			return ClientScheduler.Take();
		}
	};

	void TestLatestInputAndExpiry() {
		Fixture World;
		World.Bind();
		Check(World.Server.SetPossession(First, FirstEntity, 51, Limits, 1) && World.ServerScheduler.Queued.empty(),
			"A stable possession emitted another lease");
		const auto Empty = World.Server.Consume(First, FirstEntity, 51, 1);
		Check(Empty && !Empty->Fresh && !Empty->AcknowledgedInput.IsValid(), "No-input tick was not neutral");
		Check(!World.Server.Consume(First, FirstEntity, 51, 1), "A repeated simulation tick consumed input again");
		const auto FirstMove = World.Submit(51, {1.0f, 0.0f}, true);
		const auto SecondMove = World.Submit(52, {0.0f, -1.0f});
		Check(World.Server.HandleReceived(FirstMove, 52) && World.Server.HandleReceived(SecondMove, 52),
			"Ordered inputs could not be received");
		Check(!World.Server.HandleReceived(FirstMove, 52) && !World.Server.HandleReceived(SecondMove, 52),
			"Reordered or replayed input reached the pending sample");
		const auto Consumed = World.Server.Consume(First, FirstEntity, 53, 1);
		Check(Consumed && Consumed->Movement == glm::vec2(0.0f, -1.0f) && Consumed->Fresh &&
			Consumed->FacingYawRadians == 0.25f && Consumed->JumpRequested && Consumed->AgeTicks == 1 &&
			Consumed->AuthoritativeTick == 53 && Consumed->AcknowledgedInput.Value() == 2,
			"Latest sample did not preserve a queued jump edge and canonical tick");
		const auto Held = World.Server.Consume(First, FirstEntity, 52 + GameplayInputFreshnessTicks, 1);
		Check(Held && Held->Fresh && !Held->JumpRequested && Held->Movement == Consumed->Movement,
			"Holding latest intent repeated an edge or dropped early");
		const auto Expired = World.Server.Consume(First, FirstEntity, 53 + GameplayInputFreshnessTicks, 1);
		Check(Expired && !Expired->Fresh && Expired->Movement == glm::vec2(0.0f) && !Expired->JumpRequested &&
			Expired->AcknowledgedInput.Value() == 2, "Expired input did not become neutral");
		const auto &Metrics = World.Server.GetMetrics();
		Check(Metrics.InputsAccepted == 2 && Metrics.InputsConsumed == 1 && Metrics.InputsCoalesced == 1 &&
			World.Client.GetInputsProduced() == 2 && Metrics.StaleInputs == 2 &&
			Metrics.InputsExpired == 1 && Metrics.MaximumConsumedInputAgeTicks == 1,
			"Input diagnostics diverged from accepted and consumed state");
	}

	void TestAdmissionBounds() {
		Fixture World;
		World.Bind();
		Check(!World.Client.Submit(First, SecondEntity, 51, {0.0f, 0.0f}, 0.0f, false, Limits),
			"Client accepted input with an unrelated replicated target");
		Check(!World.Client.Submit(First, FirstEntity, 51, {1.0f, 1.0f}, 0.0f, false, Limits) &&
			!World.Client.Submit(First, FirstEntity, 51, {0.0f, 0.0f},
				std::numeric_limits<float>::infinity(), false, Limits), "Invalid client movement entered the scheduler");
		const auto Valid = World.Submit(51);
		auto Rejected = Valid;
		Rejected.Connection = Second;
		Check(!World.Server.HandleReceived(Rejected, 51), "Unbound peer issued a valid-looking command");
		Rejected = Valid;
		Rejected.Payload.push_back(std::byte{});
		Check(!World.Server.HandleReceived(Rejected, 51), "Overlong input entered the lease buffer");
		Rejected = Valid;
		Rejected.Order = RealtimeStateOrder{StateChannelId(2), RealtimeStateSequence(1)};
		Check(!World.Server.HandleReceived(Rejected, 51), "An unrelated scheduler channel was admitted");
		Rejected = Valid;
		Rejected.Payload[7] = std::byte{2};
		Check(!World.Server.HandleReceived(Rejected, 51), "An unknown intent flag was admitted");
		Check(World.Server.HandleReceived(Valid, 51) && !World.Server.HandleReceived(Valid, 51),
			"Malformed bound-peer attempts failed to consume the per-tick budget");
		const auto Ahead = World.Submit(1000);
		Check(World.Server.HandleReceived(Ahead, 60), "Independent client frame count blocked a current input");
		const auto Next = World.Submit(1001);
		Check(World.Server.HandleReceived(Next, 61) && !World.Server.HandleReceived(Ahead, 61),
			"Newer input did not supersede the old sequence");
		const auto &Metrics = World.Server.GetMetrics();
		Check(Metrics.MalformedInputs == 3 && Metrics.UnauthorizedInputs == 1 && Metrics.RateLimitedInputs == 1 &&
			Metrics.StaleInputs == 1, "Input rejection counters lost classification");
	}

	void TestRevocationAndReuse() {
		Fixture World;
		World.Bind();
		const auto Prior = World.Submit(53, {1.0f, 0.0f}, true);
		Check(World.Server.HandleReceived(Prior, 54), "Initial lease input was rejected");
		Check(!World.Server.Consume(First, FirstEntity, 55, 2) &&
			!World.Server.Consume(First, SecondEntity, 55, 1), "The current relation was not validated at consumption");
		Check(!World.Server.SetPossession(Second, FirstEntity, 55, Limits, 1),
			"Two connections leased the same entity concurrently");
		Check(World.Server.SetPossession(First, FirstEntity, 55, Limits, 2), "Same-entity new revision did not rebind");
		const auto OldUnbind = World.ServerScheduler.Take();
		Check(World.Client.HandleReceived(OldUnbind) && !World.Client.GetPossessedEntity().IsValid(),
			"Reliable control revoke failed");
		Check(World.Client.HandleReceived(World.ServerScheduler.Take()) && !World.Client.HandleReceived(OldUnbind),
			"Stale reliable revoke closed the new lease");
		Check(!World.Server.HandleReceived(Prior, 55), "An old lease replay survived same-entity rebind");
		const auto Clean = World.Server.Consume(First, FirstEntity, 56, 2);
		Check(Clean && !Clean->Fresh && !Clean->AcknowledgedInput.IsValid(), "Old input survived rebind");
		const auto Current = World.Submit(55);
		Check(World.Server.HandleReceived(Current, 56), "The current input epoch did not resume");
		const auto Accepted = World.Server.Consume(First, FirstEntity, 57, 2);
		Check(Accepted && Accepted->AcknowledgedInput.Value() == 2, "Input sequence reset on the realtime channel");
		World.Server.RemovePeer(First);
		Check(!World.Server.HandleReceived(Current, 58) && !World.Server.Consume(First, FirstEntity, 58, 2),
			"Distrusted disconnected lease survived cleanup");
		World.Client.Reset();
		constexpr ConnectionId Reconnected{First.Slot, First.Generation + 1};
		constexpr ObjectId Recycled{FirstEntity.Slot, FirstEntity.Generation + 1};
		Check(World.Server.SetPossession(Reconnected, Recycled, 59, Limits, 1) &&
			World.Client.HandleReceived(World.ServerScheduler.Take()), "Recycled entity and connection did not bind");
		Check(!World.Server.HandleReceived(Current, 59) && !World.Server.Consume(Reconnected, FirstEntity, 60, 1),
			"Connection or entity slot reuse accepted the previous generation");
	}

	void TestReliableFailureAndCapacity() {
		Fixture World;
		World.ServerScheduler.RejectNext = true;
		Check(!World.Server.SetPossession(First, FirstEntity, 49, Limits, 1), "Rejected bind was reported as accepted");
		Check(!World.Server.Consume(First, FirstEntity, 49, 1), "A rejected bind left a live lease");
		World.Bind();
		World.ServerScheduler.RejectNext = true;
		Check(!World.Server.SetPossession(First, SecondEntity, 55, Limits, 2), "Rejected revoke was reported as accepted");
		Check(!World.Server.Consume(First, FirstEntity, 55, 1) && !World.Server.Consume(First, SecondEntity, 55, 2),
			"Reliable queue failure left a stale or partial lease");
		for (std::uint32_t Index = 1; Index <= MaximumGameplayInputLeases; ++Index) {
			Check(World.Server.SetPossession({Index, 2}, {Index, 2}, 60, Limits), "Lease bound rejected an eligible peer");
		}
		constexpr auto Overflow = static_cast<std::uint32_t>(MaximumGameplayInputLeases + 1);
		Check(!World.Server.SetPossession({Overflow, 2}, {Overflow, 2}, 60, Limits), "Lease capacity exceeded its ceiling");
		World.Server.RemovePeer({1, 2});
		Check(World.Server.SetPossession({Overflow, 2}, {Overflow, 2}, 60, Limits),
			"Lease cleanup did not release one capacity slot");
	}
}

int main() {
	TestLatestInputAndExpiry();
	TestAdmissionBounds();
	TestRevocationAndReuse();
	TestReliableFailureAndCapacity();
	std::cout << "[Gameplay:InputTests] Passed\n";
}
