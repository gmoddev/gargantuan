// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "../../src/network/BlockGridPeer.hpp"
#include "RegionalGridFixture.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace gargantuan;
using namespace gargantuan::blockgrid;
using namespace gargantuan::network;
namespace {
	void Check(bool Value,const char* Label) { if (!Value) throw std::runtime_error(Label); }
	template<class Fn> void Reject(Fn Work) {
		try { Work(); } catch (const std::exception&) { return; }
		throw std::runtime_error("invalid page stream accepted");
	}
	struct Delivery { GridStreamReceiver::Result View;std::size_t Literal{},Wire{},Fragments{}; };
	Delivery Deliver(const GridReplicaContent& Data,const GridReplicaContent* Base,const GridReplicaContent* ReceiverBase,
		unsigned Serial,std::size_t* Reused=nullptr) {
		const auto Scope=GridStreamScope{77,{9,1},{10,1},6};
		const auto Source=MakeGridOffer(Scope,7,Serial,Serial,1,AssetContentId::Hash(*Data.Bytes),AssemblyProjection{},Data,Base);
		auto InvalidSource=Source;
		InvalidSource.Membership=0;
		Reject([&]{ (void)EncodeGridStream(InvalidSource); });
		const auto Packet=EncodeGridStream(Source);
		const auto Offer=DecodeGridStream(Packet);
		Check(Offer && Offer->Scope==Scope && Offer->Target==Data.Digest,"invalid scoped offer");
		GridStreamReceiver Receiver;
		Reject([&]{auto Wrong=Scope; ++Wrong.GrantGeneration;Receiver.Begin(*Offer,Wrong,ReceiverBase);});
		Receiver.Begin(*Offer,Scope,ReceiverBase);
		std::size_t Literal{},Fragments{},Wire=Packet.size(),Cursor=Offer->Prefix;
		for (;;) {
			const auto [Start,Length]=NextGridLiteral(*Offer,Cursor);
			if (!Length) break;
			const auto N=std::min(Length,MaximumGridStreamFragment);
			auto Part=*Offer;Part.Kind=GridStreamKind::Fragment;Part.Offset=static_cast<std::uint32_t>(Start);
			Part.Fragment=std::span(*Data.Bytes).subspan(Start,N);
			const auto Frame=EncodeGridStream(Part);const auto Decoded=DecodeGridStream(Frame);
			Check(Decoded.has_value(),"fragment decode failed");
			if (!Fragments) {
				auto Stale=*Decoded; ++Stale.Serial; Reject([&]{Receiver.Append(Stale);});
				Reject([&]{(void)Receiver.Finish();});
			}
			Receiver.Append(*Decoded);Literal+=N;Wire+=Frame.size();++Fragments;Cursor=Start+N;
		}
		Check(Receiver.Complete(),"partial window admitted");
		if (Reused) {
			*Reused=0;
			for (auto R:Offer->Reuse) *Reused+=R.Length;
		}
		return {Receiver.Finish(),Literal,Wire,Fragments};
	}
}
int main() try {
	auto Grid=test::LargePublicField();const auto Canonical=Encode(Grid,DataProfile::RelevantPeers);
	CFrame Pose;
	Pose.Position={400,50,-200};
	Pose.Rotation=glm::mat3(0,0,-1,0,1,0,1,0,0);
	auto Focus=[&](glm::vec3 Local){GridInterest I;I.Count=1;I.Centers[0]=Pose.Position+Pose.Rotation*(Local*2.f);return I;};
	const auto NearInterest=Focus(glm::vec3(12.5f));
	const auto Near=SelectGridCoverage(Grid,Pose,2,NearInterest);
	Check(Near.Origins.size()==27 && Near.Contains({{0,0,0},{24,24,24}}),"rotated two-unit interest failed");
	const auto First=*CaptureGridReplica(Grid,Near,{});
	const auto A=Deliver(First,nullptr,nullptr,10);
	Check(A.Fragments>=2 && A.Literal==First.Bytes->size() && A.View.Content.Coverage->Contains(SingleCell({12,12,12})) &&
		A.View.Field.GetValue({12,12,12},0)==17 && A.View.Field.Get({12,12,12}).Orientation==11,"large first install failed");
	const std::array LocalEdits{Write{SingleCell({0,0,0}),{Brick,23}},Write{SingleCell({23,23,23}),{Brick,22}}};
	Check(Grid.Apply(LocalEdits),"nonadjacent cells unchanged");
	const auto Edited=*CaptureGridReplica(Grid,Near,{});
	std::size_t Reused{};
	const auto B=Deliver(Edited,&First,&A.View.Content,11,&Reused);
	Check(Reused>35000 && B.Literal<4000 && B.View.Field.Get({0,0,0})==Block{Brick,23} &&
		B.View.Field.Get({23,23,23})==Block{Brick,22},"nonadjacent delta resent an unchanged middle");
	const auto Shifted=SelectGridCoverage(Grid,Pose,2,Focus({20.5f,12.5f,12.5f}));
	const auto Next=*CaptureGridReplica(Grid,Shifted,{});
	std::size_t Overlap{};
	const auto C=Deliver(Next,&Edited,&B.View.Content,12,&Overlap);
	Check(Overlap>24000 && C.Literal<16000 && C.View.Field.GetOccupiedCount()==27*512 &&
		!C.View.Content.Coverage->Contains(SingleCell({0,0,0})),"shifted view failed to reuse or retire pages");
	const auto Late=Deliver(Next,nullptr,nullptr,13);
	Check(Late.View.Content.Digest==C.View.Content.Digest && Late.View.Field.HasSameContents(C.View.Field),"late join diverged");
	GridInterest Four;
	Four.Count=4;
	Four.Centers={Focus({12.5f,12.5f,12.5f}).Centers[0],Focus({76.5f,12.5f,12.5f}).Centers[0],
		Focus({12.5f,52.5f,52.5f}).Centers[0],Focus({76.5f,52.5f,52.5f}).Centers[0]};
	Check(SelectGridCoverage(Grid,Pose,2,Four).Origins.size()==MaximumReplicaPages,"independent origins were combined incorrectly");
	const SpatialDestinationBounds MotionArea{{-5,25,-6},{7,39,7}};
	Four.ControlledMotion=MotionArea;
	const auto Controlled=SelectGridCoverage(Grid,Pose,2,Four);
	const auto SweptCells=GridLocalBounds(Pose,2,MotionArea);
	Check(SweptCells && Controlled.Contains(*SweptCells) && Controlled.Origins.size()<=MaximumReplicaPages,
		"independent focus origins displaced verified controller motion coverage");
	GridInterest Landing;Landing.Destination=SpatialDestinationBounds{glm::vec3(-400),glm::vec3(400)};
	const auto Exhausted=SelectGridCoverage(Grid,Pose,2,Landing);
	Check(!Exhausted.Whole && Exhausted.Origins.empty() && !Exhausted.Contains(SingleCell({12,12,12})),"oversized landing claimed coverage");
	Landing.Destination=SpatialDestinationBounds{NearInterest.Centers[0]-.1f,NearInterest.Centers[0]+.1f};
	const auto Exact=SelectGridCoverage(Grid,Pose,2,Landing);
	Check(Exact.Rectangle && Exact.Origins.empty() && Exact.Contains(SingleCell({12,12,12})),"exact staged destination failed");
	// Default capsule, legal half-unit blocks and a camera-independent chart rotation.
	const glm::mat3 Y{.9659258f,0,-.2588190f,0,1,0,.2588190f,0,.9659258f};
	const glm::mat3 X{1,0,0,0,.7071068f,.7071068f,0,-.7071068f,.7071068f};
	CFrame Tilt;Tilt.Rotation=Y*X;
	GridInterest Actor;Actor.ControlledMotion=SpatialDestinationBounds{{-3,-4,-3},{7,8,7}};
	Field Sparse;
	const Cell Distant{90000,0,0};
	const std::array<Write,2> Blocks{{{SingleCell({0,0,0}),{Stone,0}},{SingleCell(Distant),{Brick,5}}}};
	Check(Sparse.Apply(Blocks),"sparse fixture did not install");
	const auto Rect=SelectGridCoverage(Sparse,Tilt,.5f,Actor);
	Check(Rect.Rectangle && !Rect.Whole && Rect.Contains(*GridLocalBounds(Tilt,.5f,*Actor.ControlledMotion)) &&
		!Rect.Contains(SingleCell(Distant)),"sparse tilted chart failed to authorize exact controller bounds");
	auto Pages=Sparse.CapturePages(Rect).value();
	ReplicaCoverage Admitted;
	const auto Decoded=Field::DecodePages(Pages.Bytes,Admitted);
	Check(Pages.Bytes[4]==2 && Admitted==Rect && Pages.Records.size()==1 && Decoded.Get({0,0,0}).Definition==Stone &&
		Decoded.Get(Distant).Definition==Air,"compact rectangle disclosed distant occupied geometry");
	auto Broken=Pages.Bytes;
	Broken[6]|=1;
	Reject([&]{ (void)Field::DecodePages(Broken,Admitted); });
	Broken=Pages.Bytes; // max X, little endian: vastly more than 1,024 addressable pages
	Broken[18]=0;Broken[19]=0;Broken[20]=1;Broken[21]=0;
	Reject([&]{ (void)Field::DecodePages(Broken,Admitted); });
	Broken=Pages.Bytes;
	Broken[4]=3;
	Reject([&]{ (void)Field::DecodePages(Broken,Admitted); });
	Broken=Pages.Bytes;
	const auto XOffset=Pages.Records.front().Offset+2;
	for (unsigned I=0;I<4;++I) Broken[XOffset+I]=static_cast<std::uint8_t>(Distant.X>>(I*8));
	Reject([&]{ (void)Field::DecodePages(Broken,Admitted); });
	Field Dense;
	std::vector<Write> DenseWrites;
	const auto [Low,High]=*Rect.Rectangle;
	for(int Z=Low.Z;Z<High.Z;Z+=LeafSide) for(int V=Low.Y;V<High.Y;V+=LeafSide) for(int U=Low.X;U<High.X;U+=LeafSide)
		DenseWrites.push_back({SingleCell({U,V,Z}),{Stone,0}});
	Check(DenseWrites.size()==120 && Dense.Apply(DenseWrites) && !Dense.CapturePages(Rect),
		"occupied-page exhaustion must fail without certifying any partial rectangle");
	std::cout<<"[BlockGrid:RegionalWire] canonical="<<Canonical.size()<<" firstWire="<<A.Wire<<" firstFragments="<<A.Fragments
		<<" editWire="<<B.Wire<<" editLiterals="<<B.Literal<<" editPageReuse="<<Reused
		<<" shiftWire="<<C.Wire<<" shiftLiterals="<<C.Literal<<" shiftPageReuse="<<Overlap<<" lateWire="<<Late.Wire<<"\n";
} catch (const std::exception& E) { std::cerr<<"[BlockGrid:RegionalWire] "<<E.what()<<'\n';return 1; }
