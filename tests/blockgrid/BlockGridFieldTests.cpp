// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/blockgrid/Field.hpp"
#include "gargantuan/blockgrid/Picking.hpp"
#include "gargantuan/blockgrid/Edit.hpp"
#include <algorithm>
#include <array>
#include <cmath>
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
	using DVec=std::array<double,3>;
	DVec Rotate(DVec P,std::uint8_t R,double Pivot) {
		DVec Out{Pivot,Pivot,Pivot};
		for (unsigned I=0;I<3;++I) for (unsigned J=0;J<3;++J) Out[I]+=Basis(R)[J][I]*(P[J]-Pivot);
		return Out;
	}
	void TestDefinitions() {
		Field F;
		const auto Empty=Encode(F);
		Definition Ramp{"project:ramp",Shape::Wedge,{.25f,.5f,.75f},true,true,9};
		Check(F.Define(Ramp) && !F.Define(Ramp) && F.GetDefinitionId(Ramp.Key)==3 && F.IsValid({3,23}));
		Reject([&]{ (void)F.GetDefinitionId("project:unknown"); });
		for (const auto Key:{"Stone","material:x","project:","project:A","project:0-a","project:bad/key"}) {
			auto Invalid=Ramp; Invalid.Key=Key; Reject([&]{ (void)F.Define(Invalid); });
		}
		auto Other=Ramp; Other.Color[1]=.2f;
		Reject([&]{ (void)F.Define(Other); });
		Check(DefinitionDigest(Ramp).size()==64 && DefinitionDigest(Other)!=DefinitionDigest(Ramp));
		const auto Inactive=F;
		Check(Fill(F,SingleCell({-1,1,1}),{3,23}) && Fill(F,SingleCell({8,0,0}),{3,1}));
		const auto Original=Encode(F);
		Check(Original[4]==2 && Encode(Decode(Original))==Original);
		auto Restored=Decode(Original);
		Reject([&]{ Restored.UndoDefinitionInsertion(Ramp); });
		Check(Restored.GetDefinition(3)==Ramp);
		unsigned Dirty=0;
		Reject([&]{ (void)Restored.Define(Other,true,[&](const Field::Candidate& Next,auto Writes) {
			Check(Writes.empty() && Next.GetDefinition(3)==Other && Next.GetOccupiedCount()==2);
			Next.ForEachChangedLeaf([&](Cell,const Leaf* L){ Check(L && L->GetOccupiedCount()==1); ++Dirty; });
			return false;
		}); });
		Check(Dirty==2 && Encode(Restored)==Original);
		auto Changed=Restored;
		Check(Fill(Changed,SingleCell({-1,1,1}),{}));
		Dirty=0;
		Check(Changed.Define(Other,true,[&](const auto& Next,auto) {
			Next.ForEachChangedLeaf([&](Cell P,const Leaf*){ Check(P==Cell{8,0,0}); ++Dirty; });
			return true;
		}) && Dirty==1 && Restored.GetDefinition(3)==Ramp);
		auto Undo=Inactive; Undo.UndoDefinitionInsertion(Ramp); Check(Encode(Undo)==Empty);
		auto Broken=Original; Broken[7]=255; Reject([&]{ (void)Decode(Broken); });
		Broken=Original; Broken[8]='x'; Reject([&]{ (void)Decode(Broken); });
		Broken=Original; Broken[8+Ramp.Key.size()]=255; Reject([&]{ (void)Decode(Broken); });
		Broken=Original; Broken[8+Ramp.Key.size()+14]=128; Reject([&]{ (void)Decode(Broken); });
		for (unsigned S=0;S<4;++S) for (std::uint8_t R=0;R<24;++R) {
			const auto Kind=static_cast<Shape>(S);
			const auto& G=GetGeometry(Kind,R);
			const auto [U,Front]=OrientationFaces(R);
			Check(OrientationIndex(U,Front)==R && G.Collision.size()==(Kind==Shape::Stairs?2:1));
			double Volume=0;
			for (const auto& Face:G.Faces) {
				Check(Face.Count==3 || Face.Count==4);
				for (unsigned J=1;J+1<Face.Count;++J) {
					const auto A=Face.Corners[0],B=Face.Corners[J],C=Face.Corners[J+1];
					Volume+=(A[0]*(B[1]*C[2]-B[2]*C[1])+A[1]*(B[2]*C[0]-B[0]*C[2])+A[2]*(B[0]*C[1]-B[1]*C[0]))/6.;
				}
			}
			Check(std::abs(Volume-(Kind==Shape::Cube?1.:Kind==Shape::Stairs?.75:.5))<1e-6);
			unsigned Mask=0;
			for (unsigned A=0;A<6;++A) if (Kind==Shape::Cube || A==3 || (Kind!=Shape::Slab && A==4))
				Mask|=1u<<RotateFace(A,R);
			Check(G.FullFaces==Mask);
			if (Kind==Shape::Wedge) {
				Check(G.Collision[0].Kind==PieceKind::Wedge && RotateFace(5,G.Collision[0].Orientation)==RotateFace(4,R));
			}
			Field ShapeField; auto Spec=Ramp; Spec.Geometry=Kind; Check(ShapeField.Define(Spec));
			Check(Fill(ShapeField,SingleCell({0,0,0}),{3,R}));
			for (double Z:{.25,.75}) {
				const auto Hit=Cast(ShapeField,Rotate({.25,2,Z},R,.5),Rotate({0,-2,0},R,0));
				const auto Top=Kind==Shape::Cube?1.:Kind==Shape::Slab?.5:Kind==Shape::Wedge?Z:Z<.5?.5:1.;
				Check(Hit && Hit->Position==Cell{} && std::abs(Hit->Fraction-(2.-Top)/2.)<1e-12);
				const auto N=Rotate(Kind==Shape::Wedge?DVec{0,std::sqrt(.5),-std::sqrt(.5)}:DVec{0,1,0},R,0);
				for (unsigned Axis=0;Axis<3;++Axis) Check(std::abs(Hit->Normal[Axis]-N[Axis])<1e-12);
			}
			if (Kind==Shape::Stairs) {
				const auto Interior=Cast(ShapeField,Rotate({.25,.25,.75},R,.5),Rotate({0,1,0},R,0));
				Check(Interior && Interior->Fraction==.75);
			}
			Spec.Queryable=false; Check(ShapeField.Define(Spec,true));
			Check(!Cast(ShapeField,Rotate({.25,2,.75},R,.5),Rotate({0,-2,0},R,0)));
		}
	}
	void TestState() {
		Field F;
		const auto G=F.GetGeometryRevision(), P=F.GetPublicRevision();
		const ChannelSpec Damage{"damage",ChannelKind::UInt8,0,true,false,true,false};
		const ChannelSpec Device{"device",ChannelKind::Bool,0,true,true,true,true};
		const ChannelSpec Heat{"heat",ChannelKind::Float32,ScalarBits(ChannelKind::Float32,2.5),false,true,false,true};
		Check(F.DefineChannel(Damage) && !F.DefineChannel(Damage) && F.GetGeometryRevision()==G && F.GetPublicRevision()==P);
		Check(F.DefineChannel(Device) && F.DefineChannel(Heat));
		Reject([&]{ (void)ScalarBits(ChannelKind::Bool,1.); });
		Reject([&]{ (void)ScalarBits(ChannelKind::UInt8,256.); });
		Reject([&]{ (void)ScalarBits(ChannelKind::Float32,std::numeric_limits<double>::infinity()); });
		Reject([&]{ (void)ScalarBits(ChannelKind::Int32,.25); });
		Check(ScalarBits(ChannelKind::Float32,-0.)==0 && std::get<double>(ScalarValue(ChannelKind::Int32,ScalarBits(ChannelKind::Int32,-3.)))==-3);
		const Cell A{-1,0,0},B{0,0,0},C{1,0,0};
		const std::vector<Operation> Batch{Write{SingleCell(A),{Stone,0}},ValueWrite{A,0,9},ValueWrite{A,1,1},
			ValueWrite{A,2,ScalarBits(ChannelKind::Float32,4.5)},Write{SingleCell(A),{Stone,2}}};
		Check(F.ApplyOperations(Batch) && F.GetValue(A,0)==9 && F.GetValue(A,1)==1 && F.GetOverrideCount()==3);
		const auto Machine=F;
		const auto PublicBefore=Encode(F,DataProfile::RelevantPeers);
		const auto PublicRev=F.GetPublicRevision(), Geo=F.GetGeometryRevision();
		const std::array<Operation,1> PrivateChange{ValueWrite{A,0,10}};
		bool ProviderCalled=false;
		Check(F.ApplyOperations(PrivateChange,[&](const auto&,auto){ProviderCalled=true;return false;}) && !ProviderCalled);
		Check(F.GetGeometryRevision()==Geo && F.GetPublicRevision()==PublicRev && PublicBefore==Encode(F,DataProfile::RelevantPeers));
		Check(Machine.GetValue(A,0)==9 && F.GetValue(A,0)==10);
		auto Saved=Decode(Encode(F)),Client=Decode(Encode(F,DataProfile::ClientPersistent)),Clone=Decode(Encode(F,DataProfile::Clone));
		Check(Saved.GetChannelCount()==2 && Saved.GetValue(A,Saved.GetChannelId("damage"))==10);
		Check(Client.GetChannelCount()==1 && Client.GetValue(A,Client.GetChannelId("device"))==1);
		Check(Clone.GetValue(A,0)==0 && Clone.GetValue(A,1)==1 && Clone.GetValue(A,2)==ScalarBits(ChannelKind::Float32,4.5));
		Check(Decode(PublicBefore).GetChannelCount()==2 && Encode(Saved)==Encode(F));
		const std::array<Operation,1> Copy{Transfer{SingleCell(A),B,false}};
		Check(F.ApplyOperations(Copy) && F.GetValue(B,0)==0 && F.GetValue(B,1)==1 && F.Get(B)==Block{Stone,2});
		const std::array<Operation,1> Move{Transfer{{A,{1,1,1}},B,true}};
		Check(F.ApplyOperations(Move) && !F.Get(A).Definition && F.GetValue(B,0)==10 && F.GetValue(C,0)==0);
		auto Prior=Encode(F,DataProfile::RelevantPeers);
		const auto R=F.GetRevision();
		const std::array<Operation,2> Invalid{Write{SingleCell(B),{}},ValueWrite{B,0,1}};
		Reject([&]{(void)F.ApplyOperations(Invalid);});
		Check(F.GetRevision()==R && Prior==Encode(F,DataProfile::RelevantPeers));
		const std::array<Operation,2> Reset{Write{SingleCell(B),{}},Write{SingleCell(B),{Stone,2}}};
		Check(F.ApplyOperations(Reset) && F.GetValue(B,0)==0 && F.GetValue(B,1)==0);
		Reject([&]{F.UndoChannelInsertion(Device);});
		auto Broken=Encode(Saved); Broken.back()=255; Reject([&]{(void)Decode(Broken);});
		Check(Fill(F,SingleCell(C),{Brick,0}) && F.GetValue(C,1)==0);
		Check(F.GetOverrideCount()==0);
		Reject([&]{(void)F.GetValue(A,0);});
	}
}
int main() try {
	TestDefinitions();
	TestState();
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
