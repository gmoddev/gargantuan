// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "gargantuan/blockgrid/Edit.hpp"
#include "gargantuan/blockgrid/Transfer.hpp"
#include "gargantuan/datatypes/CFrame.hpp"
#include "gargantuan/runtime/MutationGateway.hpp"

namespace gargantuan {
	class BlockGrid;
	class WorldRoot;
	struct GridTransferConfiguration {
		CFrame Pose;
		float Size{};
		bool Visible{}, Collides{}, Queries{};
		ObjectId Assignment;
		bool operator==(const GridTransferConfiguration& Other) const {
			return Pose.Position==Other.Pose.Position && Pose.Rotation==Other.Pose.Rotation && Size==Other.Size &&
				Visible==Other.Visible && Collides==Other.Collides && Queries==Other.Queries && Assignment==Other.Assignment;
		}
	};
	struct GridTransferHistory {
		ObjectId Source,Destination,Parent,Domain;
		bool Extract{};
		GridTransferConfiguration SourceConfig,DestinationConfig;
		blockgrid::TransferFields Before,After;
		// Only an empty, detached undo shell is retained. Existing external references keep their identity.
		mutable std::shared_ptr<BlockGrid> Parked;
		~GridTransferHistory();
		[[nodiscard]] std::size_t Charge() const noexcept;
	};
	class PreparedGridTransfer {
	  public:
		static MutationResult Apply(const std::shared_ptr<BlockGrid>& Grid,const blockgrid::EditRequest& Request,
			const MutationAuthorityContext& Authority);
		static TransactionResult Replay(DataModel& World,const ScriptSecurityContext& Security,bool Redo);
	  private:
		enum class Hierarchy { Keep,Attach,Detach };
		static MutationResult Install(DataModel& World,const std::shared_ptr<WorldRoot>& Domain,const std::shared_ptr<Instance>& Parent,
			const std::shared_ptr<BlockGrid>& Source,const std::shared_ptr<BlockGrid>& Destination,blockgrid::TransferFields Next,
			Hierarchy Change,const std::shared_ptr<const GridTransferHistory>& Receipt,
			std::optional<AuthoritativeTransactionHistory> History,blockgrid::TransferMotion Motion=blockgrid::TransferMotion::Static);
	};
}
