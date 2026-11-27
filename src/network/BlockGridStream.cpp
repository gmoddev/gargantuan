// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/network/BlockGridStream.hpp"
#include "gargantuan/network/BinaryCodec.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace gargantuan::network {
	namespace {
		constexpr std::uint32_t Magic = 0x53474247; // GBGS
		constexpr std::uint16_t Version = 5;
		bool ValidScope(const GridStreamScope &Scope) {
			return Scope.Epoch && Scope.Grid.IsValid() && (Scope.Domain.IsValid() || Scope.Domain == ObjectId{}) &&
				   Scope.Domain != Scope.Grid;
		}
		bool Valid(const GridStreamMessage &Message) {
			if (!ValidScope(Message.Scope) || !Message.Serial || !Message.Subscription) return false;
			if ((Message.Kind==GridStreamKind::Offer || Message.Kind==GridStreamKind::Installed) && !Message.Membership) return false;
			if (Message.Fence.Other!=ObjectId{} && (!Message.Fence.Generation || !Message.Fence.Other.IsValid() ||
				Message.Fence.Other==Message.Scope.Grid)) return false;
			switch (Message.Kind) {
			case GridStreamKind::Request:
			case GridStreamKind::Forget:
			case GridStreamKind::Unavailable:
				return true;
			case GridStreamKind::Installed:
				return Message.Target.IsValid() && Message.Placement.IsValid() && Message.Revision &&
					   Message.Configuration && Message.MotionRevision;
			case GridStreamKind::Offer:
				if (Message.Reuse.size()>blockgrid::MaximumReplicaPages || (!Message.Reuse.empty() && !Message.Base.IsValid()) ||
					Message.Suffix>Message.Size || Message.Prefix>Message.Size-Message.Suffix) return false;
				{
					auto End=Message.Prefix;
					for (const auto R:Message.Reuse) {
						if (!R.Length || R.To<End || R.To>Message.Size-Message.Suffix ||
							R.Length>Message.Size-Message.Suffix-R.To || R.From>MaximumGridReplicaBytes ||
							R.Length>MaximumGridReplicaBytes-R.From) return false;
						End=R.To+R.Length;
					}
				}
				return Message.Assembly && blockgrid::ValidAssemblyProjection(*Message.Assembly) &&
					   Message.MotionRevision == Message.Assembly->Revision && Message.Revision &&
					   Message.Configuration && Message.Placement.IsValid() && Message.Target.IsValid() &&
					   Message.Size && Message.Size <= MaximumGridReplicaBytes && Message.Prefix <= Message.Size &&
					   Message.Suffix <= Message.Size - Message.Prefix &&
					   (Message.Base.IsValid() || (!Message.Prefix && !Message.Suffix));
			case GridStreamKind::Fragment:
				return !Message.Fragment.empty() && Message.Fragment.size() <= MaximumGridStreamFragment &&
					   Message.Offset <= MaximumGridReplicaBytes - Message.Fragment.size();
			}
			return false;
		}
		template <class Projection, class Component> bool VisitMotionComponents(Projection &P, Component Visit) {
			auto Vector = [&](auto &V) { return Visit(V.x) && Visit(V.y) && Visit(V.z); };
			auto Matrix = [&](auto &M) { return Vector(M[0]) && Vector(M[1]) && Vector(M[2]); };
			return Vector(P.Pose.Position) && Matrix(P.Pose.Rotation) && Vector(P.LinearVelocity) &&
				   Vector(P.AngularVelocity) && Visit(P.Mass) && Vector(P.LocalCenterOfMass) && Matrix(P.LocalInertia);
		}
		bool ReadDigest(GameBinaryReader &Reader, AssetContentId &Digest) {
			std::span<const std::byte> Raw;
			if (!Reader.Take(Digest.Bytes.size(), Raw)) return false;
			std::memcpy(Digest.Bytes.data(), Raw.data(), Raw.size());
			return true;
		}
	}
	bool IsGridStream(std::span<const std::byte> Bytes) {
		GameBinaryReader Reader(Bytes);
		std::uint32_t Value;
		return Reader.Integer(Value) && Value == Magic;
	}
	std::vector<std::byte> EncodeGridStream(const GridStreamMessage &Value) {
		if (!Valid(Value)) throw std::invalid_argument("[BlockGrid:Network] Invalid stream message");
		GameBinaryWriter Writer(MaximumGridStreamFragment + 128);
		Writer.Integer(Magic);
		Writer.Integer(Version);
		Writer.Integer(static_cast<std::uint8_t>(Value.Kind));
		Writer.Integer(Value.Scope.Epoch);
		Writer.Integer(Value.Scope.Grid.Slot);
		Writer.Integer(Value.Scope.Grid.Generation);
		Writer.Integer(Value.Scope.Domain.Slot);
		Writer.Integer(Value.Scope.Domain.Generation);
		Writer.Integer(Value.Scope.GrantGeneration);
		Writer.Integer(Value.Subscription);
		Writer.Integer(Value.Serial);
		if (Value.Kind == GridStreamKind::Request) Writer.Append(std::as_bytes(std::span(Value.Base.Bytes)));
		if (Value.Kind == GridStreamKind::Installed || Value.Kind == GridStreamKind::Offer) {
			Writer.Integer(Value.Revision);
			Writer.Integer(Value.Configuration);
			Writer.Integer(Value.MotionRevision);
			Writer.Integer(Value.Membership);
			Writer.Integer<std::uint8_t>(Value.RetainsController);
			Writer.Append(std::as_bytes(std::span(Value.Target.Bytes)));
			Writer.Append(std::as_bytes(std::span(Value.Placement.Bytes)));
			Writer.Integer(Value.Fence.Generation);
			Writer.Integer(Value.Fence.Other.Slot);
			Writer.Integer(Value.Fence.Other.Generation);
		}
		if (Value.Kind == GridStreamKind::Offer) {
			const auto &Assembly = *Value.Assembly;
			Writer.Integer(Assembly.Version);
			Writer.Integer<std::uint8_t>(Assembly.Dynamic);
			VisitMotionComponents(Assembly, [&](float Number) {
				Writer.Float(Number);
				return Writer.Succeeded();
			});
			Writer.Append(std::as_bytes(std::span(Value.Base.Bytes)));
			Writer.Integer(Value.Size);
			Writer.Integer(Value.Prefix);
			Writer.Integer(Value.Suffix);
			Writer.Integer(static_cast<std::uint8_t>(Value.Reuse.size()));
			for (const auto R:Value.Reuse) { Writer.Integer(R.From);Writer.Integer(R.To);Writer.Integer(R.Length); }
		} else if (Value.Kind == GridStreamKind::Fragment) {
			Writer.Integer(Value.Offset);
			Writer.Append(std::as_bytes(Value.Fragment));
		}
		if (!Writer.Succeeded()) throw std::length_error("[BlockGrid:Network] Stream frame exceeded its bound");
		return std::move(Writer.Bytes);
	}
	std::optional<GridStreamMessage> DecodeGridStream(std::span<const std::byte> Bytes) {
		if (Bytes.size() > MaximumGridStreamFragment + 128) return {};
		GameBinaryReader Reader(Bytes);
		std::uint32_t Signature;
		std::uint16_t Format;
		std::uint8_t Kind;
		GridStreamMessage M;
		if (!Reader.Integer(Signature) || Signature != Magic || !Reader.Integer(Format) || Format != Version ||
			!Reader.Integer(Kind) || !Reader.Integer(M.Scope.Epoch) || !Reader.Integer(M.Scope.Grid.Slot) ||
			!Reader.Integer(M.Scope.Grid.Generation) || !Reader.Integer(M.Scope.Domain.Slot) ||
			!Reader.Integer(M.Scope.Domain.Generation) || !Reader.Integer(M.Scope.GrantGeneration) || !Reader.Integer(M.Subscription) || !Reader.Integer(M.Serial))
			return {};
		M.Kind = static_cast<GridStreamKind>(Kind);
		if (M.Kind == GridStreamKind::Request && !ReadDigest(Reader, M.Base)) return {};
		if (M.Kind==GridStreamKind::Installed || M.Kind==GridStreamKind::Offer) {
			std::uint8_t Retained{};
			if (!Reader.Integer(M.Revision) || !Reader.Integer(M.Configuration) || !Reader.Integer(M.MotionRevision) ||
				!Reader.Integer(M.Membership) || !Reader.Integer(Retained) || Retained>1 || !ReadDigest(Reader,M.Target) ||
				!ReadDigest(Reader,M.Placement) || !Reader.Integer(M.Fence.Generation) ||
				!Reader.Integer(M.Fence.Other.Slot) || !Reader.Integer(M.Fence.Other.Generation)) return {};
			M.RetainsController=Retained!=0;
		}
		if (M.Kind == GridStreamKind::Offer) {
			auto &Assembly = M.Assembly.emplace();
			std::uint8_t Dynamic{};
			if (!Reader.Integer(Assembly.Version) || !Reader.Integer(Dynamic) || Dynamic > 1 ||
				!VisitMotionComponents(Assembly, [&](float &Number) { return Reader.Float(Number); }))
				return {};
			Assembly.Revision = M.MotionRevision;
			Assembly.Dynamic = Dynamic != 0;
		}
		if (M.Kind == GridStreamKind::Offer && (!ReadDigest(Reader, M.Base) || !Reader.Integer(M.Size) ||
												!Reader.Integer(M.Prefix) || !Reader.Integer(M.Suffix)))
			return {};
		if (M.Kind==GridStreamKind::Offer) {
			std::uint8_t Count{};
			if (!Reader.Integer(Count) || Count>blockgrid::MaximumReplicaPages || Count>Reader.Remaining()/12) return {};
			M.Reuse.resize(Count);
			for (auto& R:M.Reuse) if (!Reader.Integer(R.From) || !Reader.Integer(R.To) || !Reader.Integer(R.Length)) return {};
		}
		if (M.Kind == GridStreamKind::Fragment) {
			std::span<const std::byte> Raw;
			if (!Reader.Integer(M.Offset) || !Reader.Take(Reader.Remaining(), Raw)) return {};
			M.Fragment = {reinterpret_cast<const std::uint8_t *>(Raw.data()), Raw.size()};
		}
		return Reader.Complete() && Valid(M) ? std::optional(M) : std::nullopt;
	}
	GridReplicaContent CaptureGridReplica(const blockgrid::Field &Field) {
		return *CaptureGridReplica(Field,blockgrid::ReplicaCoverage{true},{});
	}
	std::optional<GridReplicaContent> CaptureGridReplica(const blockgrid::Field& Field,
		const blockgrid::ReplicaCoverage& Coverage,const std::function<bool(const blockgrid::Field&)>& AdmitSchema) {
		auto Pages=Field.CapturePages(Coverage,AdmitSchema);
		if (!Pages) return {};
		if (Pages->Bytes.size()>MaximumGridReplicaBytes) throw std::length_error("[BlockGrid:Network] Regional page budget exceeded");
		auto Bytes=std::make_shared<const std::vector<std::uint8_t>>(std::move(Pages->Bytes));
		return GridReplicaContent{AssetContentId::Hash(*Bytes),std::move(Bytes),
			std::make_shared<const blockgrid::ReplicaCoverage>(Coverage),
			std::make_shared<const std::vector<blockgrid::ReplicaPageRange>>(std::move(Pages->Records))};
	}
	std::pair<std::size_t,std::size_t> NextGridLiteral(const GridStreamMessage& Offer,std::size_t Cursor) {
		const auto End=Offer.Size-Offer.Suffix;
		for (const auto R:Offer.Reuse) {
			if (Cursor<R.To) return {Cursor,R.To-Cursor};
			if (Cursor<R.To+R.Length) Cursor=R.To+R.Length;
		}
		return {Cursor,End-Cursor};
	}
	GridStreamMessage MakeGridOffer(
		GridStreamScope Scope,
		std::uint64_t Subscription,
		std::uint64_t Serial,
		std::uint64_t Revision,
		std::uint64_t Configuration,
		AssetContentId Placement,
		const blockgrid::AssemblyProjection &Assembly,
		const GridReplicaContent &Next,
		const GridReplicaContent *Known
	) {
		if (!Next.Bytes || !Next.Digest.IsValid() || Next.Bytes->empty() ||
			Next.Bytes->size() > MaximumGridReplicaBytes)
			throw std::invalid_argument("[BlockGrid:Network] Bad authored snapshot");
		GridStreamMessage Offer{
			.Kind = GridStreamKind::Offer,
			.Scope = Scope,
			.Subscription = Subscription,
			.Serial = Serial,
			.Revision = Revision,
			.Configuration = Configuration,
			.MotionRevision = Assembly.Revision,
			.Target = Next.Digest,
			.Placement = Placement,
			.Assembly = Assembly,
			.Size = static_cast<std::uint32_t>(Next.Bytes->size())
		};
		if (Known && Known->Bytes && Known->Digest.IsValid()) {
			if (Next.Bytes == Known->Bytes) {
				Offer.Base = Known->Digest;
				Offer.Prefix = Offer.Size;
				if (!Valid(Offer)) throw std::invalid_argument("[BlockGrid:Network] Invalid motion-only offer");
				return Offer;
			}
			const auto Common = std::min(Next.Bytes->size(), Known->Bytes->size());
			while (Offer.Prefix < Common && (*Next.Bytes)[Offer.Prefix] == (*Known->Bytes)[Offer.Prefix])
				++Offer.Prefix;
			while (Offer.Suffix < Common - Offer.Prefix && (*Next.Bytes)[Next.Bytes->size() - 1 - Offer.Suffix] ==
															   (*Known->Bytes)[Known->Bytes->size() - 1 - Offer.Suffix])
				++Offer.Suffix;
			if (Offer.Prefix || Offer.Suffix) Offer.Base = Known->Digest;
			if (Next.Records && Known->Records) for (const auto& Page:*Next.Records) {
				const auto End=std::min(Page.Offset+Page.Length,Offer.Size-Offer.Suffix);
				const auto Start=std::max(Page.Offset,Offer.Prefix);
				if (End<=Start || End-Start<16) continue;
				const auto It=std::ranges::lower_bound(*Known->Records,Page.Origin,blockgrid::CellOrder{},&blockgrid::ReplicaPageRange::Origin);
				if (It==Known->Records->end() || It->Origin!=Page.Origin || It->Length!=Page.Length) continue;
				const auto Old=It->Offset+(Start-Page.Offset);
				if (!std::equal(Next.Bytes->begin()+Start,Next.Bytes->begin()+End,Known->Bytes->begin()+Old)) continue;
				if (!Offer.Reuse.empty() && Offer.Reuse.back().To+Offer.Reuse.back().Length==Start &&
					Offer.Reuse.back().From+Offer.Reuse.back().Length==Old) Offer.Reuse.back().Length+=End-Start;
				else Offer.Reuse.push_back({Old,Start,End-Start});
				Offer.Base=Known->Digest;
			}
		}
		if (!Valid(Offer)) throw std::invalid_argument("[BlockGrid:Network] Invalid source binding");
		return Offer;
	}
	void GridStreamReceiver::Begin(
		const GridStreamMessage &Offer, const GridStreamScope &Granted, const GridReplicaContent *Known
	) {
		if (Active || Offer.Kind != GridStreamKind::Offer || !Valid(Offer) || Offer.Scope != Granted)
			throw std::invalid_argument("[BlockGrid:Network] Unsolicited or overlapping offer");
		if (Offer.Base.IsValid() &&
			(!Known || !Known->Bytes || Known->Digest != Offer.Base || Offer.Prefix > Known->Bytes->size() ||
			 Offer.Suffix > Known->Bytes->size() - Offer.Prefix))
			throw std::invalid_argument("[BlockGrid:Network] Replica base mismatch");
		std::vector<std::uint8_t> Staged(Offer.Size);
		if (Offer.Prefix) std::copy_n(Known->Bytes->begin(), Offer.Prefix, Staged.begin());
		if (Offer.Suffix)
			std::copy(Known->Bytes->end() - Offer.Suffix, Known->Bytes->end(), Staged.end() - Offer.Suffix);
		for (const auto R:Offer.Reuse) {
			if (R.From>Known->Bytes->size() || R.Length>Known->Bytes->size()-R.From)
				throw std::invalid_argument("[BlockGrid:Network] Invalid prior page reference");
			std::copy_n(Known->Bytes->begin()+R.From,R.Length,Staged.begin()+R.To);
		}
		Bytes = std::move(Staged);
		Written = NextGridLiteral(Offer,Offer.Prefix).first;
		Active = Offer;
	}
	void GridStreamReceiver::Append(const GridStreamMessage &Part) {
		if (!Active || Part.Kind != GridStreamKind::Fragment || !Valid(Part) || Part.Scope != Active->Scope ||
			Part.Subscription != Active->Subscription || Part.Serial != Active->Serial || Part.Offset != Written ||
			Part.Fragment.size() > NextGridLiteral(*Active,Written).second)
			throw std::invalid_argument("[BlockGrid:Network] Out-of-order or stale fragment");
		std::copy(Part.Fragment.begin(), Part.Fragment.end(), Bytes.begin() + Written);
		Written = NextGridLiteral(*Active,Written+Part.Fragment.size()).first;
	}
	bool GridStreamReceiver::Complete() const {
		return Active && Written == Bytes.size() - Active->Suffix;
	}
	GridStreamReceiver::Result GridStreamReceiver::Finish() {
		if (!Complete() || AssetContentId::Hash(Bytes) != Active->Target)
			throw std::invalid_argument("[BlockGrid:Network] Incomplete or corrupt replica");
		blockgrid::ReplicaCoverage Coverage;
		auto Field=blockgrid::Field::DecodePages(Bytes,Coverage);
		GridReplicaContent Content{Active->Target,std::make_shared<const std::vector<std::uint8_t>>(std::move(Bytes)),
			std::make_shared<const blockgrid::ReplicaCoverage>(std::move(Coverage))};
		Cancel();
		return {std::move(Field), std::move(Content)};
	}
	void GridStreamReceiver::Cancel() noexcept {
		Active.reset();
		Bytes = {};
		Written = 0;
	}
}
