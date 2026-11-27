#include "gargantuan/scripting/RootScriptDebug.hpp"

#include <luacode.h>
#include <lualib.h>

#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
	void Check(bool Value, const char *Message) {
		if (!Value) throw std::runtime_error(Message);
	}
}

int main() {
	try {
		const std::unique_ptr<lua_State, decltype(&lua_close)> Main(luaL_newstate(), lua_close);
		Check(Main != nullptr, "Luau VM allocation");
		luaL_openlibs(Main.get());
		auto *Thread = lua_newthread(Main.get());
		const char *Text = "debug_first=7\nlocal function nested()\n debug_nested=4\nend\nnested()\ndebug_last=9";
		std::size_t Size = 0;
		lua_CompileOptions Options{.debugLevel = 1, .vectorLib = "Vector3", .vectorCtor = "new", .vectorType = "Vector3"};
		const std::unique_ptr<char, decltype(&std::free)> Bytes(luau_compile(Text, std::strlen(Text), &Options, &Size), std::free);
		Check(Bytes && luau_load(Thread, "RootFixture", Bytes.get(), Size, 0) == LUA_OK, "fixture bytecode loaded");
		int Previous = 0;
		for (int Expected : {1, 2, 5, 6}) {
			{
				gargantuan::RootScriptDebugTurn Turn(Thread, Previous);
				Check(lua_resume(Thread, Main.get(), 0) == LUA_BREAK && Turn.GetLine() == Expected,
					"step parks before the next root line and skips the nested frame");
			}
			Check(lua_callbacks(Main.get())->debugstep == nullptr, "debug hook retires after the turn");
			Previous = Expected;
		}
		Check(lua_resume(Thread, Main.get(), 0) == LUA_OK, "continue completes the parked owner");
		lua_getglobal(Thread, "debug_last");
		Check(lua_tointeger(Thread, -1) == 9, "continue executes the last line");
		std::cout << "[Editor:RootDebugger] Root stepping, nested frame and hook retirement PASS\n";
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Editor:RootDebugger] " << Error.what() << '\n';
		return 1;
	}
}
