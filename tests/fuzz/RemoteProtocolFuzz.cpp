#include "gargantuan/network/RemoteProtocol.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *Data, std::size_t Size) {
	const auto Bytes = std::span(reinterpret_cast<const std::byte *>(Data), Size);
	std::size_t StorageBytes = 0;
	const auto Decoded = gargantuan::network::DecodeRemoteMessage(Bytes, &StorageBytes);
	if (Decoded) {
		const auto Reencoded = gargantuan::network::EncodeRemoteMessage(*Decoded);
		if (!Reencoded || !std::ranges::equal(*Reencoded, Bytes) ||
			gargantuan::network::GetRemoteArgumentStorageBytes(Decoded->Arguments) != StorageBytes)
			std::abort();
	}
	return 0;
}
