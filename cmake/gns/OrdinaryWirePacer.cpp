#include "OrdinaryWirePacer.hpp"
#include "../../include/gargantuan/network/ReliableServiceProfile.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace SteamNetworkingSocketsLib {

std::uint64_t GargantuanOrdinaryWireRate() noexcept {
	const gargantuan::network::PooledReliableServiceProfile Profile;
	// Conservative while ANY grant is running; do not invent per-peer shares
	// or promise to recycle the other three grants' unused structural budget.
	return Profile.BackendCap - (Profile.StructuralPool + Profile.RequiredTransportReserve);
}

GargantuanOrdinaryWirePacer &GargantuanSharedOrdinaryWirePacer(std::uint64_t MaximumNativeBytes) {
	static GargantuanOrdinaryWirePacer Budget(GargantuanOrdinaryWireRate(), MaximumNativeBytes);
	return Budget;
}

GargantuanOrdinaryWirePacer::GargantuanOrdinaryWirePacer(std::uint64_t Rate, std::uint64_t Maximum) :
	WireRate(Rate), MaximumNativeBytes(Maximum), MaximumWireBytes(Maximum + 48),
	CreditCap(MaximumWireBytes * Microseconds), Credit(CreditCap) {
	if (!Rate || !Maximum || Maximum > (std::numeric_limits<std::uint64_t>::max() / Microseconds) - 48)
		throw std::invalid_argument("[Gns:OrdinaryPacer] Invalid fixed wire budget");
}

void GargantuanOrdinaryWirePacer::Refill(std::int64_t Now) noexcept {
	if (Now <= LastRefill) return; // Concurrent/stale native timestamps never mint credit.
	if (!LastRefill) { LastRefill = Now; return; }
	const auto Elapsed = static_cast<std::uint64_t>(Now - LastRefill);
	const auto Missing = CreditCap - Credit;
	const auto FillTime = Missing / WireRate + (Missing % WireRate != 0);
	Credit = Elapsed >= FillTime ? CreditCap : Credit + Elapsed * WireRate;
	LastRefill = Now;
}

std::int64_t GargantuanOrdinaryWirePacer::EligibleAt(std::int64_t Now) {
	if (Now <= 0) return std::numeric_limits<std::int64_t>::max();
	std::lock_guard<std::mutex> Lock(Mutex);
	Refill(Now);
	const auto Missing = CreditCap - Credit;
	const auto Wait = Missing / WireRate + (Missing % WireRate != 0);
	const auto Base = std::max(Now, LastRefill);
	if (Base < 0 || Wait > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() - Base))
		return std::numeric_limits<std::int64_t>::max();
	return Base + static_cast<std::int64_t>(Wait);
}

bool GargantuanOrdinaryWirePacer::Permit::Reserve(GargantuanOrdinaryWirePacer &Budget, std::int64_t Now) {
	if (Owner || Now <= 0) return false;
	std::lock_guard<std::mutex> Lock(Budget.Mutex);
	Budget.Refill(Now);
	if (Budget.Credit != Budget.CreditCap) return false;
	Budget.Credit = 0;
	Owner = &Budget;
	return true;
}

void GargantuanOrdinaryWirePacer::Refund(std::uint64_t WireBytes) noexcept {
	std::lock_guard<std::mutex> Lock(Mutex);
	const auto RefundCredit = WireBytes * Microseconds;
	Credit += std::min(RefundCredit, CreditCap - Credit);
}

bool GargantuanOrdinaryWirePacer::Permit::Complete(int NativeBytes, bool HasData) noexcept {
	if (!Owner) return true; // Exempt native packet: existing path unchanged.
	auto *Budget = Owner;
	Owner = nullptr;
	if (!HasData || NativeBytes <= 0) { Budget->Refund(Budget->MaximumWireBytes); return true; }
	if (static_cast<std::uint64_t>(NativeBytes) > Budget->MaximumNativeBytes) return false;
	Budget->Refund(Budget->MaximumNativeBytes - static_cast<std::uint64_t>(NativeBytes));
	return true; // Retained debit is exactly successful native bytes + IPv6/UDP 48.
}

void GargantuanOrdinaryWirePacer::Permit::Cancel() noexcept {
	if (!Owner) return;
	auto *Budget = Owner;
	Owner = nullptr;
	Budget->Refund(Budget->MaximumWireBytes);
}

} // namespace SteamNetworkingSocketsLib
