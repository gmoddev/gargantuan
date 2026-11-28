#include "host/server/ServerHost.hpp"
#include "host/server/PrivateServerControl.hpp"
#include "gargantuan/filesystem/ProjectIdentity.hpp"

int main(int ArgumentCount, char *Arguments[]) {
	const bool Shared = ArgumentCount > 1 && std::string_view(Arguments[1]) == "--owned-multiplayer-session";
	if (Shared || (ArgumentCount > 1 && std::string_view(Arguments[1]) == "--owned-session")) {
		std::filesystem::path Profile;
		std::optional<gargantuan::WorldSlot> Slot;
		int Consumed = 1;
		unsigned Seats = 1;
		if (Shared) {
			if (ArgumentCount < 3 || std::string_view(Arguments[2]).size() != 1 || Arguments[2][0] < '2' || Arguments[2][0] > '8') return 2;
			Seats = Arguments[2][0] - '0';
			++Consumed;
		}
		if (ArgumentCount > Consumed + 1 && std::string_view(Arguments[Consumed + 1]) == "--owned-world") {
			if (ArgumentCount < Consumed + 8) return 2;
			Profile = std::filesystem::u8path(Arguments[Consumed + 2]);
			Slot = gargantuan::WorldSlot{Arguments[Consumed + 3], Arguments[Consumed + 4], Arguments[Consumed + 5], Arguments[Consumed + 6], Arguments[Consumed + 7]};
			if (!Profile.is_absolute() || !gargantuan::ProjectId::Parse(Slot->SaveId) ||
				!gargantuan::ProjectId::Parse(Slot->WorldId) || Slot->Name.empty() || Slot->Name.size() > 96 ||
				Slot->Baseline.size() > 512 || Slot->Seed.size() > 128 || !gargantuan::IsPersistenceUtf8(Slot->Name) ||
				!gargantuan::IsPersistenceUtf8(Slot->Seed)) return 2;
			Consumed += 7;
		}
		auto Control = std::make_shared<gargantuan::host::PrivateServerControl>(std::move(Profile), std::move(Slot), Seats);
		if (!Control->Snapshot().AuthorityCurrent) return 2;
		for (int I = 1; I <= ArgumentCount - Consumed; ++I) Arguments[I] = Arguments[I + Consumed];
		return gargantuan::host::RunDedicatedServer(ArgumentCount - Consumed, Arguments,
			{.AdmissionProvider = Control, .ManagedLifecycle = Control});
	}
	return gargantuan::host::RunDedicatedServer(ArgumentCount, Arguments);
}
