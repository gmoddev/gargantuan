#pragma once
#include <cstdint>

namespace gargantuan::runtime_detail {
// Private acceptance metadata, independent of optional qualification identities.
// Origins 0..5 are the scheduler's FIFO priorities; 6 is direct submission.
struct OrdinaryServiceMetadata {
	std::uint64_t AcceptedAtMicroseconds = 0;
	std::uint8_t Origin = 6;
	bool Protected = false;
};
inline thread_local OrdinaryServiceMetadata ActiveOrdinaryService;
class OrdinaryServiceScope final {
	OrdinaryServiceMetadata Previous;
public:
	explicit OrdinaryServiceScope(OrdinaryServiceMetadata Value) noexcept : Previous(ActiveOrdinaryService) {
		ActiveOrdinaryService = Value;
	}
	~OrdinaryServiceScope() { ActiveOrdinaryService = Previous; }
	OrdinaryServiceScope(const OrdinaryServiceScope &) = delete;
	OrdinaryServiceScope &operator=(const OrdinaryServiceScope &) = delete;
};
}
