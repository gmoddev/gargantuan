#pragma once

#include "../../network/GameSessionTestAccess.hpp"
#include "gargantuan/content/ContentAvailability.hpp"
#include "gargantuan/network/GameSession.hpp"
#include "gargantuan/network/ReliableServiceProfile.hpp"
#include "FarmRemoteOwnershipEvidence.hpp"
#include <cstdint>
#include <ostream>
#include <string_view>

namespace gargantuan::host::detail {
// The existing qualified GameSession benchmark's post-Stop invariants, sampled
// before the objects are destroyed. Process exit cannot substitute for these.
struct FarmLifecycleEvidence {
	bool SessionMeasured = false;
	bool SessionTerminal = false;
	std::uint64_t Connections = 0, JournalReaders = 0, AdmissionOwners = 0, AdmissionBytes = 0;
	network::ReliableByteAdmissionMetrics Admission;
	network::RemoteMetrics Remotes;
	bool ContentPresent = false;
	ContentAvailabilityMetrics Content;

	[[nodiscard]] bool Valid() const {
		return SessionMeasured && SessionTerminal && ValidRemoteOwnership(Remotes) && !Connections && !JournalReaders && !AdmissionOwners &&
			Admission.AcceptedBytes <= Admission.ReservedBytes &&
			Admission.RolledBackBytes == Admission.ReservedBytes - Admission.AcceptedBytes &&
			!Admission.OutstandingBytes && !Admission.ActiveDrainGrants &&
			!Content.RequestedUnits && !Content.AcquiringUnits && !Content.PreparedUnits &&
			!Content.ReservedCompletionPayloadBytes && !Content.CompletedPayloadBytes && !Content.DecodedDocumentBytes;
	}

	void ObserveSession(const network::GameSession &Session) {
		SessionMeasured = true;
		const auto Status = Session.GetStatus();
		SessionTerminal = Status == network::GameSessionStatus::Closed || Status == network::GameSessionStatus::Failed;
		Connections = network::detail::GameSessionTestAccess::GetConnections(Session).size();
		JournalReaders = network::detail::GameSessionTestAccess::GetJournalRequirements(Session).size();
		const auto Metrics = Session.GetMetrics();
		AdmissionOwners = Metrics.ReliableAdmissionPeerStates;
		AdmissionBytes = Metrics.ReliableAdmissionLogicalBytes;
		Admission = Metrics.ReliableAdmission;
		Remotes = network::detail::GameSessionTestAccess::GetRemoteMetrics(Session);
	}
	void ObserveContent(const ContentAvailabilityService *Service) {
		ContentPresent = Service != nullptr;
		if (Service) Content = Service->GetMetrics();
	}
	void Write(std::ostream &Out, std::string_view Run, std::string_view Role, int Slot, std::uint64_t Nonce) const {
		Out << "[Qualification:Lifecycle] event=post_stop contract=logical_stop_v1 run_id=" << Run
			<< " role=" << Role << " slot=" << Slot << " nonce=" << Nonce
			<< " session_measured=" << SessionMeasured << " session_terminal=" << SessionTerminal
			<< " connections=" << Connections << " journal_readers=" << JournalReaders
			<< " admission_owners=" << AdmissionOwners << " admission_bytes=" << AdmissionBytes
			<< " reserved=" << Admission.ReservedBytes << " accepted=" << Admission.AcceptedBytes
			<< " rolled_back=" << Admission.RolledBackBytes << " retired=" << Admission.VerifiedAttributedRetirement
			<< " terminal_release=" << Admission.TerminalReleasedBytes << " outstanding=" << Admission.OutstandingBytes
			<< " active_grants=" << Admission.ActiveDrainGrants << " content_present=" << ContentPresent
			<< " requested=" << Content.RequestedUnits << " acquiring=" << Content.AcquiringUnits
			<< " prepared=" << Content.PreparedUnits << " completion_reserved=" << Content.ReservedCompletionPayloadBytes
			<< " completed_bytes=" << Content.CompletedPayloadBytes << " decoded_bytes=" << Content.DecodedDocumentBytes
			<< " resident=" << Content.ResidentUnits << " cached_bytes=" << Content.CachedPayloadBytes
			<< " records=" << Content.RetainedRecordCount << " resident_objects=" << Content.ResidentPackageObjects
			<< " valid=" << Valid() << '\n';
		WriteFarmRemoteOwnership(Out, Run, Role, Slot, Nonce, Remotes);
	}
};
}
