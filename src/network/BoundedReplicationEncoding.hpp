#pragma once

#include "gargantuan/network/ReplicationProtocol.hpp"

namespace gargantuan::network::detail {

// Internal hard-message bound, not currently available admission credit. Full
// frame validity precedes size rejection; successful output is canonical GRPL.
SerializationResult<std::vector<std::byte>> EncodeReplicationFrameBounded(
	const ReplicationFrame &Frame, std::size_t MaximumBytes
);

} // namespace gargantuan::network::detail
