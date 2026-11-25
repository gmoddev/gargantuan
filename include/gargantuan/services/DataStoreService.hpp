#pragma once

#include "gargantuan/services/generated/DataStoreService.hpp"

namespace gargantuan {
	class DataStoreService : public Instance {
		I_DataStoreService;
	};

	void RegisterDataStoreUserdata(lua_State* L);
}
