// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "BlockGridBinding.hpp"
#include "gargantuan/blockgrid/RuntimeHandles.hpp"
#include "gargantuan/classes/WorldRoot.hpp"
#include "gargantuan/datatypes/Vector3.hpp"
#include "gargantuan/scripting/NativeCallback.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <new>
#include <stdexcept>
#include <tuple>

namespace gargantuan::blockgrid::binding {
	namespace {
		using Clock=std::chrono::steady_clock;
		constexpr std::size_t MiB=1024*1024,GlobalPinLimit=128*MiB,VmPinLimit=64*MiB;
		std::atomic_size_t PinnedBytes{};
		constexpr char EditName[]="Gargantuan.BlockGridEdit",CursorName[]="Gargantuan.BlockGridTraversal";
		void Table(lua_State* L,int I) {
			if (!lua_istable(L,I)) throw std::invalid_argument("[BlockGrid:Argument] Expected a table");
		}
		void Fields(lua_State* L,int Index,std::initializer_list<std::string_view> Names) {
			const auto I=lua_absindex(L,Index); Table(L,I);
			lua_pushnil(L);
			while (lua_next(L,I)) {
				if (std::ranges::find(Names,StringAt(L,-2))==Names.end()) throw std::invalid_argument("[BlockGrid:Argument] Unexpected field");
				lua_pop(L,1);
			}
		}
		unsigned Integer(lua_State* L,int I,unsigned Min,unsigned Max) {
			if (lua_type(L,I)!=LUA_TNUMBER) throw std::invalid_argument("[BlockGrid:Argument] Expected an integer");
			const double N=lua_tonumber(L,I);
			if (!std::isfinite(N) || N<Min || N>Max || std::trunc(N)!=N) throw std::invalid_argument("[BlockGrid:Argument] Integer outside admitted range");
			return static_cast<unsigned>(N);
		}
		unsigned Array(lua_State* L,int Index,unsigned Max) {
			const auto I=lua_absindex(L,Index); Table(L,I);
			const auto N=static_cast<unsigned>(lua_objlen(L,I));
			if (N>Max) throw std::length_error("[BlockGrid:Limit] Array exceeds capacity");
			unsigned Found=0;
			lua_pushnil(L);
			while (lua_next(L,I)) {
				if (++Found>N) throw std::invalid_argument("[BlockGrid:Argument] Expected a dense array");
				(void)Integer(L,-2,1,N); lua_pop(L,1);
			}
			if (Found!=N) throw std::invalid_argument("[BlockGrid:Argument] Expected a dense array");
			return N;
		}
		bool Boolean(lua_State* L,int I) {
			if (lua_type(L,I)!=LUA_TBOOLEAN) throw std::invalid_argument("[BlockGrid:Argument] Expected a boolean");
			return lua_toboolean(L,I)!=0;
		}
		std::uint32_t Bits(lua_State* L,int I,ChannelKind Kind) {
			if (lua_type(L,I)==LUA_TBOOLEAN) return ScalarBits(Kind,Boolean(L,I));
			if (lua_type(L,I)==LUA_TNUMBER) return ScalarBits(Kind,lua_tonumber(L,I));
			throw std::invalid_argument("[BlockGrid:Value] Expected a number or boolean");
		}
		bool Choice(lua_State* L,int I,std::string_view First,std::string_view Second) {
			const auto V=StringAt(L,I);
			if (V==First) return false;
			if (V==Second) return true;
			throw std::invalid_argument("[BlockGrid:Argument] Unsupported option");
		}
		void Vector(lua_State* L,Cell P) { StackValue<glm::vec3>::Push(L,glm::vec3(P.X,P.Y,P.Z)); }
		Region Bounds(lua_State* L,int Index) {
			const auto I=lua_absindex(L,Index);
			Fields(L,I,{"Min","MaxExclusive"});
			lua_rawgetfield(L,I,"Min"); const auto Lo=ReadCell(L,-1); lua_pop(L,1);
			lua_rawgetfield(L,I,"MaxExclusive");const auto Hi=ReadCell(L,-1,true);lua_pop(L,1);
			return {Lo,Hi};
		}
		std::vector<std::uint16_t> Ids(lua_State* L,int Index,const Field& Source) {
			const auto I=lua_absindex(L,Index); const auto N=Array(L,I,64);
			std::vector<std::uint16_t> Result;Result.reserve(N);
			for (unsigned J=1;J<=N;++J) { lua_rawgeti(L,I,J);Result.push_back(Source.GetDefinitionId(StringAt(L,-1)));lua_pop(L,1); }
			return Result;
		}
	}
	Filter ReadFilter(lua_State* L,int Index,const Field& Source) {
		Filter F;
		if (lua_isnoneornil(L,Index)) return F;
		const auto I=lua_absindex(L,Index);
		Fields(L,I,{"Definitions","Where"});
		lua_rawgetfield(L,I,"Definitions");
		if (!lua_isnil(L,-1)) F.Definitions=Ids(L,-1,Source);
		lua_pop(L,1);
		lua_rawgetfield(L,I,"Where");
		if (!lua_isnil(L,-1)) {
			const auto W=lua_gettop(L);
			Fields(L,W,{"Channel","Equals"});
			lua_rawgetfield(L,W,"Channel");const auto Id=Source.GetChannelId(StringAt(L,-1));lua_pop(L,1);
			lua_rawgetfield(L,W,"Equals");F.Where=ValueTest{Id,Bits(L,-1,Source.GetChannel(Id).Spec.Kind)};lua_pop(L,1);
		}
		lua_pop(L,1);
		ValidateFilter(Source,F);
		return F;
	}
	struct Handle {
		std::weak_ptr<BlockGrid> Owner;
		ObjectId World,Domain;
		std::uint64_t Config{};
		Clock::time_point Born{Clock::now()},Last{Born};
		PageStatus Status{PageStatus::More};
		std::string Token;
		bool IsEdit{};
		std::size_t Charge{},Visits{};
		std::shared_ptr<EditRequest> Edit;
		std::unique_ptr<Field> Shadow;
		std::unique_ptr<Traversal> Scan;
		~Handle(){Close(PageStatus::Cancelled);}
		void Close(PageStatus Reason) {
			if (Status!=PageStatus::More) return;
			Status=Reason; Scan.reset(); Shadow.reset(); Edit.reset();
			if (Charge) { PinnedBytes.fetch_sub(Charge,std::memory_order_relaxed); Charge=0; }
		}
		void Inspect(Clock::time_point Now) {
			if (Status!=PageStatus::More) return;
			const auto Grid=Owner.lock();
			if (!Grid || Grid->GetDestroyed() || Grid->IsDestroying() || Grid->GetReplicationScopeId()!=World ||
				Grid->GetConfigurationRevision()!=Config) {Close(PageStatus::Expired);return;}
			const auto W=WorldRoot::GetEffectiveWorld(*Grid);
			if ((W?W->GetObjectId():ObjectId{})!=Domain || Now-Born>=std::chrono::seconds(30) || Now-Last>=std::chrono::seconds(5))
				Close(PageStatus::Expired);
		}
	};
	void HandleSet::Prune() {
		const auto Now=Clock::now();
		std::erase_if(Handles,[&](const auto& Weak) {
			const auto H=Weak.lock();
			if (!H) return true;
			H->Inspect(Now);
			return H->Status!=PageStatus::More;
		});
	}
	HandleSet::~HandleSet(){for (const auto& W:Handles) if (auto H=W.lock()) H->Close(PageStatus::Cancelled);}
	namespace {
		void Charge(Handle& H,const HandleSet& Set,std::size_t Bytes) {
			if (Bytes<=H.Charge) return;
			std::size_t VmTotal=0;
			for (const auto& W:Set.Handles) if (auto Other=W.lock();Other && Other.get()!=&H) VmTotal+=Other->Charge;
			if (Bytes>VmPinLimit || VmTotal>VmPinLimit-Bytes) throw std::length_error("[BlockGrid:Limit] VM pin capacity exhausted");
			const auto Extra=Bytes-H.Charge;
			auto Prior=PinnedBytes.load(std::memory_order_relaxed);
			do { if (Extra>GlobalPinLimit-Prior) throw std::length_error("[BlockGrid:Limit] Process pin capacity exhausted"); }
			while (!PinnedBytes.compare_exchange_weak(Prior,Prior+Extra,std::memory_order_relaxed));
			H.Charge=Bytes;
		}
		std::shared_ptr<Handle> Open(lua_State* L,BlockGrid& Grid,bool Edit,std::size_t ChargeBytes) {
			auto& Set=ScriptEngine::Get(L)->BlockGridHandles;
			if (!Set) Set=std::make_unique<HandleSet>();
			Set->Prune();
			unsigned Same=0;
			for (const auto& W:Set->Handles) if (auto Other=W.lock();Other && Other->IsEdit==Edit && Other->Owner.lock().get()==&Grid) ++Same;
			if (Same>=4 || Set->Handles.size()>=64) throw std::length_error("[BlockGrid:Limit] Active handle capacity exhausted");
			auto H=std::make_shared<Handle>(); H->IsEdit=Edit;
			H->Owner=std::static_pointer_cast<BlockGrid>(Grid.shared_from_this()); H->World=Grid.GetReplicationScopeId();
			const auto Domain=WorldRoot::GetEffectiveWorld(Grid); H->Domain=Domain?Domain->GetObjectId():ObjectId{};
			H->Config=Grid.GetConfigurationRevision(); H->Token=Grid.GetBlockRevision();
			Charge(*H,*Set,ChargeBytes); Set->Handles.push_back(H);
			return H;
		}
		Handle& From(lua_State* L,bool Edit) {
			auto* Value=static_cast<std::shared_ptr<Handle>*>(luaL_checkudata(L,1,Edit?EditName:CursorName));
			auto& H=**Value; H.Inspect(Clock::now()); return H;
		}
		std::shared_ptr<BlockGrid> Access(lua_State* L,Handle& H) {
			auto Grid=H.Owner.lock();
			if (!Grid) throw std::runtime_error("[BlockGrid:Expired] Owner no longer exists");
			(void)ReadTarget(L,Grid.get(),H.IsEdit);
			H.Last=Clock::now();
			if (H.IsEdit && (H.Status!=PageStatus::More || Grid->GetBlockRevision()!=H.Token)) {
				if (H.Status!=PageStatus::More) throw std::runtime_error("[BlockGrid:Expired] Edit is closed");
				throw std::domain_error("[BlockGrid:Conflict] Edit revision changed");
			}
			return Grid;
		}
		struct Method { const char* Name; lua_CFunction Function; };
		int PushHandle(lua_State* L,std::shared_ptr<Handle> H,const char* Name,std::initializer_list<Method> Methods) {
			using Ref=std::shared_ptr<Handle>;
			new(lua_newuserdatadtor(L,sizeof(Ref),[](void* P){std::destroy_at(static_cast<Ref*>(P));})) Ref(std::move(H));
			if (luaL_newmetatable(L,Name)) {
				lua_newtable(L);
				for (const auto& M:Methods) {lua_pushcfunction(L,M.Function,M.Name);lua_setfield(L,-2,M.Name);}
				lua_setreadonly(L,-1,true); lua_setfield(L,-2,"__index");
				lua_pushstring(L,Name);lua_setfield(L,-2,"__metatable");lua_setreadonly(L,-1,true);
			}
			lua_setmetatable(L,-2);return 1;
		}
		template<int (*Member)(lua_State*)> int Checked(lua_State* L) {return InvokeNativeCallback(L,[&]{return Member(L);});}
		template<bool Edit> int Cancel(lua_State* L) {From(L,Edit).Close(PageStatus::Cancelled);return 0;}
		enum class Action { Set,Remove,Fill,Clear,Value,Copy,Move,Packed };
		PackedWrite ParsePacked(lua_State* L,Region R,const Field& F,std::size_t Limit) {
			const auto N=Volume(R,Limit);
			Fields(L,4,{"Size","Bindings","Palette","Cells","Revision"});
			lua_rawgetfield(L,4,"Size");
			const auto Size=ReadCell(L,-1,true);lua_pop(L,1);
			if (Size!=Cell{R.MaxExclusive.X-R.Min.X,R.MaxExclusive.Y-R.Min.Y,R.MaxExclusive.Z-R.Min.Z})
				throw std::invalid_argument("[BlockGrid:Packed] Packed dimensions differ");
			lua_rawgetfield(L,4,"Palette");const auto PaletteIndex=lua_gettop(L); const auto Count=Array(L,PaletteIndex,static_cast<unsigned>(N));
			std::vector<Block> Palette{{}}; std::set<std::pair<std::uint16_t,std::uint8_t>> Used;
			std::set<std::uint16_t> Keys;
			for (unsigned I=1;I<=Count;++I) {
				lua_rawgeti(L,PaletteIndex,I); const auto P=lua_gettop(L); Fields(L,P,{"Definition","Orientation"});
				lua_rawgetfield(L,P,"Definition");const auto KeyIndex=lua_gettop(L); lua_rawgetfield(L,P,"Orientation");
				const auto B=ReadBlock(L,F,KeyIndex,lua_gettop(L));
				if (!Used.emplace(B.Definition,B.Orientation).second) throw std::invalid_argument("[BlockGrid:Packed] Duplicate palette entry");
				Keys.insert(B.Definition); Palette.push_back(B);lua_pop(L,3);
			}
			lua_pop(L,1);lua_rawgetfield(L,4,"Bindings");const auto Map=lua_gettop(L);Table(L,Map);
			lua_pushnil(L);
			while (lua_next(L,Map)) {
				const auto Id=F.GetDefinitionId(StringAt(L,-2));
				if (!Keys.erase(Id) || StringAt(L,-1)!=DefinitionDigest(F.GetDefinition(Id))) throw std::domain_error("[BlockGrid:Conflict] Packed binding does not match");
				lua_pop(L,1);
			}
			if (!Keys.empty()) throw std::invalid_argument("[BlockGrid:Packed] Incomplete palette bindings");
			lua_pop(L,1);lua_rawgetfield(L,4,"Cells");std::size_t Bytes=0; const auto Data=static_cast<const std::uint8_t*>(lua_tobuffer(L,-1,&Bytes));
			if (!Data || Bytes!=2*N) throw std::invalid_argument("[BlockGrid:Packed] Wrong cell buffer size");
			PackedWrite Out{R};Out.Values.reserve(N);
			for (std::size_t I=0;I<Bytes;I+=2) {
				const unsigned J=Data[I]|(unsigned(Data[I+1])<<8);
				if (J>=Palette.size()) throw std::invalid_argument("[BlockGrid:Packed] Palette index out of range");
				Out.Values.push_back(Palette[J]);
			}
			lua_pop(L,1);return Out;
		}
		template<Action A> int Enqueue(lua_State* L) {
			auto& H=From(L,true);
			try {
				(void)Access(L,H);
				if (H.Edit->Commands.size()>=MaximumCommands) throw std::length_error("[BlockGrid:Limit] Command capacity exhausted");
				const auto Remaining=MaximumEditVisits-H.Visits;
				if (!Remaining) throw std::length_error("[BlockGrid:Limit] Edit visit capacity exhausted");
				const auto P=ReadCell(L,2); Operation Op; std::size_t Cost=1;
				if constexpr (A==Action::Set) Op=Write{SingleCell(P),ReadBlock(L,*H.Shadow,3,4)};
				else if constexpr(A==Action::Remove) Op=Write{SingleCell(P),{}};
				else if constexpr(A==Action::Value) {
					const auto Id=H.Shadow->GetChannelId(StringAt(L,3));Op=ValueWrite{P,Id,Bits(L,4,H.Shadow->GetChannel(Id).Spec.Kind)};
				} else {
					const Region R{P,ReadCell(L,3,true)};
					if constexpr(A==Action::Copy || A==Action::Move) {
						Cost=2*Volume(R,Remaining/2);Op=Transfer{R,ReadCell(L,4),A==Action::Move};
					} else {
						Cost=Volume(R,Remaining);
						if constexpr(A==Action::Packed) Op=ParsePacked(L,R,*H.Shadow,Remaining);
						else if constexpr(A==Action::Fill) Op=Write{R,ReadBlock(L,*H.Shadow,4,5)};
						else Op=Write{R,{}};
					}
				}
				(void)H.Shadow->ApplyOperations(std::span(&Op,1));
				H.Edit->Commands.push_back(std::move(Op)); H.Visits+=Cost;
				Charge(H,*ScriptEngine::Get(L)->BlockGridHandles,H.Shadow->GetSnapshotBytes()+MiB);
				return 0;
			} catch (...) { H.Close(PageStatus::Cancelled);throw; }
		}
		int Commit(lua_State* L) {
			auto& H=From(L,true);
			try { auto Grid=Access(L,H);auto Request=std::move(H.Edit);H.Close(PageStatus::Complete);return SubmitRequest(L,*Grid,std::move(Request)); }
			catch (...) {H.Close(PageStatus::Cancelled);throw;}
		}
		int Step(lua_State* L) {
			auto& H=From(L,false); const auto Budget=Integer(L,2,1,1024);
			Page P{H.Status};
			if (H.Status==PageStatus::More) {
				(void)Access(L,H); P=H.Scan->Step(Budget); if (P.Status!=PageStatus::More) H.Close(P.Status);
			}
			constexpr std::array Names{"More","Complete","Found","Limit","Unavailable","Expired","Cancelled"};
			lua_createtable(L,0,4);PushText(L,Names[static_cast<unsigned>(P.Status)]);lua_setfield(L,-2,"Status");
			PushText(L,H.Token);lua_setfield(L,-2,"Revision");
			lua_createtable(L,static_cast<int>(P.Cells.size()),0);int I=0;
			for (auto Pos:P.Cells) {Vector(L,Pos);lua_rawseti(L,-2,++I);}
			lua_setfield(L,-2,"Cells");
			if (P.Match) {Vector(L,*P.Match);lua_setfield(L,-2,"Match");}
			return 1;
		}
	}
}

