// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "BlockGridBinding.hpp"
#include "gargantuan/runtime/MutationGateway.hpp"

namespace gargantuan {
	using namespace blockgrid::binding;
	int BlockGrid::ExtractRegion(lua_State* L,Instance* Object) {
		auto& Grid=ReadTarget(L,Object,true);
		auto Edit=std::make_shared<blockgrid::EditRequest>();
		Edit->ExpectedRevision=StringAt(L,4);
		Edit->Bilateral=blockgrid::GridTransferEdit{blockgrid::Region{ReadCell(L,2),ReadCell(L,3,true)}};
		MutationGateway Gateway;
		const auto Result=Gateway.Apply(SubmitBlockGridEditCommand{Grid.GetObjectId(),Grid.GetReplicationScopeId(),std::move(Edit)});
		if (!Result.Succeeded() || !Result.Object) throw std::runtime_error(Result.Message.empty()?"[BlockGrid:Transfer] Extraction failed":Result.Message);
		return StackValue<std::shared_ptr<Instance>>::Push(L,ObjectRegistry::Get().Lookup(*Result.Object));
	}
	int BlockGrid::MergeFrom(lua_State* L,Instance* Object) {
		auto& Grid=ReadTarget(L,Object,true);
		const auto Source=StackValue<std::shared_ptr<Instance>>::Is(L,2)?
			std::dynamic_pointer_cast<BlockGrid>(StackValue<std::shared_ptr<Instance>>::From(L,2)):nullptr;
		if (!Source) throw std::invalid_argument("[BlockGrid:Transfer] Expected a source BlockGrid");
		(void)ReadTarget(L,Source.get(),true);
		auto Edit=std::make_shared<blockgrid::EditRequest>();
		Edit->ExpectedRevision=StringAt(L,3);
		Edit->Bilateral=blockgrid::GridTransferEdit{{},Source->GetObjectId(),std::string(StringAt(L,4))};
		return SubmitRequest(L,Grid,std::move(Edit));
	}
}
