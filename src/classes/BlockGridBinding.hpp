#pragma once
#include "gargantuan/classes/BlockGrid.hpp"
#include "gargantuan/blockgrid/Traversal.hpp"

namespace gargantuan::blockgrid::binding {
	BlockGrid& ReadTarget(lua_State* L,Instance* Object,bool Edit=false,bool AllowMissing=false);
	Cell ReadCell(lua_State* L,int Index,bool Exclusive=false);
	std::string_view StringAt(lua_State* L,int Index);
	Block ReadBlock(lua_State* L,const Field& Source,int Key,int Orientation);
	void PushBlock(lua_State* L,const Field& Source,Block B);
	void PushText(lua_State* L,std::string_view S);
	int SubmitRequest(lua_State* L,BlockGrid& Grid,std::shared_ptr<EditRequest> Edit);
	Filter ReadFilter(lua_State* L,int Index,const Field& Source);
}
