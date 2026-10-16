#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace SteamNetworkingSocketsLib {

// The actor owns this queue under the existing connection lock.  A datagram
// enters it before decryption, sequence tracking, stats, or SNP ACK mutation.
// A rejected datagram is ordinary UDP loss, and permanently invalidates the
// affected native-egress qualification sample.
class GargantuanPendingNativeDatagramQueue {
public:
	static constexpr std::size_t MaximumDatagrams = 64;
	static constexpr std::size_t MaximumDatagramBytes = 1300;

	struct Datagram {
		std::array<std::uint8_t, MaximumDatagramBytes> Bytes{};
		std::int64_t ReceivedAtMicroseconds = 0;
		std::uint16_t Size = 0;
	};

	void Begin() noexcept {
		Active = true;
		Overflowed = false;
		Head = 0;
		Count = 0;
		HighWater = 0;
	}

	bool Push(const void *Bytes, int Size, std::int64_t ReceivedAtMicroseconds) noexcept {
		if (!Active || !Bytes || Size <= 0 ||
			static_cast<std::size_t>(Size) > MaximumDatagramBytes || Count == MaximumDatagrams) {
			Overflowed = true;
			return false;
		}
		Datagram &Next = Queue[(Head + Count) % MaximumDatagrams];
		std::memcpy(Next.Bytes.data(), Bytes, static_cast<std::size_t>(Size));
		Next.Size = static_cast<std::uint16_t>(Size);
		Next.ReceivedAtMicroseconds = ReceivedAtMicroseconds;
		++Count;
		if (Count > HighWater) HighWater = Count;
		return true;
	}

	bool Take(Datagram &Result) noexcept {
		if (!Count) return false;
		Result = Queue[Head];
		Head = (Head + 1) % MaximumDatagrams;
		--Count;
		return true;
	}

	bool End() noexcept {
		if (Count) return false;
		Active = false;
		return true;
	}

	bool IsActive() const noexcept { return Active; }
	bool HasOverflowed() const noexcept { return Overflowed; }
	std::size_t Size() const noexcept { return Count; }
	std::size_t MaximumObserved() const noexcept { return HighWater; }

private:
	std::array<Datagram, MaximumDatagrams> Queue{};
	std::size_t Head = 0;
	std::size_t Count = 0;
	std::size_t HighWater = 0;
	bool Active = false;
	bool Overflowed = false;
};

} // namespace SteamNetworkingSocketsLib
