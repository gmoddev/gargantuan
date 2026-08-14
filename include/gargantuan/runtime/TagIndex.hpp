#pragma once

#include "gargantuan/runtime/ObjectId.hpp"
#include "gargantuan/scripting/ScriptSecurity.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace gargantuan {
	class Instance;

	inline constexpr std::size_t MaximumTagNameBytes = 100;
	inline constexpr std::size_t MaximumTagsPerInstance = 64;
	inline constexpr std::size_t MaximumTagsPerQuery = MaximumTagsPerInstance;
	inline constexpr std::size_t MaximumDistinctTagsPerDataModel = 1024;

	using TagId = std::uint32_t;
	inline constexpr TagId InvalidTagId = 0;

	void ValidateTagName(std::string_view name);

	class TagIndex {
	  public:
		bool Add(ObjectId scope, ObjectId object, std::string_view name, const ScriptSecurityContext &securityContext);
		bool Remove(ObjectId scope, ObjectId object, std::string_view name, const ScriptSecurityContext &securityContext);

		[[nodiscard]] bool Has(
			ObjectId scope,
			ObjectId object,
			std::string_view name,
			const ScriptSecurityContext &securityContext
		) const;
		[[nodiscard]] std::vector<std::string> GetTags(
			ObjectId scope,
			ObjectId object,
			const ScriptSecurityContext &securityContext
		) const;
		[[nodiscard]] std::vector<ObjectId> GetTagged(
			ObjectId scope,
			std::string_view name,
			const ScriptSecurityContext &securityContext
		) const;
		[[nodiscard]] std::vector<ObjectId> GetTaggedAll(
			ObjectId scope,
			const std::vector<std::string> &names,
			const ScriptSecurityContext &securityContext
		) const;

	  private:
		friend class Instance;
		std::vector<std::string> RemoveAll(ObjectId scope, ObjectId object, bool publishChanges = true);
		TagId Intern(std::string_view name);
		void ReleaseIfUnused(TagId id);
		[[nodiscard]] bool IsLiveInScope(ObjectId scope, ObjectId object) const;
		[[nodiscard]] TagId Find(std::string_view name) const;

		TagId NextId = 1;
		std::map<std::string, TagId, std::less<>> NameToId;
		std::map<TagId, std::string> IdToName;
		std::map<ObjectId, std::set<TagId>> ObjectToTags;
		std::map<TagId, std::set<ObjectId>> TagToObjects;
	};
}
