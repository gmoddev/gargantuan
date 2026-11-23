// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#include "gargantuan/assets/GasDependencies.hpp"
#include "gargantuan/reflection/InstanceClassRegistry.hpp"
#include "gargantuan/services/AssetService.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace gargantuan::Gas {
	SerializationResult<std::vector<Dependency>> ResolveDependencies(
		const Document &DocumentValue, const AssetService *Assets, bool IncludeEntireCatalog) {
		try {
			std::set<AssetId> Seen;
			std::vector<AssetId> Pending;
			const auto Add = [&](AssetId Id) {
				if (!Id.IsValid()) throw std::invalid_argument("Invalid asset identity");
				if (!Seen.insert(Id).second) return;
				if (Seen.size() > MaximumDependencies) throw std::length_error("Too many dependencies");
				Pending.push_back(Id);
			};
			for (const auto &Entry : DocumentValue.Dependencies) Add(Entry.Id);
			if (IncludeEntireCatalog && Assets)
				for (const auto &Record : Assets->GetCatalog(false)) Add(Record.Id);

			const auto Inspect = [&](const Value &Property) {
				const auto *Wire = std::get_if<WireValue>(&Property);
				const auto *Text = Wire ? std::get_if<std::string>(Wire) : nullptr;
				if (!Text) throw std::invalid_argument("An asset-valued property must be text");
				if (Text->empty()) return;
				const auto Reference = AssetReference::Parse(*Text);
				if (!Reference) throw std::invalid_argument("An asset-valued property has an invalid reference");
				if (Reference->ProjectAsset) Add(*Reference->ProjectAsset);
			};
			for (const auto &Node : DocumentValue.Nodes) {
				const auto *Class = InstanceClassRegistry::GetDefinitionBySchemaId(Node.Class);
				if (!Class) throw std::invalid_argument("Asset references have an unknown owning class");
				for (const auto &[Name, Property] : Node.SavedProperties) {
					const auto Member = Class->AllProperties.find(Name);
					if (Member == Class->AllProperties.end()) continue;
					const auto &Definition = *Member->second;
					const bool HasHint = Definition.EditorHint && Definition.EditorHint->starts_with("AssetReference:");
					const bool Image = Name == "Image" &&
						Definition.DeclaringSchemaId == SchemaId::FromNativeName("Engine", "ImageLabel");
					const bool Font = Name == "FontFace" &&
						Definition.DeclaringSchemaId == SchemaId::FromNativeName("Engine", "TextLabel");
					if (HasHint || Image || Font) Inspect(Property);
				}
			}

			std::vector<Dependency> Dependencies;
			while (!Pending.empty()) {
				const auto Id = Pending.back();
				Pending.pop_back();
				const auto Reference = AssetReference::FromAssetId(Id);
				const auto Record = Assets ? Assets->GetAsset(Reference.Value) : std::nullopt;
				if (!Record || Record->Id != Id || Record->BuiltIn ||
					!Record->ContentId.IsValid() || !Assets->IsAvailable(Reference.Value))
					throw std::invalid_argument("An asset dependency is absent or unavailable in the project catalog");
				Dependencies.push_back({Id, Record->ContentId});
				for (const auto Dependency : Record->Dependencies) Add(Dependency);
			}
			std::ranges::sort(Dependencies, {}, &Dependency::Id);
			return Dependencies;
		} catch (const std::length_error &) {
			return SerializationFailure(SerializationErrorCode::LimitExceeded, "GAS asset dependency count exceeds its limit");
		} catch (const std::invalid_argument &Error) {
			return SerializationFailure(SerializationErrorCode::InvalidValue, Error.what(), "GAS assets");
		} catch (const std::exception &) {
			return SerializationFailure(SerializationErrorCode::InternalFailure, "Could not resolve GAS asset dependencies");
		}
	}
}
