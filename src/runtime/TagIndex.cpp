#include "gargantuan/runtime/TagIndex.hpp"

#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace gargantuan {
	namespace {
		bool IsValidUtf8(std::string_view value) {
			for (std::size_t index = 0; index < value.size();) {
				const auto first = static_cast<unsigned char>(value[index]);
				std::size_t count = 0;
				std::uint32_t codePoint = 0;
				if (first <= 0x7f) { count = 1; codePoint = first; }
				else if ((first & 0xe0) == 0xc0) { count = 2; codePoint = first & 0x1f; }
				else if ((first & 0xf0) == 0xe0) { count = 3; codePoint = first & 0x0f; }
				else if ((first & 0xf8) == 0xf0) { count = 4; codePoint = first & 0x07; }
				else return false;
				if (index + count > value.size()) return false;
				for (std::size_t offset = 1; offset < count; ++offset) {
					const auto continuation = static_cast<unsigned char>(value[index + offset]);
					if ((continuation & 0xc0) != 0x80) return false;
					codePoint = (codePoint << 6) | (continuation & 0x3f);
				}
				if ((count == 2 && codePoint < 0x80) || (count == 3 && codePoint < 0x800) ||
					(count == 4 && codePoint < 0x10000) || codePoint > 0x10ffff ||
					(codePoint >= 0xd800 && codePoint <= 0xdfff)) return false;
				index += count;
			}
			return true;
		}

		void DemandRead(const ScriptSecurityContext &securityContext) {
			if (!securityContext.HasCapability(ScriptCapability::ReadDataModel))
				throw std::runtime_error("Tag query requires ReadDataModel");
		}

		void DemandMutation(const ScriptSecurityContext &securityContext) {
			AssertAuthoritativeMutation("Tag mutation");
			if (!securityContext.HasCapability(ScriptCapability::MutateDataModel))
				throw std::runtime_error("Tag mutation requires MutateDataModel");
		}
	}

	void ValidateTagName(std::string_view name) {
		if (name.empty()) throw std::invalid_argument("Tag name cannot be empty");
		if (name.size() > MaximumTagNameBytes) throw std::invalid_argument("Tag name exceeds its byte limit");
		if (name.find('\0') != std::string_view::npos) throw std::invalid_argument("Tag name contains an embedded null");
		if (!IsValidUtf8(name)) throw std::invalid_argument("Tag name is not valid UTF-8");
	}

	TagId TagIndex::Find(std::string_view name) const {
		auto found = NameToId.find(name);
		return found == NameToId.end() ? InvalidTagId : found->second;
	}

	TagId TagIndex::Intern(std::string_view name) {
		if (const auto existing = Find(name); existing != InvalidTagId) return existing;
		if (NameToId.size() >= MaximumDistinctTagsPerDataModel)
			throw std::invalid_argument("DataModel exceeds its distinct tag limit");
		if (NextId == InvalidTagId || NextId == std::numeric_limits<TagId>::max())
			throw std::overflow_error("Tag identifier space is exhausted");
		const auto id = NextId++;
		const auto canonical = std::string(name);
		NameToId.emplace(canonical, id);
		try { IdToName.emplace(id, canonical); }
		catch (...) { NameToId.erase(canonical); throw; }
		return id;
	}

	void TagIndex::ReleaseIfUnused(TagId id) {
		auto reverse = TagToObjects.find(id);
		if (reverse != TagToObjects.end() && !reverse->second.empty()) return;
		TagToObjects.erase(id);
		auto name = IdToName.find(id);
		if (name == IdToName.end()) return;
		NameToId.erase(name->second);
		IdToName.erase(name);
	}

	bool TagIndex::IsLiveInScope(ObjectId scope, ObjectId object) const {
		auto instance = ObjectRegistry::Get().Lookup(object);
		return instance && !instance->GetDestroyed() && instance->GetReplicationScopeId() == scope;
	}

	bool TagIndex::Add(
		ObjectId scope,
		ObjectId object,
		std::string_view name,
		const ScriptSecurityContext &securityContext
	) {
		DemandMutation(securityContext);
		ValidateTagName(name);
		if (!scope.IsValid() || !IsLiveInScope(scope, object)) throw std::invalid_argument("Tag target is stale or belongs to another DataModel");
		auto forward = ObjectToTags.find(object);
		const auto existingId = Find(name);
		if (existingId != InvalidTagId && forward != ObjectToTags.end() && forward->second.contains(existingId)) return false;
		if (forward != ObjectToTags.end() && forward->second.size() >= MaximumTagsPerInstance)
			throw std::invalid_argument("Instance exceeds its tag count limit");

		const TagAddedChange change{std::string(name)};
		const auto id = Intern(name);
		bool reverseMembershipInserted = false;
		bool forwardMapInserted = false;
		bool forwardMembershipInserted = false;
		try {
			auto [reverseMapPosition, reverseMapInserted] = TagToObjects.try_emplace(id);
			(void)reverseMapInserted;
			if (!reverseMapPosition->second.insert(object).second)
				throw std::runtime_error("Tag reverse index is inconsistent");
			reverseMembershipInserted = true;
			auto [objectPosition, objectInserted] = ObjectToTags.try_emplace(object);
			forwardMapInserted = objectInserted;
			if (!objectPosition->second.insert(id).second) throw std::runtime_error("Tag forward index is inconsistent");
			forwardMembershipInserted = true;
			ChangeJournal::Get().Commit(scope, object, change);
		} catch (...) {
			if (forwardMembershipInserted) {
				auto objectPosition = ObjectToTags.find(object);
				if (objectPosition != ObjectToTags.end()) {
					objectPosition->second.erase(id);
					if (objectPosition->second.empty()) ObjectToTags.erase(objectPosition);
				}
			} else if (forwardMapInserted) {
				ObjectToTags.erase(object);
			}
			if (reverseMembershipInserted) {
				auto reversePosition = TagToObjects.find(id);
				if (reversePosition != TagToObjects.end()) reversePosition->second.erase(object);
			}
			ReleaseIfUnused(id);
			throw;
		}
		return true;
	}

	bool TagIndex::Remove(
		ObjectId scope,
		ObjectId object,
		std::string_view name,
		const ScriptSecurityContext &securityContext
	) {
		DemandMutation(securityContext);
		ValidateTagName(name);
		if (!scope.IsValid() || !IsLiveInScope(scope, object)) throw std::invalid_argument("Tag target is stale or belongs to another DataModel");
		const auto id = Find(name);
		auto forward = ObjectToTags.find(object);
		if (id == InvalidTagId || forward == ObjectToTags.end() || !forward->second.contains(id)) return false;
		const TagRemovedChange change{std::string(name)};
		auto reverse = TagToObjects.find(id);
		if (reverse == TagToObjects.end() || !reverse->second.contains(object))
			throw std::runtime_error("Tag indexes are inconsistent");
		ChangeJournal::Get().Commit(scope, object, change);
		forward->second.erase(id);
		reverse->second.erase(object);
		if (forward->second.empty()) ObjectToTags.erase(forward);
		ReleaseIfUnused(id);
		return true;
	}

	std::vector<std::string> TagIndex::RemoveAll(ObjectId scope, ObjectId object, bool publishChanges) {
		AssertAuthoritativeMutation("Tag lifecycle cleanup");
		std::vector<std::string> removed;
		auto forward = ObjectToTags.find(object);
		if (forward == ObjectToTags.end()) return removed;
		for (const auto id : forward->second) removed.push_back(IdToName.at(id));
		std::sort(removed.begin(), removed.end());
		for (const auto id : forward->second) {
			auto reverse = TagToObjects.find(id);
			if (reverse == TagToObjects.end() || !reverse->second.contains(object))
				throw std::runtime_error("Tag indexes are inconsistent during lifecycle cleanup");
		}
		const auto ids = forward->second;
		if (publishChanges) {
			std::vector<std::pair<ObjectId, ChangePayload>> changes;
			changes.reserve(removed.size());
			for (const auto &name : removed) changes.emplace_back(object, TagRemovedChange{name});
			ChangeJournal::Get().CommitBatch(scope, std::move(changes));
		}
		for (const auto id : ids) TagToObjects.at(id).erase(object);
		ObjectToTags.erase(forward);
		for (const auto id : ids) ReleaseIfUnused(id);
		return removed;
	}

	bool TagIndex::Has(ObjectId scope, ObjectId object, std::string_view name, const ScriptSecurityContext &securityContext) const {
		DemandRead(securityContext);
		ValidateTagName(name);
		if (!IsLiveInScope(scope, object)) return false;
		const auto id = Find(name);
		auto found = ObjectToTags.find(object);
		return id != InvalidTagId && found != ObjectToTags.end() && found->second.contains(id);
	}

	std::vector<std::string> TagIndex::GetTags(ObjectId scope, ObjectId object, const ScriptSecurityContext &securityContext) const {
		DemandRead(securityContext);
		if (!IsLiveInScope(scope, object)) return {};
		std::vector<std::string> result;
		if (auto found = ObjectToTags.find(object); found != ObjectToTags.end()) {
			for (const auto id : found->second) result.push_back(IdToName.at(id));
			std::sort(result.begin(), result.end());
		}
		return result;
	}

	std::vector<ObjectId> TagIndex::GetTagged(ObjectId scope, std::string_view name, const ScriptSecurityContext &securityContext) const {
		DemandRead(securityContext);
		ValidateTagName(name);
		std::vector<ObjectId> result;
		const auto id = Find(name);
		if (auto found = TagToObjects.find(id); id != InvalidTagId && found != TagToObjects.end())
			for (const auto object : found->second) if (IsLiveInScope(scope, object)) result.push_back(object);
		return result;
	}

	std::vector<ObjectId> TagIndex::GetTaggedAll(
		ObjectId scope,
		const std::vector<std::string> &names,
		const ScriptSecurityContext &securityContext
	) const {
		DemandRead(securityContext);
		if (names.empty()) return {};
		if (names.size() > MaximumTagsPerQuery) throw std::invalid_argument("Tag query exceeds its name count limit");
		std::vector<TagId> ids;
		ids.reserve(names.size());
		for (const auto &name : names) {
			ValidateTagName(name);
			const auto id = Find(name);
			if (id == InvalidTagId) return {};
			ids.push_back(id);
		}
		std::sort(ids.begin(), ids.end());
		ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
		auto smallest = std::min_element(ids.begin(), ids.end(), [this](TagId left, TagId right) {
			return TagToObjects.at(left).size() < TagToObjects.at(right).size();
		});
		std::vector<ObjectId> result;
		for (const auto object : TagToObjects.at(*smallest)) {
			if (!IsLiveInScope(scope, object)) continue;
			if (std::all_of(ids.begin(), ids.end(), [this, object](TagId id) { return TagToObjects.at(id).contains(object); }))
				result.push_back(object);
		}
		return result;
	}
}
