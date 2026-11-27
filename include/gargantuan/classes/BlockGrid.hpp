// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "gargantuan/blockgrid/Field.hpp"
#include "gargantuan/blockgrid/Edit.hpp"
#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/classes/generated/BlockGrid.hpp"
#include "gargantuan/runtime/SpatialDomainMember.hpp"

namespace gargantuan {
	class MutationGateway;
	class BlockGrid final : public Instance, public SpatialDomainMember {
		I_BlockGrid;
	  public:
		BlockGrid();
		[[nodiscard]] const blockgrid::Field& GetField() const;
		[[nodiscard]] const std::optional<blockgrid::CellDelta>& GetLastCellEdit() const { return LastCellEdit; }
		[[nodiscard]] std::string GetPublicBlockRevision() const;
		// Trusted asset materialization, before adoption or publication.
		void RestoreField(blockgrid::Field Value);
		void SetAdmission(blockgrid::Field::Admission BeforeCommit);
		void PushGridHit(lua_State* L, blockgrid::Cell Cell, glm::vec3 Position, glm::vec3 Normal) const;
	  private:
		friend class MutationGateway;
		[[nodiscard]] std::string ApplyEdit(const blockgrid::EditRequest& Edit);
		[[nodiscard]] std::string RevisionToken(std::uint64_t SourceRevision) const;
		void CheckConfigurationWrite() const;
		void NotifyConfiguration(std::string_view Name);
		blockgrid::Field Cells;
		std::optional<blockgrid::CellDelta> LastCellEdit;
		blockgrid::Field::Admission Admission;
		gargantuan::CFrame Transform;
		float Size{1.0f};
		bool IsVisible{true}, Collides{true}, Queryable{true}, Editing{};
		std::uint64_t ConfigurationRevision{1};
	};
}
