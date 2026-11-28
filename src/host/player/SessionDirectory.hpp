// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "StandalonePlayer.hpp"
#include "host/common/LocalGuestGrant.hpp"
#include "host/common/PackagedHost.hpp"
#include "gargantuan/network/GameNetworkingSocketsTransport.hpp"
#include "gargantuan/runtime/ApplicationSession.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"
#include "serialization/JsonCodec.hpp"
#include <algorithm>

namespace gargantuan::host {
	class SessionDirectory final {
	  public:
		SessionDirectory(LauncherDirectory Launcher, const PackageInspection &Package)
			: Launcher(std::move(Launcher)), Project(Package.Identity.ToString()), Manifest(Package.ManifestSha256) {}

		void Poll(ApplicationSession &Sessions) {
			for (unsigned Index = 0; Open && Index < 8; ++Index) {
				const auto Message = Launcher.Channel.Poll();
				if (!Message) {
					Open = false;
					Sessions.CloseDirectory();
					Cancel();
					break;
				}
				if (Message->empty()) break;
				const auto Frame = JsonCodec::Parse(*Message, 4096, "session directory");
				if (Frame && Frame->is_object()) Receive(Sessions, *Frame);
			}
		}
		void Cancel() { Waiting.reset(); }
		std::optional<PlayerAdmission> Acquire(const ApplicationSession &Sessions, std::string_view Id) {
			if (!Open) throw SessionAdmissionFailure::Unavailable;
			const auto *Row = Sessions.GetListing(Id);
			if (!Row || !Sessions.MatchesSelectedListing(*Row) || (Waiting && Waiting->Revoked))
				throw SessionAdmissionFailure::NotAuthorized;
			const auto Now = std::chrono::steady_clock::now();
			if (!Waiting) {
				Waiting.emplace(Request{Row->Id, ProjectId::New().ToString(), Row->Address, Row->ServerKey,
					std::min(Row->Expires, Now + std::chrono::seconds(8))});
				const JsonCodec::Json Frame{{"Version", 1}, {"Action", "Request"}, {"Source", Launcher.Source},
					{"SessionID", Row->Id}, {"RequestID", Waiting->Id}, {"ProjectID", Project}, {"PlayerPackageSHA256", Manifest}};
				if (!Launcher.Channel.Publish(Frame.dump())) throw SessionAdmissionFailure::Unavailable;
			}
			if (Waiting->Listing != Id || Now >= Waiting->Deadline) throw SessionAdmissionFailure::TimedOut;
			if (!Waiting->Grant) return {};
			auto Admission = std::move(Waiting->Grant);
			Cancel();
			return Admission;
		}
	  private:
		struct Request {
			std::string Listing, Id, Address, Key;
			std::chrono::steady_clock::time_point Deadline;
			bool Revoked{};
			std::optional<PlayerAdmission> Grant;
		};
		static std::string_view Field(const JsonCodec::Json &Frame, const char *Key) {
			const auto It = Frame.find(Key);
			return It != Frame.end() && It->is_string() ? std::string_view(It->get_ref<const std::string &>()) : std::string_view{};
		}
		void Receive(ApplicationSession &Sessions, const JsonCodec::Json &Frame) {
			if (!Frame.contains("Version") || Frame["Version"] != 1 || Field(Frame, "Source") != Launcher.Source ||
				Field(Frame, "ProjectID") != Project || Field(Frame, "PlayerPackageSHA256") != Manifest) return;
			const auto Id = Field(Frame, "SessionID"), Action = Field(Frame, "Action");
			if (!ProjectId::Parse(Id)) return;
			if (Action == "Withdraw") {
				Sessions.WithdrawSession(std::string(Id));
				if (Waiting && Waiting->Listing == Id) Waiting->Revoked = true;
				return;
			}
			const auto Expires = Frame.find("ExpiresUnixMillis");
			if (Expires == Frame.end() || !Expires->is_number_unsigned()) return;
			const auto End = Expires->get<std::uint64_t>(), Now = static_cast<std::uint64_t>(InvitationTimeMillis());
			if (End <= Now || End > Now + 60'000) return;
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(End - Now);
			const auto Address = Field(Frame, "Endpoint"), Key = Field(Frame, "ServerPublicKeyHex");
			const auto Endpoint = ParseEndpoint(Address);
			const auto ServerKey = detail::DecodePlayerJoinProof(Key);
			if (!Endpoint || !network::IsConcreteGnsEndpoint(*Endpoint) || !ServerKey) return;
			if (Action == "Offer") {
				const auto Name = Field(Frame, "Name");
				if (Name.empty() || Name.size() > 100 || !IsValidProtocolUtf8(Name) ||
					std::ranges::any_of(Name, [](unsigned char Ch) { return Ch < 32 || Ch == 127; })) return;
				Sessions.OfferSession({std::string(Id), std::string(Name), Deadline, std::string(Address), std::string(Key)});
			} else if (Action == "Grant" && Waiting && !Waiting->Revoked && Waiting->Listing == Id &&
				Field(Frame, "RequestID") == Waiting->Id && Waiting->Address == Address && Waiting->Key == Key) {
				const auto Proof = detail::DecodePlayerJoinProof(Field(Frame, "GuestProof"));
				if (!Proof) return;
				Waiting->Deadline = std::min(Waiting->Deadline, Deadline);
				Waiting->Grant = PlayerAdmission{*Endpoint, *Proof, *ServerKey};
			}
		}
		LauncherDirectory Launcher;
		std::string Project, Manifest;
		std::optional<Request> Waiting;
		bool Open = true;
	};
}
