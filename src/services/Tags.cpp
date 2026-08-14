#include "gargantuan/services/Tags.hpp"

#include "gargantuan/classes/DataModel.hpp"

#include <lua.h>
#include <stdexcept>

namespace gargantuan {
	namespace {
		std::shared_ptr<DataModel> Owner(Instance *service) {
			auto owner = service->GetDataModel();
			if (!owner) throw std::runtime_error("Tags service is not owned by a DataModel");
			return owner;
		}

		std::shared_ptr<Instance> ReadInstance(lua_State *L, int index) {
			auto instance = StackValue<std::shared_ptr<Instance>>::From(L, index);
			if (!instance) throw std::invalid_argument("Tags requires a live Instance");
			return instance;
		}

		std::string_view ReadString(lua_State *L, int index) {
			size_t length = 0;
			const auto value = luaL_checklstring(L, index, &length);
			return {value, length};
		}

		int PushInstances(lua_State *L, const std::vector<ObjectId> &ids) {
			lua_createtable(L, static_cast<int>(ids.size()), 0);
			int index = 1;
			for (const auto id : ids) {
				auto instance = ObjectRegistry::Get().Lookup(id);
				if (!instance) continue;
				StackValue<std::shared_ptr<Instance>>::Push(L, std::move(instance));
				lua_rawseti(L, -2, index++);
			}
			return 1;
		}
	}

	int Tags::Add(lua_State *L, Instance *service) {
		auto owner = Owner(service);
		auto target = ReadInstance(L, 2);
		if (target->GetDataModel() != owner) throw std::invalid_argument("Tags target belongs to another DataModel");
		const auto tag = ReadString(L, 3);
		MutationGateway gateway;
		auto result = gateway.Apply(AddTagCommand{target->GetObjectId(), std::string(tag), owner->GetObjectId()});
		if (!result.Succeeded()) throw std::runtime_error(result.Message.empty() ? "Tag addition rejected" : result.Message);
		return 0;
	}

	int Tags::Remove(lua_State *L, Instance *service) {
		auto owner = Owner(service);
		auto target = ReadInstance(L, 2);
		if (target->GetDataModel() != owner) throw std::invalid_argument("Tags target belongs to another DataModel");
		const auto tag = ReadString(L, 3);
		MutationGateway gateway;
		auto result = gateway.Apply(RemoveTagCommand{target->GetObjectId(), std::string(tag), owner->GetObjectId()});
		if (!result.Succeeded()) throw std::runtime_error(result.Message.empty() ? "Tag removal rejected" : result.Message);
		return 0;
	}

	int Tags::Has(lua_State *L, Instance *service) {
		auto owner = Owner(service);
		auto target = ReadInstance(L, 2);
		return StackValue<bool>::Push(L, owner->Tags.Has(owner->GetObjectId(), target->GetObjectId(), ReadString(L, 3), GetCurrentScriptSecurityContext()));
	}

	int Tags::GetTags(lua_State *L, Instance *service) {
		auto owner = Owner(service);
		auto target = ReadInstance(L, 2);
		auto tags = owner->Tags.GetTags(owner->GetObjectId(), target->GetObjectId(), GetCurrentScriptSecurityContext());
		lua_createtable(L, static_cast<int>(tags.size()), 0);
		for (std::size_t index = 0; index < tags.size(); ++index) {
			StackValue<std::string>::Push(L, tags[index]);
			lua_rawseti(L, -2, static_cast<int>(index + 1));
		}
		return 1;
	}

	int Tags::GetTagged(lua_State *L, Instance *service) {
		auto owner = Owner(service);
		return PushInstances(L, owner->Tags.GetTagged(owner->GetObjectId(), ReadString(L, 2), GetCurrentScriptSecurityContext()));
	}

	int Tags::GetTaggedAll(lua_State *L, Instance *service) {
		auto owner = Owner(service);
		luaL_checktype(L, 2, LUA_TTABLE);
		std::vector<std::string> names;
		const auto length = lua_objlen(L, 2);
		if (length > MaximumTagsPerQuery) throw std::invalid_argument("Tag query exceeds its name count limit");
		names.reserve(length);
		for (std::size_t index = 1; index <= length; ++index) {
			lua_rawgeti(L, 2, static_cast<int>(index));
			names.emplace_back(ReadString(L, -1));
			lua_pop(L, 1);
		}
		return PushInstances(L, owner->Tags.GetTaggedAll(owner->GetObjectId(), names, GetCurrentScriptSecurityContext()));
	}
}
