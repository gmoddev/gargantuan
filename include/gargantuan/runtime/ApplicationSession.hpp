// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>

struct lua_State;
namespace gargantuan {
	class ApplicationPresentation;
	class Instance;
	enum class SessionCommand { StartLocal, Leave, Pause, Resume };

	class ApplicationSession final {
	  public:
		using Executor = std::function<void(SessionCommand)>;
		[[nodiscard]] bool IsActive() const { return Active; }
		[[nodiscard]] bool IsPaused() const { return Active && Paused; }
		void Commit(ApplicationPresentation &App, const Executor &Execute);
		static int Request(lua_State *L, Instance *Receiver, SessionCommand Command);
		static int GetInfo(lua_State *L, Instance *Receiver);
	  private:
		struct Operation;
		std::shared_ptr<Operation> Pending;
		std::uint64_t Generation = 0;
		bool Active = false;
		bool Paused = false;
	};
}
