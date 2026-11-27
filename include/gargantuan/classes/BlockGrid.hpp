// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "gargantuan/blockgrid/Field.hpp"
#include "gargantuan/blockgrid/Edit.hpp"
#include "gargantuan/blockgrid/Assembly.hpp"
#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/classes/generated/BlockGrid.hpp"
#include "gargantuan/runtime/SpatialDomainMember.hpp"

namespace gargantuan {
	class MutationGateway;
	class AssetService;
	class BlockGrid final : public Instance, public SpatialDomainMember {
		I_BlockGrid;
	  public:
		BlockGrid();
		[[nodiscard]] const blockgrid::Field& GetField() const;
		[[nodiscard]] const std::optional<blockgrid::CellDelta>& GetLastCellEdit() const { return LastCellEdit; }
		// Resolves decoded project pins after adoption. False means a required resource is unavailable.
		[[nodiscard]] bool EnsureAssetBindings() const;
		[[nodiscard]] blockgrid::Definition ResolveAssetDefinition(blockgrid::Definition Spec) const;
		[[nodiscard]] blockgrid::Definition ResolveMaterialAlias(std::string_view Key) const;
		[[nodiscard]] std::uint64_t GetAssetPublicationRevision() const noexcept { return AssetPublicationRevision; }
		[[nodiscard]] std::string GetPublicBlockRevision() const;
		[[nodiscard]] std::uint64_t GetConfigurationRevision() const noexcept { return ConfigurationRevision; }
		[[nodiscard]] bool IsDynamicAssembly() const noexcept { return DynamicAssembly; }
		[[nodiscard]] std::optional<blockgrid::AssemblyProjection> GetAssemblyProjection() const;
		bool ApplyReplicaAssemblyProjection(const blockgrid::AssemblyProjection& Value);
		void ClearReplicaAssemblyProjection();
		// Trusted asset materialization, before adoption or publication.
		void RestoreField(blockgrid::Field Value);
		// Used only after the authenticated content receiver resolves the live structural shell.
		void InstallReplicaField(blockgrid::Field Value);
		[[nodiscard]] bool HasReplicaField() const { return ReplicaFieldAvailable; }
		void SetAdmission(blockgrid::Field::Admission BeforeCommit);
		void PushGridHit(lua_State* L, blockgrid::Cell Cell, glm::vec3 Position, glm::vec3 Normal) const;
	  private:
		friend class MutationGateway;
		friend class WorldRoot;
		[[nodiscard]] std::string ApplyEdit(const blockgrid::EditRequest& Edit,const blockgrid::Patch* Prepared=nullptr);
		[[nodiscard]] std::shared_ptr<const blockgrid::Patch> PlanMaterialEdit(const blockgrid::EditRequest& Edit) const;
		[[nodiscard]] std::string RevisionToken(std::uint64_t SourceRevision) const;
		void CheckConfigurationWrite() const;
		void NotifyConfiguration(std::string_view Name);
		std::optional<blockgrid::CellDelta> LastCellEdit;
		void PublishAssemblyState(const blockgrid::AssemblyProjection& Value);
		void RetireAssemblyState();
		mutable blockgrid::Field Cells;
		blockgrid::Field::Admission Admission;
		gargantuan::CFrame Transform;
		float Size{1.0f};
		bool IsVisible{true}, Collides{true}, Queryable{true}, Editing{};
		bool ReplicaFieldAvailable{};
		bool DynamicAssembly{};
		std::uint64_t AssemblyRevision{1};
		std::unique_ptr<blockgrid::AssemblyProjection> AssemblyState;
		std::uint64_t ConfigurationRevision{1};
		mutable std::weak_ptr<AssetService> LastAssetService;
		mutable std::uint64_t AssetSequence{},AssetPublicationRevision{};
		mutable bool AssetsReady{};
	};
}
