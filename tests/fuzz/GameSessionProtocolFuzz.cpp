#include "gargantuan/network/GameSessionProtocol.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *Data, std::size_t Size) {
	const auto Bytes = std::span(reinterpret_cast<const std::byte *>(Data), Size);
	(void)gargantuan::network::DecodeGameSessionMessage(Bytes);
	return 0;
}
