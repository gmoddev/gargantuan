#include "host/server/ServerHost.hpp"
#include "host/server/PrivateServerControl.hpp"
#include "gargantuan/filesystem/ProjectIdentity.hpp"

int main(int ArgumentCount, char *Arguments[]) {
	if (ArgumentCount > 1 && std::string_view(Arguments[1]) == "--owned-session") {
		std::filesystem::path Profile;
		std::optional<gargantuan::WorldSlot> Slot;
		int Consumed = 1;
		if (ArgumentCount > 2 && std::string_view(Arguments[2]) == "--owned-world") {
			if (ArgumentCount < 9) return 2;
			Profile = std::filesystem::u8path(Arguments[3]);
			Slot = gargantuan::WorldSlot{Arguments[4], Arguments[5], Arguments[6], Arguments[7], Arguments[8]};
			if (!Profile.is_absolute() || !gargantuan::ProjectId::Parse(Slot->SaveId) ||
				!gargantuan::ProjectId::Parse(Slot->WorldId) || Slot->Name.empty() || Slot->Name.size() > 96 ||
				Slot->Baseline.size() > 512 || Slot->Seed.size() > 128 || !gargantuan::IsPersistenceUtf8(Slot->Name) ||
				!gargantuan::IsPersistenceUtf8(Slot->Seed)) return 2;
			Consumed = 8;
		}
		auto Control = std::make_shared<gargantuan::host::PrivateServerControl>(std::move(Profile), std::move(Slot));
		if (!Control->Snapshot().AuthorityCurrent) return 2;
		for (int I = 1; I <= ArgumentCount - Consumed; ++I) Arguments[I] = Arguments[I + Consumed];
		return gargantuan::host::RunDedicatedServer(ArgumentCount - Consumed, Arguments,
			{.AdmissionProvider = Control, .ManagedLifecycle = Control});
	}
	return gargantuan::host::RunDedicatedServer(ArgumentCount, Arguments);
}
