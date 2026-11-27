#include "gargantuan/scripting/RootScriptDebug.hpp"

#include <stdexcept>

namespace gargantuan {
	thread_local RootScriptDebugTurn *RootScriptDebugTurn::Active = nullptr;

	RootScriptDebugTurn::RootScriptDebugTurn(lua_State *Thread, int PreviousLine)
		: Thread(Thread), Prior(lua_callbacks(Thread)->debugstep), PreviousLine(PreviousLine) {
		if (Active || Prior) throw std::logic_error("[Editor:PlayDebugger] Debug hook already owned");
		Active = this;
		lua_callbacks(Thread)->debugstep = Step;
		lua_singlestep(Thread, true);
	}

	RootScriptDebugTurn::~RootScriptDebugTurn() {
		lua_singlestep(Thread, false);
		lua_callbacks(Thread)->debugstep = Prior;
		Active = nullptr;
	}

	void RootScriptDebugTurn::Step(lua_State *Thread, lua_Debug *Frame) {
		auto *Turn = Active;
		if (!Turn || Turn->Thread != Thread || lua_stackdepth(Thread) != 1 || !lua_isyieldable(Thread)) return;
		const int Line = Frame->currentline;
		if (Line <= 0 || Line == Turn->PreviousLine) return;
		Turn->Line = Line;
		lua_break(Thread);
	}
}
