#include "gargantuan/serialization/SerializationError.hpp"

namespace gargantuan {
	std::string SerializationError::Format() const {
		if (Path.empty()) return Message;
		return Path + ": " + Message;
	}
}
