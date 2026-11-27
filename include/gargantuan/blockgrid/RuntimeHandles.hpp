#pragma once
#include <memory>
#include <vector>

namespace gargantuan::blockgrid::binding {
	struct Handle;
	struct HandleSet {
		std::vector<std::weak_ptr<Handle>> Handles;
		void Prune();
		~HandleSet();
	};
}
