#pragma once

#include <cstddef>
#include <lobject.h>

namespace gargantuan {
	// The public iterator visits empty storage as well as live entries. Call
	// only with a table whose type was verified on this VM's stack.
	inline std::size_t LuauTableStorageSlots(const void *Identity) {
		const auto *Table = static_cast<const LuaTable *>(Identity);
		return static_cast<std::size_t>(Table->sizearray) + (std::size_t{1} << Table->lsizenode);
	}
}
