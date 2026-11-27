// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/blockgrid/Field.hpp"
#include "gargantuan/blockgrid/Picking.hpp"
#include "gargantuan/blockgrid/Edit.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <random>
#include <stdexcept>

using namespace gargantuan::blockgrid;
namespace {
	void Check(bool Value) { if (!Value) throw std::runtime_error("[BlockGrid:FieldTest] Incorrect result"); }
	template<class F> void Reject(F Action) {
		try { Action(); } catch (const std::exception&) { return; }
		throw std::runtime_error("[BlockGrid:FieldTest] Invalid input was accepted");
	}
	bool Fill(Field& Grid, Region Bounds, Block B) {
		const Write W{Bounds,B}; return Grid.Apply(std::span(&W,1));
	}
}
int main() try {
	Field Grid;
	Check(!Grid.GetOccupiedCount() && !Grid.GetLeafCount() && Grid.Get({-1,-1,-1}) == Block{});
	Check(Fill(Grid,{{-8,-8,-8},{0,0,0}},{Stone,0}) && Grid.GetOccupiedCount() == LeafVolume);
	unsigned Uniform = 0;
	Grid.ForEachLeaf([&](Cell Base, const Leaf& L){ Check(Base==Cell{-8,-8,-8} && L.GetUniform()==Block{Stone,0}); ++Uniform; });
	Check(Uniform == 1);
	const auto Original = Encode(Grid);
	Check(Original.size() == 25);
	const auto Snapshot = Grid;
	const auto Revision = Grid.GetRevision();
	Check(Fill(Grid,SingleCell({-3,-5,-4}),{Brick,23}));
	Check(Snapshot.Get({-3,-5,-4}) == Block{Stone,0} && Decode(Encode(Grid)).Get({-3,-5,-4}) == Block{Brick,23});
	const std::array UndoPair{Write{SingleCell({-1,-1,-1}),{}},Write{SingleCell({-1,-1,-1}),{Stone,0}}};
	Check(!Grid.Apply(UndoPair) && Grid.GetRevision() == Revision+1);
	Reject([&]{ (void)Grid.Apply(UndoPair,Revision); });
	const auto Prior = Encode(Grid);
	std::array Bad{Write{SingleCell({0,0,0}),{Brick,0}},Write{SingleCell({1,0,0}),{65535,0}}};
	Reject([&]{ (void)Grid.Apply(Bad); });
	const Write Capacity{SingleCell({0,0,0}),{Stone,1}};
	Reject([&]{ (void)Grid.Apply(std::span(&Capacity,1),{},[](const Field::Candidate& Next,auto){
		Check(Next.Get({0,0,0})==Block{Stone,1} && Next.GetOccupiedCount()==LeafVolume+1);
		unsigned Changed=0, Pages=0;
		Next.ForEachChangedLeaf([&](Cell P,const Leaf* L){ Check(P==Cell{} && L && L->GetOccupiedCount()==1); ++Changed; });
		Next.ForEachLeaf([&](Cell,const Leaf&){ ++Pages; });
		Check(Changed==1 && Pages==2);
		return false;
	}); });
	Check(Prior==Encode(Grid) && Grid.GetRevision()==Revision+1);
	Reject([&]{ (void)Fill(Grid,{{-CoordinateLimit,0,0},{CoordinateLimit,CoordinateLimit,CoordinateLimit}},{Stone,0}); });
	Check(Fill(Grid,{{-8,-8,-8},{0,0,0}},{}) && Grid.GetLeafCount()==0);
	std::mt19937 Random(218735);
	std::map<Cell,Block,CellOrder> Oracle;
	for (unsigned I=0; I<1000; ++I) {
		const Cell Pos{static_cast<int>(Random()%23)-11, static_cast<int>(Random()%5)-2, static_cast<int>(Random()%11)-5};
		const auto Kind = static_cast<std::uint16_t>(Random()%3);
		const Block B{Kind,static_cast<std::uint8_t>(Kind ? Random()%24 : 0)};
		(void)Fill(Grid,SingleCell(Pos),B);
		if (Kind) Oracle[Pos]=B; else Oracle.erase(Pos);
	}
	auto Bytes = Encode(Grid);
	auto Restored = Decode(Bytes);
	Check(Encode(Restored)==Bytes && Restored.GetOccupiedCount()==Oracle.size());
	std::map<Cell,Block,CellOrder> Observed;
	Restored.ForEachOccupied([&](Cell Pos,Block B){ Check(Observed.emplace(Pos,B).second); });
	Check(Observed==Oracle);
	for (std::size_t Cut : {std::size_t{0},std::size_t{8},Bytes.size()-1})
		Reject([&]{ (void)Decode(std::span(Bytes).first(Cut)); });
	auto Corrupt = Original;
	Corrupt[4]=9; Reject([&]{ (void)Decode(Corrupt); });
	Corrupt=Original; Corrupt[24]=24; Reject([&]{ (void)Decode(Corrupt); });
	Corrupt=Original; Corrupt[9]=255; Reject([&]{ (void)Decode(Corrupt); });
	Corrupt=Original; Corrupt.push_back(0); Reject([&]{ (void)Decode(Corrupt); });
	Corrupt=Original; Corrupt.insert(Corrupt.end(),Original.begin()+9,Original.end()); Corrupt[5]=2;
	Reject([&]{ (void)Decode(Corrupt); });
	Field Pick;
	Check(Fill(Pick,SingleCell({-2,0,0}),{Stone,0}));
	auto Face=Cast(Pick,{-1,.25,.25},{-2,0,0});
	Check(Face && Face->Position==Cell{-2,0,0} && Face->Fraction==0 && Face->Normal==std::array<double,3>{1,0,0});
	auto Inside=Cast(Pick,{-1.75,.25,.25},{2,0,0});
	Check(Inside && Inside->Fraction==.375 && Inside->Normal==std::array<double,3>{1,0,0});
	auto Corner=Cast(Pick,{-3,-1,-1},{2,2,2});
	Check(Corner && Corner->Fraction==.5 && Corner->Normal==std::array<double,3>{-1,0,0});
	Check(!Cast(Pick,{-3,1,.5},{2,0,0}));
	Reject([&]{ (void)Cast(Pick,{.5,3,.5},{32,0,0},2); });
	Check(Fill(Pick,SingleCell({-3,0,0}),{Brick,2}));
	auto Tie=Cast(Pick,{-1.5,.5,.5},{-2,0,0});
	Check(Tie && Tie->Position==Cell{-3,0,0} && Tie->Fraction==.25 && Tie->Normal==std::array<double,3>{1,0,0});
	Check(Fill(Pick,SingleCell({CoordinateLimit-1,0,0}),{Stone,0}));
	Check(Cast(Pick,{CoordinateLimit+2.,.5,.5},{-4,0,0})->Position==Cell{CoordinateLimit-1,0,0});
	Field Recorded;
	const Write Whole{{{-16,0,-16},{16,2,16}},{Brick,3}};
	const auto Delta=CapturePatch(Recorded,std::span(&Whole,1));
	Check(Delta->Cells.size()==2048 && Recorded.Apply(std::span(&Whole,1)));
	const auto Saved=Encode(Recorded);
	const auto Undo=ReplayWrites(Recorded,*Delta,false);
	Check(Recorded.ApplyCellPatch(Undo) && !Recorded.GetOccupiedCount());
	const auto Redo=ReplayWrites(Recorded,*Delta,true);
	Check(Recorded.ApplyCellPatch(Redo) && Encode(Recorded)==Saved);
	Check(Fill(Recorded,SingleCell({0,0,0}),{Stone,0}));
	const auto Diverged=Encode(Recorded);
	Reject([&]{ (void)ReplayWrites(Recorded,*Delta,false); });
	Check(Encode(Recorded)==Diverged);
	std::cout << "[BlockGrid:FieldTest] Storage, admission, codec and oracle passed\n";
} catch (const std::exception& Error) { std::cerr<<Error.what()<<'\n'; return 1; }