namespace gargantuan {
	using namespace blockgrid;
	using namespace blockgrid::binding;
	int BlockGrid::DefineChannel(lua_State* L,Instance* Object) {
		auto& Grid=ReadTarget(L,Object,true);
		Fields(L,3,{"Kind","Default","Persistent","Audience","Indexed","OnCopy"});
		const auto N=StringAt(L,2);
		if (N.size()>48) throw std::invalid_argument("[BlockGrid:Channel] Channel name is too long");
		ChannelSpec C;C.Name=N;
		lua_rawgetfield(L,3,"Kind");
		constexpr std::array<std::string_view,4> Types{"Bool","UInt8","Int32","Float32"};
		const auto Kind=std::ranges::find(Types,StringAt(L,-1));
		if (Kind==Types.end()) throw std::invalid_argument("[BlockGrid:Channel] Unknown channel kind");
		C.Kind=static_cast<ChannelKind>(Kind-Types.begin());lua_pop(L,1);
		lua_rawgetfield(L,3,"Default");C.Default=Bits(L,-1,C.Kind);lua_pop(L,1);
		lua_rawgetfield(L,3,"Persistent");if (!lua_isnil(L,-1)) C.Persistent=Boolean(L,-1);lua_pop(L,1);
		lua_rawgetfield(L,3,"Indexed");if (!lua_isnil(L,-1)) C.Indexed=Boolean(L,-1);lua_pop(L,1);
		lua_rawgetfield(L,3,"Audience");if (!lua_isnil(L,-1)) C.RelevantPeers=Choice(L,-1,"Server","RelevantPeers");lua_pop(L,1);
		lua_rawgetfield(L,3,"OnCopy");if (!lua_isnil(L,-1)) C.CopyValue=Choice(L,-1,"Default","Value");lua_pop(L,1);
		auto Edit=std::make_shared<EditRequest>();Edit->Channel=std::move(C);
		return SubmitRequest(L,Grid,std::move(Edit));
	}
	int BlockGrid::GetValue(lua_State* L,Instance* Object) {
		const auto& F=ReadTarget(L,Object).GetField();
		const auto Id=F.GetChannelId(StringAt(L,3));
		const auto V=ScalarValue(F.GetChannel(Id).Spec.Kind,F.GetValue(ReadCell(L,2),Id));
		if (const auto* B=std::get_if<bool>(&V)) lua_pushboolean(L,*B); else lua_pushnumber(L,std::get<double>(V));
		return 1;
	}
	int BlockGrid::BeginEdit(lua_State* L,Instance* Object) {
		auto& G=ReadTarget(L,Object,true);
		if (!lua_isnoneornil(L,2) && StringAt(L,2)!=G.GetBlockRevision()) throw std::domain_error("[BlockGrid:Conflict] Edit revision changed");
		auto H=Open(L,G,true,G.GetField().GetSnapshotBytes()+MiB);
		H->Edit=std::make_shared<EditRequest>();H->Edit->ExpectedRevision=H->Token;
		H->Shadow=std::make_unique<Field>(G.GetField());
		return PushHandle(L,std::move(H),EditName,{{"SetBlock",Checked<Enqueue<Action::Set>>},{"RemoveBlock",Checked<Enqueue<Action::Remove>>},
			{"Fill",Checked<Enqueue<Action::Fill>>},{"Clear",Checked<Enqueue<Action::Clear>>},{"SetValue",Checked<Enqueue<Action::Value>>},
			{"CopyRegion",Checked<Enqueue<Action::Copy>>},{"MoveRegion",Checked<Enqueue<Action::Move>>},{"WritePacked",Checked<Enqueue<Action::Packed>>},
			{"Cancel",Checked<Cancel<true>>},{"Commit",Checked<Commit>}});
	}
	int BlockGrid::FindConnected(lua_State* L,Instance* Object) {
		auto& G=ReadTarget(L,Object);const auto& F=G.GetField();
		const auto N=Array(L,2,64);
		if (!N) throw std::invalid_argument("[BlockGrid:Traversal] At least one seed is required");
		std::vector<Cell> Seeds;Seeds.reserve(N);
		for (unsigned I=1;I<=N;++I) {lua_rawgeti(L,2,I);Seeds.push_back(ReadCell(L,-1));lua_pop(L,1);}
		Fields(L,3,{"Bounds","Filter","Adjacency","UsePorts","StopAt","MaxVisited"});
		lua_rawgetfield(L,3,"Bounds"); TraversalOptions O{Bounds(L,-1)};lua_pop(L,1);
		lua_rawgetfield(L,3,"Filter");O.Select=ReadFilter(L,-1,F);lua_pop(L,1);
		lua_rawgetfield(L,3,"Adjacency");
		if (!lua_isnil(L,-1)) {
			constexpr std::array<std::string_view,3> Choices{"Face","Edge","Corner"};
			const auto It=std::ranges::find(Choices,StringAt(L,-1));
			if (It==Choices.end()) throw std::invalid_argument("[BlockGrid:Traversal] Unknown adjacency");
			O.Neighbors=static_cast<Adjacency>(It-Choices.begin());
		}
		lua_pop(L,1);
		lua_rawgetfield(L,3,"UsePorts");if (!lua_isnil(L,-1)) O.UsePorts=Boolean(L,-1);lua_pop(L,1);
		lua_rawgetfield(L,3,"StopAt");if (!lua_isnil(L,-1)) O.StopAt=Ids(L,-1,F);lua_pop(L,1);
		lua_rawgetfield(L,3,"MaxVisited");if (!lua_isnil(L,-1)) O.MaxVisited=Integer(L,-1,1,65'536);lua_pop(L,1);
		auto H=Open(L,G,false,F.GetSnapshotBytes()+O.MaxVisited*128);
		H->Scan=std::make_unique<Traversal>(std::make_shared<const Field>(F),Seeds,std::move(O));
		return PushHandle(L,std::move(H),CursorName,{{"Step",Checked<Step>},{"Cancel",Checked<Cancel<false>>}});
	}
	int BlockGrid::FindActive(lua_State* L,Instance* Object) {
		auto& G=ReadTarget(L,Object);const auto& F=G.GetField();
		const auto Id=F.GetChannelId(StringAt(L,2));const auto R=Bounds(L,3);
		auto H=Open(L,G,false,F.GetSnapshotBytes()+4096);
		H->Scan=std::make_unique<Traversal>(std::make_shared<const Field>(F),Id,R);
		return PushHandle(L,std::move(H),CursorName,{{"Step",Checked<Step>},{"Cancel",Checked<Cancel<false>>}});
	}
	int BlockGrid::ReadPacked(lua_State* L,Instance* Object) {
		auto& G=ReadTarget(L,Object);const auto& F=G.GetField();
		const Region R{ReadCell(L,2),ReadCell(L,3,true)};
		const auto Count=Volume(R);
		lua_createtable(L,0,5);
		Vector(L,{R.MaxExclusive.X-R.Min.X,R.MaxExclusive.Y-R.Min.Y,R.MaxExclusive.Z-R.Min.Z});lua_setfield(L,-2,"Size");
		PushText(L,G.GetBlockRevision());lua_setfield(L,-2,"Revision");
		auto* Bytes=static_cast<std::uint8_t*>(lua_newbuffer(L,Count*2));
		std::map<std::pair<std::uint16_t,std::uint8_t>,std::uint16_t> Lookup;
		std::vector<Block> Palette; std::set<std::uint16_t> Keys;
		std::size_t Offset=0;
		for (int Z=R.Min.Z;Z<R.MaxExclusive.Z;++Z) for (int Y=R.Min.Y;Y<R.MaxExclusive.Y;++Y) for (int X=R.Min.X;X<R.MaxExclusive.X;++X) {
			const auto B=F.Get({X,Y,Z});std::uint16_t Index{};
			if (B.Definition) {
				const auto [It,Added]=Lookup.try_emplace({B.Definition,B.Orientation},static_cast<std::uint16_t>(Palette.size()+1));
				if (Added) {Palette.push_back(B);Keys.insert(B.Definition);}
				Index=It->second;
			}
			Bytes[Offset++]=static_cast<std::uint8_t>(Index);Bytes[Offset++]=static_cast<std::uint8_t>(Index>>8);
		}
		lua_setfield(L,-2,"Cells");
		lua_createtable(L,static_cast<int>(Palette.size()),0);int I=0;
		for (auto B:Palette) {PushBlock(L,F,B);lua_rawseti(L,-2,++I);}lua_setfield(L,-2,"Palette");
		lua_createtable(L,0,static_cast<int>(Keys.size()));
		for (auto Id:Keys) {
			const auto& Spec=F.GetDefinition(Id);
			PushText(L,Spec.Key);PushText(L,DefinitionDigest(Spec));lua_rawset(L,-3);
		}
		lua_setfield(L,-2,"Bindings");
		return 1;
	}
}
