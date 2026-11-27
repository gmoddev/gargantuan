// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/blockgrid/Edit.hpp"
#include <stdexcept>

namespace gargantuan::blockgrid {
	std::shared_ptr<const Patch> CapturePatch(const Field& Source, std::span<const Write> Writes) {
		if (Writes.size()>MaximumCommands) throw std::length_error("[BlockGrid:History] Command limit exceeded");
		if (Source.GetChannelCount()) {
			auto Captured=std::make_shared<Patch>();
			const std::vector<Operation> Ops(Writes.begin(),Writes.end());
			Captured->StateCells=Source.PlanOperations(Ops);
			return Captured;
		}
		std::size_t Visits=0;
		for (const auto& W : Writes) {
			Visits+=Volume(W.Bounds,MaximumEditVisits-Visits);
			if (!Source.IsValid(W.Value)) throw std::invalid_argument("[BlockGrid:History] Invalid definition or orientation");
		}
		std::map<Cell,Block,CellOrder> Desired;
		for (const auto& W : Writes)
			for (int Z=W.Bounds.Min.Z; Z<W.Bounds.MaxExclusive.Z; ++Z)
				for (int Y=W.Bounds.Min.Y; Y<W.Bounds.MaxExclusive.Y; ++Y)
					for (int X=W.Bounds.Min.X; X<W.Bounds.MaxExclusive.X; ++X) Desired[{X,Y,Z}]=W.Value;
		auto Result=std::make_shared<Patch>();
		for (const auto& [P,After] : Desired) {
			const auto Before=Source.Get(P);
			if (Before!=After) Result->Cells.push_back({P,Before,After});
		}
		return Result;
	}
	std::vector<Write> ReplayWrites(const Field& Source,const Patch& P,bool Forward) {
		if (P.Cells.size()>MaximumEditVisits) throw std::length_error("[BlockGrid:History] Cell history exceeds visit limit");
		std::vector<Write> Result;
		Result.reserve(P.Cells.size());
		std::optional<Cell> Previous;
		for (const auto& D : P.Cells) {
			if ((Previous && !CellOrder{}(*Previous,D.Position)) || !Source.IsValid(D.Before) || !Source.IsValid(D.After) || D.Before==D.After)
				throw std::invalid_argument("[BlockGrid:History] Invalid cell history");
			Previous=D.Position;
			if (Source.Get(D.Position)!=(Forward?D.Before:D.After))
				throw std::domain_error("[BlockGrid:Conflict] A cell touched by history has diverged");
			for (std::uint8_t I=0;I<Source.GetChannelCount();++I)
				if (Source.GetChannel(I).Read(D.Position)!=Source.GetChannel(I).Spec.Default)
					throw std::domain_error("[BlockGrid:Conflict] A later state override occupies a history cell");
			Result.push_back({SingleCell(D.Position),Forward?D.After:D.Before});
		}
		return Result;
	}
}
