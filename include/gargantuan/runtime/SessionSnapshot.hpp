// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "gargantuan/runtime/ApplicationSession.hpp"
#include "gargantuan/scripting/StackValue.hpp"

namespace gargantuan {
	struct SessionSnapshot {
		bool Active;
		std::uint64_t Generation;
		const char *State;
		std::optional<WorldSlot> Slot;
		bool Remote, Private, Hosting;
		std::optional<SessionFailure> LastError;
	};
	template <> struct StackValue<SessionSnapshot> {
		static int Push(lua_State *L, const SessionSnapshot &Snapshot);
	};
}
