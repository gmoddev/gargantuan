#include "gargantuan/network/GameNetworkingSocketsTransport.hpp"

#include <steam/steamnetworkingsockets.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gargantuan::network {
	namespace {
		constexpr std::array<std::byte, 4> AdapterMagic{
			static_cast<std::byte>('G'), static_cast<std::byte>('G'),
			static_cast<std::byte>('N'), static_cast<std::byte>('S')
		};
		constexpr std::uint8_t AdapterEnvelopeVersion = 1;
		constexpr std::size_t AdapterEnvelopeBytes = 24;
		constexpr std::size_t BackendMaximumUnreliableFrameBytes = 1200;
		constexpr int ResourceExhaustionEndReason = k_ESteamNetConnectionEnd_AppException_Min + 1;
		constexpr int ProtocolViolationEndReason = k_ESteamNetConnectionEnd_AppException_Min + 2;
		constexpr int IncompatibleVersionEndReason = k_ESteamNetConnectionEnd_AppException_Min + 3;

		struct GlobalGnsState {
			std::recursive_mutex Mutex;
			std::uint32_t ReferenceCount = 0;
			ISteamNetworkingSockets *Interface = nullptr;
			std::unordered_map<HSteamNetConnection, void *> ConnectionOwners;
			std::unordered_map<HSteamListenSocket, void *> ListenerOwners;
		};

		GlobalGnsState &GlobalState() {
			static GlobalGnsState State;
			return State;
		}

		TransportOperationResult Operation(TransportOperationStatus Status) {
			return {.Status = Status};
		}

		TransportOperationResult TerminalOperation(
			TransportOperationStatus Status,
			DisconnectInfo Information
		) {
			return {.Status = Status, .TerminalDisconnect = std::move(Information)};
		}

		bool CheckedAdd(std::size_t First, std::size_t Second, std::size_t &Result) {
			if (First > std::numeric_limits<std::size_t>::max() - Second) return false;
			Result = First + Second;
			return true;
		}

		void SaturatingAdd(std::uint64_t &Value, std::uint64_t Delta) {
			Value = Delta > std::numeric_limits<std::uint64_t>::max() - Value
				? std::numeric_limits<std::uint64_t>::max() : Value + Delta;
		}

		void WriteU64(std::span<std::byte, 8> Output, std::uint64_t Value) {
			for (std::size_t Index = 0; Index < Output.size(); ++Index)
				Output[Index] = static_cast<std::byte>(Value >> ((7 - Index) * 8));
		}

		std::uint64_t ReadU64(std::span<const std::byte, 8> Input) {
			std::uint64_t Result = 0;
			for (const auto Value : Input) Result = (Result << 8) | std::to_integer<std::uint8_t>(Value);
			return Result;
		}

		std::uint8_t OrderKind(const MessageOrder &Order) {
			if (std::holds_alternative<std::monostate>(Order)) return 0;
			if (std::holds_alternative<RealtimeStateOrder>(Order)) return 1;
			if (std::holds_alternative<RemoteEventOrder>(Order)) return 2;
			if (std::holds_alternative<ReliableReplicationOrder>(Order)) return 3;
			return std::numeric_limits<std::uint8_t>::max();
		}

		std::pair<std::uint64_t, std::uint64_t> OrderValues(const MessageOrder &Order) {
			if (const auto *Realtime = std::get_if<RealtimeStateOrder>(&Order))
				return {Realtime->Channel.Value(), Realtime->Sequence.Value()};
			if (const auto *Event = std::get_if<RemoteEventOrder>(&Order))
				return {Event->Channel.Value(), Event->Sequence.Value()};
			if (const auto *Reliable = std::get_if<ReliableReplicationOrder>(&Order))
				return {0, Reliable->Sequence.Value()};
			return {0, 0};
		}

		std::optional<MessageOrder> DecodeOrder(
			std::uint8_t Kind,
			std::uint64_t Channel,
			std::uint64_t Sequence
		) {
			switch (Kind) {
			case 0:
				if (Channel != 0 || Sequence != 0) return std::nullopt;
				return MessageOrder{std::monostate{}};
			case 1:
				if (Channel == 0 || Sequence == 0) return std::nullopt;
				return MessageOrder{RealtimeStateOrder{StateChannelId(Channel), RealtimeStateSequence(Sequence)}};
			case 2:
				if (Channel == 0 || Sequence == 0) return std::nullopt;
				return MessageOrder{RemoteEventOrder{StateChannelId(Channel), RemoteEventSequence(Sequence)}};
			case 3:
				if (Channel != 0 || Sequence == 0) return std::nullopt;
				return MessageOrder{ReliableReplicationOrder{ReliableReplicationSequence(Sequence)}};
			default:
				return std::nullopt;
			}
		}

		std::optional<std::vector<std::byte>> EncodeFrame(const NetworkMessageIntent &Message) {
			std::size_t FrameBytes = 0;
			if (!CheckedAdd(AdapterEnvelopeBytes, Message.Payload().size(), FrameBytes) ||
				FrameBytes > static_cast<std::size_t>(k_cbMaxSteamNetworkingSocketsMessageSizeSend)) return std::nullopt;
			std::vector<std::byte> Result(FrameBytes);
			std::copy(AdapterMagic.begin(), AdapterMagic.end(), Result.begin());
			Result[4] = static_cast<std::byte>(AdapterEnvelopeVersion);
			Result[5] = static_cast<std::byte>(Message.Delivery());
			Result[6] = static_cast<std::byte>(Message.Traffic());
			Result[7] = static_cast<std::byte>(OrderKind(Message.Order()));
			const auto [Channel, Sequence] = OrderValues(Message.Order());
			WriteU64(std::span<std::byte, 8>(Result.data() + 8, 8), Channel);
			WriteU64(std::span<std::byte, 8>(Result.data() + 16, 8), Sequence);
			std::copy(Message.Payload().begin(), Message.Payload().end(), Result.begin() + AdapterEnvelopeBytes);
			return Result;
		}

		std::string BoundedDiagnostic(const char *Text) {
			if (!Text) return {};
			std::size_t Length = 0;
			while (Length < k_cchSteamNetworkingMaxConnectionCloseReason && Text[Length] != '\0') ++Length;
			return std::string(Text, Length);
		}

		DisconnectReason MapRemoteDisconnect(int Reason, ESteamNetworkingConnectionState State) {
			if (Reason == ResourceExhaustionEndReason) return DisconnectReason::ResourceExhaustion;
			if (Reason == ProtocolViolationEndReason || Reason == k_ESteamNetConnectionEnd_Remote_BadCrypt)
				return DisconnectReason::ProtocolViolation;
			if (Reason == IncompatibleVersionEndReason ||
				Reason == k_ESteamNetConnectionEnd_Remote_BadProtocolVersion)
				return DisconnectReason::IncompatibleVersion;
			if (Reason == k_ESteamNetConnectionEnd_Remote_Timeout ||
				Reason == k_ESteamNetConnectionEnd_Misc_Timeout) return DisconnectReason::Timeout;
			if (State == k_ESteamNetworkingConnectionState_ClosedByPeer &&
				Reason >= k_ESteamNetConnectionEnd_App_Min && Reason <= k_ESteamNetConnectionEnd_App_Max)
				return DisconnectReason::RemoteShutdown;
			return DisconnectReason::TransportFailure;
		}

		int BackendDisconnectReason(DisconnectReason Reason) {
			switch (Reason) {
			case DisconnectReason::LocalShutdown:
			case DisconnectReason::RemoteShutdown:
				return k_ESteamNetConnectionEnd_App_Generic;
			case DisconnectReason::ResourceExhaustion:
				return ResourceExhaustionEndReason;
			case DisconnectReason::ProtocolViolation:
				return ProtocolViolationEndReason;
			case DisconnectReason::IncompatibleVersion:
				return IncompatibleVersionEndReason;
			case DisconnectReason::Timeout:
			case DisconnectReason::AuthenticationFailure:
			case DisconnectReason::TransportFailure:
				return k_ESteamNetConnectionEnd_AppException_Generic;
			}
			return k_ESteamNetConnectionEnd_AppException_Generic;
		}

		const char *BackendDisconnectDiagnostic(DisconnectReason Reason) {
			switch (Reason) {
			case DisconnectReason::LocalShutdown:
				return "Gargantuan local shutdown";
			case DisconnectReason::RemoteShutdown:
				return "Gargantuan remote shutdown";
			case DisconnectReason::Timeout:
				return "Gargantuan timeout";
			case DisconnectReason::AuthenticationFailure:
				return "Gargantuan authentication failure";
			case DisconnectReason::ProtocolViolation:
				return "Gargantuan protocol violation";
			case DisconnectReason::ResourceExhaustion:
				return "Gargantuan resource exhaustion";
			case DisconnectReason::TransportFailure:
				return "Gargantuan transport failure";
			case DisconnectReason::IncompatibleVersion:
				return "Gargantuan incompatible version";
			}
			return "Gargantuan connection closed";
		}

		int BoundedBackendBuffer(std::size_t Bytes) {
			constexpr std::size_t BackendMinimum = 4096;
			constexpr std::size_t BackendMaximum = 0x10000000;
			return static_cast<int>(std::clamp(Bytes, BackendMinimum, BackendMaximum));
		}
	}

	struct GameNetworkingSocketsTransport::Impl {
		struct ConnectionRecord {
			HSteamNetConnection Handle = k_HSteamNetConnection_Invalid;
			ConnectionState State = ConnectionState::Connecting;
			NetworkStatistics Statistics;
		};

		explicit Impl(GameNetworkingSocketsTransportConfiguration Value) : Configuration(std::move(Value)) {}

		GameNetworkingSocketsTransportConfiguration Configuration;
		bool Started = false;
		bool OwnsGlobalReference = false;
		TransportRole Role = TransportRole::Client;
		TransportEndpoint Endpoint;
		NetworkLimits Limits;
		HSteamListenSocket Listener = k_HSteamListenSocket_Invalid;
		std::unordered_map<ConnectionId, ConnectionRecord> Connections;
		std::unordered_map<HSteamNetConnection, ConnectionId> BackendConnections;
		std::vector<std::uint32_t> Generations{0};
		std::deque<std::uint32_t> FreeSlots;
		std::deque<TransportEvent> Events;
		std::size_t PendingReceiveBytes = 0;

		static void StatusChanged(SteamNetConnectionStatusChangedCallback_t *Information) {
			if (!Information) return;
			auto &Global = GlobalState();
			std::lock_guard Lock(Global.Mutex);
			void *Owner = nullptr;
			if (const auto Connection = Global.ConnectionOwners.find(Information->m_hConn);
				Connection != Global.ConnectionOwners.end()) Owner = Connection->second;
			else if (const auto ListenerOwner = Global.ListenerOwners.find(Information->m_info.m_hListenSocket);
				ListenerOwner != Global.ListenerOwners.end()) Owner = ListenerOwner->second;
			if (Owner) static_cast<Impl *>(Owner)->OnStatusChanged(*Information);
		}

		bool AcquireGlobal() {
			auto &Global = GlobalState();
			if (Global.ReferenceCount == 0) {
				SteamNetworkingErrMsg Error{};
				if (!GameNetworkingSockets_Init(nullptr, Error)) return false;
				Global.Interface = SteamNetworkingSockets();
				if (!Global.Interface) {
					GameNetworkingSockets_Kill();
					return false;
				}
			}
			if (Global.ReferenceCount == std::numeric_limits<std::uint32_t>::max()) return false;
			++Global.ReferenceCount;
			OwnsGlobalReference = true;
			return true;
		}

		void ReleaseGlobal() {
			if (!OwnsGlobalReference) return;
			auto &Global = GlobalState();
			OwnsGlobalReference = false;
			if (--Global.ReferenceCount == 0) {
				Global.ConnectionOwners.clear();
				Global.ListenerOwners.clear();
				GameNetworkingSockets_Kill();
				Global.Interface = nullptr;
			}
		}

		std::optional<ConnectionId> AllocateConnection(HSteamNetConnection Handle) {
			if (Connections.size() >= Configuration.MaximumConnections) return std::nullopt;
			ConnectionId Id;
			while (!FreeSlots.empty()) {
				const auto Slot = FreeSlots.front();
				FreeSlots.pop_front();
				if (Generations[Slot] == std::numeric_limits<std::uint32_t>::max()) continue;
				Id = {Slot, ++Generations[Slot]};
				break;
			}
			if (!Id.IsValid()) {
				if (Generations.size() > Configuration.MaximumConnections) return std::nullopt;
				const auto Slot = static_cast<std::uint32_t>(Generations.size());
				Generations.push_back(1);
				Id = {Slot, 1};
			}
			ConnectionRecord Record;
			Record.Handle = Handle;
			Record.Statistics.BytesSent = 0;
			Record.Statistics.BytesReceived = 0;
			Record.Statistics.MessagesSent = 0;
			Record.Statistics.MessagesReceived = 0;
			Connections.emplace(Id, std::move(Record));
			BackendConnections.emplace(Handle, Id);
			GlobalState().ConnectionOwners[Handle] = this;
			return Id;
		}

		void ReleaseConnection(ConnectionId Id, HSteamNetConnection Handle) {
			GlobalState().ConnectionOwners.erase(Handle);
			BackendConnections.erase(Handle);
			Connections.erase(Id);
			if (Id.Slot < Generations.size() && Generations[Id.Slot] == Id.Generation &&
				Id.Generation != std::numeric_limits<std::uint32_t>::max()) FreeSlots.push_back(Id.Slot);
		}

		bool QueueEvent(TransportEvent Event) {
			if (const auto *Message = std::get_if<ReceivedMessageEvent>(&Event)) {
				const auto Reserved = static_cast<std::size_t>(Configuration.MaximumConnections) * 5;
				if (Events.size() >= Configuration.MaximumPendingEvents - Reserved) return false;
				std::size_t NewBytes = 0;
				if (!CheckedAdd(PendingReceiveBytes, Message->Payload.size(), NewBytes) ||
					NewBytes > Configuration.MaximumPendingReceiveBytes) return false;
				PendingReceiveBytes = NewBytes;
			}
			if (Events.size() >= Configuration.MaximumPendingEvents) return false;
			Events.push_back(std::move(Event));
			return true;
		}

		void RemoveConnectionMessageEvents(ConnectionId Id) {
			std::erase_if(Events, [&](const TransportEvent &Event) {
				const auto *Message = std::get_if<ReceivedMessageEvent>(&Event);
				if (!Message || Message->Connection != Id) return false;
				PendingReceiveBytes -= Message->Payload.size();
				return true;
			});
		}

		void CloseConnection(ConnectionId Id, DisconnectInfo Information, bool NotifyBackend) {
			const auto Iterator = Connections.find(Id);
			if (Iterator == Connections.end()) return;
			auto Handle = Iterator->second.Handle;
			auto Previous = Iterator->second.State;
			RemoveConnectionMessageEvents(Id);
			if (Previous != ConnectionState::Closing && Previous != ConnectionState::Closed) {
				QueueEvent(ConnectionStateEvent{Id, Previous, ConnectionState::Closing});
				Previous = ConnectionState::Closing;
			}
			QueueEvent(ConnectionStateEvent{Id, Previous, ConnectionState::Closed});
			QueueEvent(DisconnectedEvent{Id, Information});
			if (NotifyBackend && GlobalState().Interface)
				GlobalState().Interface->CloseConnection(
					Handle,
					BackendDisconnectReason(Information.Reason),
					BackendDisconnectDiagnostic(Information.Reason),
					false
				);
			else if (GlobalState().Interface)
				GlobalState().Interface->CloseConnection(Handle, 0, nullptr, false);
			ReleaseConnection(Id, Handle);
		}

		void FailConnection(ConnectionId Id, DisconnectReason Reason, std::string Diagnostic) {
			DisconnectInfo Information{Reason, std::move(Diagnostic)};
			if (!Information.IsValid()) Information = {Reason, "GameNetworkingSockets transport failure"};
			CloseConnection(Id, std::move(Information), true);
		}

		void OnStatusChanged(const SteamNetConnectionStatusChangedCallback_t &Information) {
			if (!Started) return;
			auto IdIterator = BackendConnections.find(Information.m_hConn);
			if (Information.m_info.m_eState == k_ESteamNetworkingConnectionState_Connecting &&
				IdIterator == BackendConnections.end()) {
				if (Role != TransportRole::Server || Information.m_info.m_hListenSocket != Listener) return;
				if (Events.size() > Configuration.MaximumPendingEvents - 5) {
					GlobalState().Interface->CloseConnection(
						Information.m_hConn,
						ResourceExhaustionEndReason,
						"Gargantuan lifecycle event limit reached",
						false
					);
					return;
				}
				auto Id = AllocateConnection(Information.m_hConn);
				if (!Id) {
					GlobalState().Interface->CloseConnection(
						Information.m_hConn,
						ResourceExhaustionEndReason,
						"Gargantuan connection limit reached",
						false
					);
					return;
				}
				if (!QueueEvent(ConnectionStateEvent{*Id, ConnectionState::Connecting, ConnectionState::Authenticating}) ||
					GlobalState().Interface->AcceptConnection(Information.m_hConn) != k_EResultOK) {
					FailConnection(*Id, DisconnectReason::TransportFailure, "Unable to accept GNS connection");
				} else Connections.find(*Id)->second.State = ConnectionState::Authenticating;
				return;
			}
			if (IdIterator == BackendConnections.end()) return;
			const auto Id = IdIterator->second;
			auto Connection = Connections.find(Id);
			if (Connection == Connections.end()) return;
			switch (Information.m_info.m_eState) {
			case k_ESteamNetworkingConnectionState_Connected:
				if (Connection->second.State == ConnectionState::Connecting) {
					QueueEvent(ConnectionStateEvent{Id, ConnectionState::Connecting, ConnectionState::Authenticating});
					Connection->second.State = ConnectionState::Authenticating;
				}
				if (Connection->second.State == ConnectionState::Authenticating) {
					QueueEvent(ConnectionStateEvent{Id, ConnectionState::Authenticating, ConnectionState::Connected});
					Connection->second.State = ConnectionState::Connected;
				}
				break;
			case k_ESteamNetworkingConnectionState_ClosedByPeer:
			case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
				const auto Reason = MapRemoteDisconnect(Information.m_info.m_eEndReason, Information.m_info.m_eState);
				auto Diagnostic = BoundedDiagnostic(Information.m_info.m_szEndDebug);
				if (Diagnostic.empty()) Diagnostic = "GameNetworkingSockets connection ended";
				CloseConnection(Id, {Reason, std::move(Diagnostic)}, false);
				break;
			}
			default:
				break;
			}
		}

		std::optional<ReceivedMessageEvent> DecodeMessage(ConnectionId Id, SteamNetworkingMessage_t &Message) {
			if (Message.m_cbSize <= static_cast<int>(AdapterEnvelopeBytes) || !Message.m_pData) return std::nullopt;
			const auto FrameBytes = static_cast<std::size_t>(Message.m_cbSize);
			if (FrameBytes > static_cast<std::size_t>(k_cbMaxSteamNetworkingSocketsMessageSizeSend)) return std::nullopt;
			const auto Frame = std::span(static_cast<const std::byte *>(Message.m_pData), FrameBytes);
			if (!std::equal(AdapterMagic.begin(), AdapterMagic.end(), Frame.begin()) ||
				std::to_integer<std::uint8_t>(Frame[4]) != AdapterEnvelopeVersion) return std::nullopt;
			const auto Delivery = static_cast<DeliveryMode>(std::to_integer<std::uint8_t>(Frame[5]));
			const auto Traffic = static_cast<TrafficClass>(std::to_integer<std::uint8_t>(Frame[6]));
			const auto Kind = std::to_integer<std::uint8_t>(Frame[7]);
			if (!IsValidDeliveryMode(Delivery) || !IsValidTrafficClass(Traffic)) return std::nullopt;
			const bool BackendReliable = (Message.m_nFlags & k_nSteamNetworkingSend_Reliable) != 0;
			if (BackendReliable != (Delivery == DeliveryMode::ReliableOrdered)) return std::nullopt;
			auto Order = DecodeOrder(
				Kind,
				ReadU64(std::span<const std::byte, 8>(Frame.data() + 8, 8)),
				ReadU64(std::span<const std::byte, 8>(Frame.data() + 16, 8))
			);
			if (!Order) return std::nullopt;
			const auto PayloadBytes = FrameBytes - AdapterEnvelopeBytes;
			const auto MessageLimit = Delivery == DeliveryMode::ReliableOrdered
				? Limits.MaximumReliableMessageBytes : Limits.MaximumUnreliableMessageBytes;
			if (PayloadBytes == 0 || PayloadBytes > MessageLimit ||
				PayloadBytes > Limits.MaximumDecodedMessageBytes ||
				PayloadBytes > Limits.MaximumReceiveBytesPerTick) return std::nullopt;
			ReceivedMessageEvent Result{
				.Connection = Id,
				.Delivery = Delivery,
				.Traffic = Traffic,
				.Order = std::move(*Order),
				.Payload = std::vector<std::byte>(Frame.begin() + AdapterEnvelopeBytes, Frame.end()),
			};
			if (!IsValidTransportEvent(Result, Limits)) return std::nullopt;
			return Result;
		}

		void DrainMessages(ConnectionId Id) {
			auto Connection = Connections.find(Id);
			if (Connection == Connections.end() || Connection->second.State != ConnectionState::Connected) return;
			std::uint32_t Count = 0;
			std::size_t Bytes = 0;
			while (Count < Limits.MaximumMessagesPerTick && Bytes < Limits.MaximumReceiveBytesPerTick) {
				SteamNetworkingMessage_t *Message = nullptr;
				const auto Result = GlobalState().Interface->ReceiveMessagesOnConnection(
					Connection->second.Handle,
					&Message,
					1
				);
				if (Result == 0) break;
				if (Result < 0 || !Message) {
					FailConnection(Id, DisconnectReason::TransportFailure, "GNS receive operation failed");
					break;
				}
				auto Event = DecodeMessage(Id, *Message);
				Message->Release();
				if (!Event) {
					FailConnection(Id, DisconnectReason::ProtocolViolation, "Malformed GNS adapter message");
					break;
				}
				const auto PayloadBytes = Event->Payload.size();
				if (PayloadBytes > Limits.MaximumReceiveBytesPerTick - Bytes || !QueueEvent(std::move(*Event))) {
					FailConnection(Id, DisconnectReason::ResourceExhaustion, "GNS receive queue limit reached");
					break;
				}
				Connection = Connections.find(Id);
				if (Connection == Connections.end()) break;
				Bytes += PayloadBytes;
				++Count;
				SaturatingAdd(*Connection->second.Statistics.MessagesReceived, 1);
				SaturatingAdd(*Connection->second.Statistics.BytesReceived, PayloadBytes);
			}
		}
	};

	bool GameNetworkingSocketsTransportConfiguration::IsValid() const {
		if (MaximumConnections == 0 || MaximumConnections > NativeMaximumGnsConnections ||
			MaximumPendingEvents == 0 || MaximumPendingEvents > NativeMaximumGnsPendingEvents ||
			MaximumPendingReceiveBytes == 0 ||
			MaximumPendingReceiveBytes > NativeMaximumGnsPendingReceiveBytes) return false;
		return MaximumConnections <= MaximumPendingEvents / 5;
	}

	GameNetworkingSocketsTransport::GameNetworkingSocketsTransport(
		GameNetworkingSocketsTransportConfiguration Configuration
	) : State(std::make_unique<Impl>(std::move(Configuration))) {}

	GameNetworkingSocketsTransport::~GameNetworkingSocketsTransport() {
		if (!State) return;
		auto &Global = GlobalState();
		std::lock_guard Lock(Global.Mutex);
		if (State->Started) (void)Stop({DisconnectReason::LocalShutdown, "Transport destroyed"});
	}

	TransportOperationResult GameNetworkingSocketsTransport::Start(
		const TransportStartConfiguration &Configuration
	) {
		auto &Global = GlobalState();
		std::lock_guard Lock(Global.Mutex);
		if (State->Started) return Operation(TransportOperationStatus::InvalidState);
		if (!State->Connections.empty() || !State->Events.empty())
			return Operation(TransportOperationStatus::InvalidState);
		if (!State->Configuration.IsValid() || !Configuration.IsValid())
			return Operation(TransportOperationStatus::MessageRejected);
		if (!Configuration.OpaqueHandshakeMaterial.empty())
			return Operation(TransportOperationStatus::MessageRejected);
		SteamNetworkingIPAddr Address;
		Address.Clear();
		if (!Address.ParseString(Configuration.Endpoint.Host.c_str()))
			return Operation(TransportOperationStatus::MessageRejected);
		Address.m_port = Configuration.Endpoint.Port;
		if (!State->AcquireGlobal()) return TerminalOperation(
			TransportOperationStatus::TransportFailure,
			{DisconnectReason::TransportFailure, "Unable to initialize GameNetworkingSockets"}
		);
		State->Role = Configuration.Role;
		State->Endpoint = Configuration.Endpoint;
		State->Limits = Configuration.AdvertisedLimits;

		std::array<SteamNetworkingConfigValue_t, 4> Options;
		Options[0].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
			reinterpret_cast<void *>(Impl::StatusChanged));
		Options[1].SetInt32(k_ESteamNetworkingConfig_SendBufferSize,
			BoundedBackendBuffer(State->Limits.MaximumQueuedReliableBytes));
		Options[2].SetInt32(k_ESteamNetworkingConfig_RecvBufferSize,
			BoundedBackendBuffer(State->Configuration.MaximumPendingReceiveBytes));
		Options[3].SetInt32(k_ESteamNetworkingConfig_RecvMaxMessageSize,
			BoundedBackendBuffer(std::min(
				State->Limits.MaximumDecodedMessageBytes + AdapterEnvelopeBytes,
				static_cast<std::size_t>(k_cbMaxSteamNetworkingSocketsMessageSizeSend)
			)));

		if (Configuration.Role == TransportRole::Server) {
			State->Listener = Global.Interface->CreateListenSocketIP(Address, static_cast<int>(Options.size()), Options.data());
			if (State->Listener == k_HSteamListenSocket_Invalid) {
				State->ReleaseGlobal();
				return TerminalOperation(
					TransportOperationStatus::TransportFailure,
					{DisconnectReason::TransportFailure, "Unable to create GNS listen socket"}
				);
			}
			Global.ListenerOwners[State->Listener] = State.get();
		} else {
			const auto Handle = Global.Interface->ConnectByIPAddress(Address, static_cast<int>(Options.size()), Options.data());
			if (Handle == k_HSteamNetConnection_Invalid) {
				State->ReleaseGlobal();
				return TerminalOperation(
					TransportOperationStatus::TransportFailure,
					{DisconnectReason::TransportFailure, "Unable to create GNS connection"}
				);
			}
			if (!State->AllocateConnection(Handle)) {
				Global.Interface->CloseConnection(Handle, ResourceExhaustionEndReason, "Connection identity exhausted", false);
				State->ReleaseGlobal();
				return TerminalOperation(
					TransportOperationStatus::ResourceExhausted,
					{DisconnectReason::ResourceExhaustion, "GNS connection identity exhausted"}
				);
			}
		}
		State->Started = true;
		return Operation(TransportOperationStatus::Succeeded);
	}

	TransportOperationResult GameNetworkingSocketsTransport::Stop(DisconnectInfo Information) {
		auto &Global = GlobalState();
		std::lock_guard Lock(Global.Mutex);
		if (!State->Started) return Operation(TransportOperationStatus::InvalidState);
		if (!Information.IsValid()) return Operation(TransportOperationStatus::MessageRejected);
		std::vector<ConnectionId> Connections;
		Connections.reserve(State->Connections.size());
		for (const auto &[Id, Record] : State->Connections) {
			(void)Record;
			Connections.push_back(Id);
		}
		for (const auto Id : Connections) State->CloseConnection(Id, Information, true);
		if (State->Listener != k_HSteamListenSocket_Invalid) {
			Global.ListenerOwners.erase(State->Listener);
			Global.Interface->CloseListenSocket(State->Listener);
			State->Listener = k_HSteamListenSocket_Invalid;
		}
		State->Started = false;
		State->ReleaseGlobal();
		return Operation(TransportOperationStatus::Succeeded);
	}

	TransportOperationResult GameNetworkingSocketsTransport::Disconnect(
		ConnectionId Connection,
		DisconnectInfo Information
	) {
		auto &Global = GlobalState();
		std::lock_guard Lock(Global.Mutex);
		if (!State->Started) return Operation(TransportOperationStatus::InvalidState);
		if (!Information.IsValid()) return Operation(TransportOperationStatus::MessageRejected);
		if (!Connection.IsValid() || !State->Connections.contains(Connection))
			return Operation(TransportOperationStatus::InvalidConnection);
		State->CloseConnection(Connection, std::move(Information), true);
		return Operation(TransportOperationStatus::Succeeded);
	}

	TransportOperationResult GameNetworkingSocketsTransport::Send(const NetworkMessageIntent &Message) {
		auto &Global = GlobalState();
		std::lock_guard Lock(Global.Mutex);
		if (!State->Started) return Operation(TransportOperationStatus::InvalidState);
		const auto Connection = State->Connections.find(Message.Destination());
		if (!Message.Destination().IsValid() || Connection == State->Connections.end())
			return Operation(TransportOperationStatus::InvalidConnection);
		if (Connection->second.State != ConnectionState::Connected)
			return Operation(TransportOperationStatus::InvalidState);
		const auto MessageLimit = Message.Delivery() == DeliveryMode::ReliableOrdered
			? State->Limits.MaximumReliableMessageBytes : State->Limits.MaximumUnreliableMessageBytes;
		if (Message.Payload().empty() || Message.Payload().size() > MessageLimit ||
			Message.Payload().size() > State->Limits.MaximumDecodedMessageBytes ||
			Message.Payload().size() > State->Limits.MaximumSendBytesPerTick ||
			!IsValidMessageOrder(Message.Order())) return Operation(TransportOperationStatus::MessageRejected);
		if (Message.Delivery() != DeliveryMode::ReliableOrdered &&
			Message.Payload().size() > BackendMaximumUnreliableFrameBytes - AdapterEnvelopeBytes)
			return Operation(TransportOperationStatus::MessageRejected);
		auto Frame = EncodeFrame(Message);
		if (!Frame) return Operation(TransportOperationStatus::MessageRejected);
		if (Message.Delivery() == DeliveryMode::ReliableOrdered) {
			SteamNetConnectionRealTimeStatus_t Status{};
			if (Global.Interface->GetConnectionRealTimeStatus(Connection->second.Handle, &Status, 0, nullptr) == k_EResultOK &&
				(Status.m_cbPendingReliable < 0 || static_cast<std::size_t>(Status.m_cbPendingReliable) >
					State->Limits.MaximumQueuedReliableBytes - std::min(
						State->Limits.MaximumQueuedReliableBytes,
						Frame->size()
					))) return Operation(TransportOperationStatus::ResourceExhausted);
		}
		const int Flags = Message.Delivery() == DeliveryMode::ReliableOrdered
			? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable;
		const auto Result = Global.Interface->SendMessageToConnection(
			Connection->second.Handle,
			Frame->data(),
			static_cast<std::uint32_t>(Frame->size()),
			Flags,
			nullptr
		);
		switch (Result) {
		case k_EResultOK:
			SaturatingAdd(*Connection->second.Statistics.MessagesSent, 1);
			SaturatingAdd(*Connection->second.Statistics.BytesSent, Message.Payload().size());
			return Operation(TransportOperationStatus::Succeeded);
		case k_EResultIgnored:
			return Operation(TransportOperationStatus::WouldBlock);
		case k_EResultLimitExceeded:
			return Operation(TransportOperationStatus::ResourceExhausted);
		case k_EResultInvalidParam:
			return Operation(TransportOperationStatus::MessageRejected);
		case k_EResultInvalidState:
			return Operation(TransportOperationStatus::InvalidState);
		case k_EResultNoConnection:
			return Operation(TransportOperationStatus::InvalidConnection);
		default:
			return Operation(TransportOperationStatus::TransportFailure);
		}
	}

	std::size_t GameNetworkingSocketsTransport::PollEvents(std::span<TransportEvent> Output) {
		auto &Global = GlobalState();
		std::lock_guard Lock(Global.Mutex);
		if (State->Started && Global.Interface) {
			Global.Interface->RunCallbacks();
			std::vector<ConnectionId> Connections;
			Connections.reserve(State->Connections.size());
			for (const auto &[Id, Record] : State->Connections) {
				(void)Record;
				Connections.push_back(Id);
			}
			for (const auto Id : Connections) State->DrainMessages(Id);
		}
		const auto Count = std::min(Output.size(), State->Events.size());
		for (std::size_t Index = 0; Index < Count; ++Index) {
			if (const auto *Message = std::get_if<ReceivedMessageEvent>(&State->Events.front()))
				State->PendingReceiveBytes -= Message->Payload.size();
			Output[Index] = std::move(State->Events.front());
			State->Events.pop_front();
		}
		return Count;
	}

	std::optional<std::size_t> GameNetworkingSocketsTransport::GetAvailableDatagramBytes(
		ConnectionId Connection
	) const {
		auto &Global = GlobalState();
		std::lock_guard Lock(Global.Mutex);
		if (!State->Started) return std::nullopt;
		const auto Iterator = State->Connections.find(Connection);
		if (!Connection.IsValid() || Iterator == State->Connections.end() ||
			Iterator->second.State != ConnectionState::Connected) return std::nullopt;
		return std::min(
			State->Limits.MaximumUnreliableMessageBytes,
			BackendMaximumUnreliableFrameBytes - AdapterEnvelopeBytes
		);
	}

	std::optional<NetworkStatistics> GameNetworkingSocketsTransport::GetStatistics(ConnectionId Connection) const {
		auto &Global = GlobalState();
		std::lock_guard Lock(Global.Mutex);
		if (!State->Started || !Global.Interface) return std::nullopt;
		const auto Iterator = State->Connections.find(Connection);
		if (!Connection.IsValid() || Iterator == State->Connections.end()) return std::nullopt;
		auto Result = Iterator->second.Statistics;
		SteamNetConnectionRealTimeStatus_t Status{};
		if (Global.Interface->GetConnectionRealTimeStatus(Iterator->second.Handle, &Status, 0, nullptr) == k_EResultOK) {
			if (Status.m_cbPendingReliable >= 0)
				Result.QueuedReliableBytes = static_cast<std::size_t>(Status.m_cbPendingReliable);
			if (Status.m_nPing >= 0)
				Result.EstimatedRoundTripTime = std::chrono::milliseconds(Status.m_nPing);
		}
		return Result.IsValid() ? std::optional<NetworkStatistics>(Result) : std::nullopt;
	}
}
