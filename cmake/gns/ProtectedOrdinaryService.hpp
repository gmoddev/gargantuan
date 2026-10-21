#pragma once
#include <array>
#include <cstdint>

// Sender-actor bookkeeping over existing reliable messages. These links own no
// payload/reference and are never copied into a feedback snapshot or wire data.
class GargantuanProtectedOrdinaryService {
public:
	static constexpr std::uint64_t DeadlineMicroseconds = 50'000;
	static constexpr unsigned Origins = 7;
	struct Message {
		GargantuanProtectedOrdinaryService *Owner = nullptr;
		Message *Previous = nullptr, *Next = nullptr;
		std::uint64_t AcceptedAtMicroseconds = 0;
		std::uint64_t MessageNumber = 0; // Native FIFO cutoff; owns no message.
		std::uint32_t RemainingBodyBytes = 0;
		unsigned Origin = 0;
		Message() = default;
		~Message() { if (Owner) Owner->Released(*this); }
		Message(const Message &) = delete;
		Message &operator=(const Message &) = delete;
	};
	bool EverFailed = false, ActiveDeadlineFailed = false, EvidenceInvalid = false;
	std::uint64_t MaximumLatencyMicroseconds = 0;
	GargantuanProtectedOrdinaryService() = default;
	~GargantuanProtectedOrdinaryService() { Purge(); }
	GargantuanProtectedOrdinaryService(const GargantuanProtectedOrdinaryService &) = delete;
	GargantuanProtectedOrdinaryService &operator=(const GargantuanProtectedOrdinaryService &) = delete;
	std::uint64_t LastPendingMessage() const noexcept {
		std::uint64_t Last = 0;
		for (const auto *Tail : Tails) if (Tail && Tail->MessageNumber > Last) Last = Tail->MessageNumber;
		return Last;
	}

	bool Enroll(Message &Value, unsigned Origin, std::uint64_t AcceptedAt,
		std::uint32_t BodyBytes) noexcept {
		// The declared Main actor and each scheduler-origin FIFO provide this
		// ordering. Generic concurrent direct Send is not a protected declaration.
		// Unsupported metadata fails health evidence, not delivery or ownership.
		if (Value.Owner || Origin >= Origins || !AcceptedAt || !BodyBytes ||
			AcceptedAt < LastAccepted[Origin]) {
			EvidenceInvalid = true; return false;
		}
		Value.Owner = this; Value.Origin = Origin;
		Value.AcceptedAtMicroseconds = AcceptedAt; Value.RemainingBodyBytes = BodyBytes;
		Value.Previous = Tails[Origin]; Value.Next = nullptr;
		if (Tails[Origin]) Tails[Origin]->Next = &Value;
		else Heads[Origin] = &Value;
		Tails[Origin] = &Value; LastAccepted[Origin] = AcceptedAt;
		return true;
	}

	void FirstSend(Message &Value, std::uint32_t BodyBytes, std::uint64_t CompletedAt) noexcept {
		if (!Value.Owner) return; // Bootstrap, overload or already complete.
		if (Value.Owner != this || !CompletedAt || CompletedAt < Value.AcceptedAtMicroseconds ||
			BodyBytes > Value.RemainingBodyBytes) {
			EvidenceInvalid = true; return;
		}
		if (!BodyBytes) return;
		Value.RemainingBodyBytes -= BodyBytes;
		const auto Latency = CompletedAt - Value.AcceptedAtMicroseconds;
		if (Latency > DeadlineMicroseconds) EverFailed = true;
		if (!Value.RemainingBodyBytes) {
			if (Latency > MaximumLatencyMicroseconds) MaximumLatencyMicroseconds = Latency;
			Forget(Value);
		}
	}

	// Call only after pending native outcomes have been reconciled. Heads include
	// messages referenced by the provisional packet after SNP cursor advancement.
	// One earliest timestamp per proven-monotonic origin covers all owned bodies.
	void Observe(std::uint64_t ResolvedThrough) noexcept {
		ActiveDeadlineFailed = false;
		for (const auto *Head : Heads) {
			if (!Head) continue;
			if (ResolvedThrough < Head->AcceptedAtMicroseconds) {
				EvidenceInvalid = true; continue;
			}
			if (ResolvedThrough - Head->AcceptedAtMicroseconds > DeadlineMicroseconds) {
				ActiveDeadlineFailed = true; EverFailed = true;
			}
		}
	}

	void Forget(Message &Value) noexcept {
		if (Value.Owner != this) return;
		const auto Origin = Value.Origin;
		if (Value.Previous) Value.Previous->Next = Value.Next;
		else Heads[Origin] = Value.Next;
		if (Value.Next) Value.Next->Previous = Value.Previous;
		else Tails[Origin] = Value.Previous;
		Value.Owner = nullptr; Value.Previous = Value.Next = nullptr;
	}
	void Released(Message &Value) noexcept {
		// Normal first-send completion and terminal purge detached this node
		// already. Unexpected early release cannot erase an unmet obligation.
		EvidenceInvalid = true;
		Forget(Value);
	}
	void Purge() noexcept {
		for (unsigned Origin = 0; Origin < Origins; ++Origin)
			while (Heads[Origin]) Forget(*Heads[Origin]);
		ActiveDeadlineFailed = false;
	}
private:
	std::array<Message *, Origins> Heads{}, Tails{};
	std::array<std::uint64_t, Origins> LastAccepted{};
};
