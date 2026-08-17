#include "gargantuan/runtime/TagIndex.hpp"

#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace gargantuan {
	namespace {
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
		if (!IsValidProtocolUtf8(name)) throw std::invalid_argument("Tag name is not valid UTF-8");
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

	void TagIndex::AdoptDetached(
		ObjectId Scope,
		const std::vector<std::pair<ObjectId, std::vector<std::string>>> &Memberships
	) {
		AssertAuthoritativeMutation("Detached tag adoption");
		if (!Scope.IsValid()) throw std::invalid_argument("Detached tag adoption requires a valid DataModel scope");
		std::vector<std::pair<ObjectId, TagId>> Inserted;
		std::size_t MembershipCount = 0;
		for (const auto &[Object, Names] : Memberships) {
			if (!IsLiveInScope(Scope, Object))
				throw std::invalid_argument("Detached tag target is stale or belongs to another DataModel");
			for (const auto &Name : Names) ValidateTagName(Name);
			MembershipCount += Names.size();
		}
		Inserted.reserve(MembershipCount);
		try {
			for (const auto &[Object, Names] : Memberships) for (const auto &Name : Names) {
				const auto Id = Intern(Name);
				bool ReverseInserted = false;
				try {
					auto [Reverse, CreatedReverse] = TagToObjects.try_emplace(Id);
					(void)CreatedReverse;
					if (!Reverse->second.insert(Object).second)
						throw std::runtime_error("Detached tag reverse index is inconsistent");
					ReverseInserted = true;
					auto [Forward, CreatedForward] = ObjectToTags.try_emplace(Object);
					(void)CreatedForward;
					if (!Forward->second.insert(Id).second)
						throw std::runtime_error("Detached tag forward index is inconsistent");
					Inserted.emplace_back(Object, Id);
				} catch (...) {
					if (ReverseInserted) {
						auto Reverse = TagToObjects.find(Id);
						if (Reverse != TagToObjects.end()) Reverse->second.erase(Object);
					}
					auto Forward = ObjectToTags.find(Object);
					if (Forward != ObjectToTags.end() && Forward->second.empty()) ObjectToTags.erase(Forward);
					ReleaseIfUnused(Id);
					throw;
				}
			}
		} catch (...) {
			for (auto It = Inserted.rbegin(); It != Inserted.rend(); ++It) {
				auto Forward = ObjectToTags.find(It->first);
				if (Forward != ObjectToTags.end()) {
					Forward->second.erase(It->second);
					if (Forward->second.empty()) ObjectToTags.erase(Forward);
				}
				auto Reverse = TagToObjects.find(It->second);
				if (Reverse != TagToObjects.end()) Reverse->second.erase(It->first);
				ReleaseIfUnused(It->second);
			}
			throw;
		}
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
		auto dataModel = std::dynamic_pointer_cast<DataModel>(ObjectRegistry::Get().Lookup(scope));
		if (dataModel) dataModel->EnsureAuthoritativeRevisionAvailable();

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
		if (dataModel) dataModel->AdvanceAuthoritativeRevision();
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
		auto dataModel = std::dynamic_pointer_cast<DataModel>(ObjectRegistry::Get().Lookup(scope));
		if (dataModel) dataModel->EnsureAuthoritativeRevisionAvailable();
		const TagRemovedChange change{std::string(name)};
		auto reverse = TagToObjects.find(id);
		if (reverse == TagToObjects.end() || !reverse->second.contains(object))
			throw std::runtime_error("Tag indexes are inconsistent");
		ChangeJournal::Get().Commit(scope, object, change);
		forward->second.erase(id);
		reverse->second.erase(object);
		if (forward->second.empty()) ObjectToTags.erase(forward);
		ReleaseIfUnused(id);
		if (dataModel) dataModel->AdvanceAuthoritativeRevision();
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
