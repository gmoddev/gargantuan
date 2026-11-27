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
		constexpr std::uint16_t Version = 2;
		bool ValidScope(const GridStreamScope &Scope) {
			return Scope.Epoch && Scope.Grid.IsValid() && (Scope.Domain.IsValid() || Scope.Domain == ObjectId{}) &&
				   Scope.Domain != Scope.Grid;
		}
		bool Valid(const GridStreamMessage &Message) {
			if (!ValidScope(Message.Scope) || !Message.Serial || !Message.Subscription) return false;
			switch (Message.Kind) {
			case GridStreamKind::Request:
			case GridStreamKind::Forget:
				return true;
			case GridStreamKind::Installed:
				return Message.Target.IsValid() && Message.Placement.IsValid() && Message.Revision && Message.Configuration;
			case GridStreamKind::Offer:
				return Message.Revision && Message.Configuration && Message.Placement.IsValid() && Message.Target.IsValid() && Message.Size &&
					   Message.Size <= MaximumGridReplicaBytes && Message.Prefix <= Message.Size &&
					   Message.Suffix <= Message.Size - Message.Prefix &&
					   (Message.Base.IsValid() || (!Message.Prefix && !Message.Suffix));
			case GridStreamKind::Fragment:
				return !Message.Fragment.empty() && Message.Fragment.size() <= MaximumGridStreamFragment &&
					   Message.Offset <= MaximumGridReplicaBytes - Message.Fragment.size();
			}
			return false;
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
		Writer.Integer(Value.Subscription);
		Writer.Integer(Value.Serial);
		if (Value.Kind == GridStreamKind::Request) Writer.Append(std::as_bytes(std::span(Value.Base.Bytes)));
		if (Value.Kind == GridStreamKind::Installed || Value.Kind == GridStreamKind::Offer) {
			Writer.Integer(Value.Revision);
			Writer.Integer(Value.Configuration);
			Writer.Append(std::as_bytes(std::span(Value.Target.Bytes)));
			Writer.Append(std::as_bytes(std::span(Value.Placement.Bytes)));
		}
		if (Value.Kind == GridStreamKind::Offer) {
			Writer.Append(std::as_bytes(std::span(Value.Base.Bytes)));
			Writer.Integer(Value.Size);
			Writer.Integer(Value.Prefix);
			Writer.Integer(Value.Suffix);
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
			!Reader.Integer(M.Scope.Domain.Generation) || !Reader.Integer(M.Subscription) || !Reader.Integer(M.Serial))
			return {};
		M.Kind = static_cast<GridStreamKind>(Kind);
		if (M.Kind == GridStreamKind::Request && !ReadDigest(Reader, M.Base)) return {};
		if ((M.Kind == GridStreamKind::Installed || M.Kind == GridStreamKind::Offer) &&
			(!Reader.Integer(M.Revision) || !Reader.Integer(M.Configuration) || !ReadDigest(Reader, M.Target) || !ReadDigest(Reader, M.Placement)))
			return {};
		if (M.Kind == GridStreamKind::Offer && (!ReadDigest(Reader, M.Base) || !Reader.Integer(M.Size) ||
												!Reader.Integer(M.Prefix) || !Reader.Integer(M.Suffix)))
			return {};
		if (M.Kind == GridStreamKind::Fragment) {
			std::span<const std::byte> Raw;
			if (!Reader.Integer(M.Offset) || !Reader.Take(Reader.Remaining(), Raw)) return {};
			M.Fragment = {reinterpret_cast<const std::uint8_t *>(Raw.data()), Raw.size()};
		}
		return Reader.Complete() && Valid(M) ? std::optional(M) : std::nullopt;
	}
	GridReplicaContent CaptureGridReplica(const blockgrid::Field &Field) {
		auto Bytes = std::make_shared<const std::vector<std::uint8_t>>(
			blockgrid::Encode(Field, blockgrid::DataProfile::RelevantPeers)
		);
		if (Bytes->size() > MaximumGridReplicaBytes)
			throw std::length_error("[BlockGrid:Network] Grid exceeds scoped replica limit");
		return {AssetContentId::Hash(*Bytes), std::move(Bytes)};
	}
	GridStreamMessage MakeGridOffer(
		GridStreamScope Scope,
		std::uint64_t Subscription,
		std::uint64_t Serial,
		std::uint64_t Revision,
		std::uint64_t Configuration,
		AssetContentId Placement,
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
			.Target = Next.Digest,
			.Placement = Placement,
			.Size = static_cast<std::uint32_t>(Next.Bytes->size())
		};
		if (Known && Known->Bytes && Known->Digest.IsValid()) {
			const auto Common = std::min(Next.Bytes->size(), Known->Bytes->size());
			while (Offer.Prefix < Common && (*Next.Bytes)[Offer.Prefix] == (*Known->Bytes)[Offer.Prefix])
				++Offer.Prefix;
			while (Offer.Suffix < Common - Offer.Prefix && (*Next.Bytes)[Next.Bytes->size() - 1 - Offer.Suffix] ==
															   (*Known->Bytes)[Known->Bytes->size() - 1 - Offer.Suffix])
				++Offer.Suffix;
			if (Offer.Prefix || Offer.Suffix) Offer.Base = Known->Digest;
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
		Bytes = std::move(Staged);
		Written = Offer.Prefix;
		Active = Offer;
	}
	void GridStreamReceiver::Append(const GridStreamMessage &Part) {
		if (!Active || Part.Kind != GridStreamKind::Fragment || !Valid(Part) || Part.Scope != Active->Scope ||
			Part.Subscription != Active->Subscription || Part.Serial != Active->Serial || Part.Offset != Written ||
			Part.Fragment.size() > Bytes.size() - Active->Suffix - Written)
			throw std::invalid_argument("[BlockGrid:Network] Out-of-order or stale fragment");
		std::copy(Part.Fragment.begin(), Part.Fragment.end(), Bytes.begin() + Written);
		Written += Part.Fragment.size();
	}
	bool GridStreamReceiver::Complete() const {
		return Active && Written == Bytes.size() - Active->Suffix;
	}
	GridStreamReceiver::Result GridStreamReceiver::Finish() {
		if (!Complete() || AssetContentId::Hash(Bytes) != Active->Target)
			throw std::invalid_argument("[BlockGrid:Network] Incomplete or corrupt replica");
		auto Field = blockgrid::Decode(Bytes);
		if (blockgrid::Encode(Field, blockgrid::DataProfile::RelevantPeers) != Bytes)
			throw std::invalid_argument("[BlockGrid:Network] Noncanonical public channel projection");
		GridReplicaContent Content{Active->Target, std::make_shared<const std::vector<std::uint8_t>>(std::move(Bytes))};
		Cancel();
		return {std::move(Field), std::move(Content)};
	}
	void GridStreamReceiver::Cancel() noexcept {
		Active.reset();
		Bytes = {};
		Written = 0;
	}
}
