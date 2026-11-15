#include "gargantuan/classes/Attachment.hpp"
#include "gargantuan/classes/Animator.hpp"
#include "gargantuan/classes/Character.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/MeshPart.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/editor/CharacterCompositionInspection.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/SourceMount.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
	using namespace gargantuan;

	void Check(bool Condition, const char *Message) {
		if (!Condition) throw std::runtime_error(Message);
	}

	bool Has(const CharacterCompositionResult &Result, CharacterCompositionIssueCode Code) {
		return std::ranges::any_of(Result.Issues,
			[&](const CharacterCompositionIssue &Issue) { return Issue.Code == Code; });
	}

	std::shared_ptr<Character> AddCharacter(const std::shared_ptr<Workspace> &WorkspaceValue) {
		auto Value = std::make_shared<Character>();
		Value->SetParent(WorkspaceValue);
		(void)Value->GetObjectId();
		return Value;
	}
}

int main() {
	using namespace gargantuan;
	const auto Root = std::filesystem::temp_directory_path() /
		("gargantuan-character-composition-" +
			std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	struct Cleanup {
		std::filesystem::path Path;
		~Cleanup() { std::error_code Ignored; std::filesystem::remove_all(Path, Ignored); }
	} CleanupValue{Root};
	try {
		BootstrapNativeRuntimeSchema();
		auto World = std::make_shared<DataModel>();
		auto WorkspaceValue = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
		auto Assets = std::dynamic_pointer_cast<AssetService>(World->GetService("AssetService"));
		Check(WorkspaceValue && Assets, "canonical world services exist");

		// A Character can be a valid logical root without visual content or AssetService access.
		auto Logical = AddCharacter(WorkspaceValue);
		auto Empty = InspectCharacterComposition(Logical, nullptr);
		Check(Empty.Complete && Empty.Valid && !Empty.RootPart && Empty.Meshes.empty() &&
			Has(Empty, CharacterCompositionIssueCode::MissingRootPart) &&
			!Has(Empty, CharacterCompositionIssueCode::MissingAssetService),
			"nullable RootPart and no asset references remain valid");
		auto PlainRoot = std::make_shared<Part>();
		PlainRoot->SetAnchored(true);
		PlainRoot->SetCanCollide(false);
		PlainRoot->SetParent(Logical);
		Logical->SetRootPart(PlainRoot);
		auto Plain = InspectCharacterComposition(Logical, nullptr);
		Check(Plain.Complete && Plain.Valid && Plain.RootPart == PlainRoot->GetObjectId() &&
			Plain.Meshes.empty() && !Has(Plain, CharacterCompositionIssueCode::MissingAssetService),
			"Part-only assembly does not require AssetService");

		std::filesystem::create_directories(Root / "assets");
		std::filesystem::copy_file(GARGANTUAN_ANIMATION_PREVIEW_FIXTURE, Root / "assets" / "rig.gltf");
		DiskFilesystem Filesystem(Root);
		SourceMount Mount(Filesystem);
		World->Root = Root;
		World->Filesystem = &Filesystem;
		const auto Import = Assets->ImportProjectAsset(Mount, "assets/rig.gltf", AssetKind::Mesh, "Custom Rig");
		Check(Import.Ok, "canonical skinned fixture imports");
		std::string MeshReference, ClipReference;
		for (const auto &Record : Import.Records) {
			if (Record.Kind == AssetKind::Mesh) MeshReference = Record.Reference.Value;
			if (Record.Kind == AssetKind::Animation) ClipReference = Record.Reference.Value;
		}
		Check(!MeshReference.empty() && !ClipReference.empty(), "fixture includes mesh and clip");

		auto CharacterValue = AddCharacter(WorkspaceValue);
		auto Rig = std::make_shared<MeshPart>();
		Rig->SetAnchored(true);
		Rig->SetCanCollide(false);
		Rig->SetMesh(MeshReference);
		Rig->SetParent(CharacterValue);
		CharacterValue->SetRootPart(Rig);
		auto AnimatorValue = std::make_shared<Animator>();
		AnimatorValue->SetParent(Rig);
		auto Socket = std::make_shared<Attachment>();
		Socket->SetJointPath("BeaconRoot/BeaconTip");
		Socket->SetParent(Rig);
		const auto Revision = World->GetAuthoritativeRevision();
		const auto JournalSequence = ChangeJournal::Get().GetRetentionWindow(World->GetObjectId()).NextSequence;
		const auto RigFrame = Rig->GetCFrame();
		auto Valid = InspectCharacterComposition(CharacterValue, Assets);
		Check(Valid.Complete && Valid.Valid && Valid.RootPart == Rig->GetObjectId() &&
			Valid.Meshes.size() == 1 && Valid.Meshes[0].Mesh.Reference == MeshReference &&
			Valid.Meshes[0].Mesh.ContentRevision > 0 && Valid.Meshes[0].JointCount == 2 &&
			Valid.Meshes[0].SkeletonCompatibilityId.IsValid() && Valid.Meshes[0].AnimatorCount == 1 &&
			Valid.Attachments.size() == 1 && Valid.Attachments[0].JointPathExists &&
			Has(Valid, CharacterCompositionIssueCode::ClipCompatibilityNotAuthored),
			"nonhumanoid skinned assembly returns bounded canonical identities and socket compatibility");
		Check(World->GetAuthoritativeRevision() == Revision &&
			ChangeJournal::Get().GetRetentionWindow(World->GetObjectId()).NextSequence == JournalSequence &&
			Rig->GetCFrame().FuzzyEq(RigFrame),
			"inspection does not mutate transforms, revision, or journal");

		Socket->SetJointPath("BeaconRoot/Missing");
		auto BadSocket = InspectCharacterComposition(CharacterValue, Assets);
		Check(BadSocket.Complete && BadSocket.Valid && !BadSocket.Attachments[0].JointPathExists &&
			Has(BadSocket, CharacterCompositionIssueCode::JointPathUnavailable),
			"unavailable joint is diagnosed without changing authored binding");
		Socket->SetJointPath("BeaconRoot/BeaconTip");
		Rig->SetMesh(ClipReference);
		auto WrongKind = InspectCharacterComposition(CharacterValue, Assets);
		Check(WrongKind.Complete && WrongKind.Valid &&
			Has(WrongKind, CharacterCompositionIssueCode::MeshWrongKind) &&
			Has(WrongKind, CharacterCompositionIssueCode::AnimatorWithoutSkinnedMesh),
			"wrong-kind asset reference and unusable Animator are readiness diagnostics");
		Rig->SetMesh("asset://00000000000000000000000000000002");
		auto MissingMesh = InspectCharacterComposition(CharacterValue, Assets);
		Check(MissingMesh.Complete && MissingMesh.Valid &&
			Has(MissingMesh, CharacterCompositionIssueCode::MeshUnavailable),
			"missing mesh is reported without inventing visual content");
		Rig->SetMesh(MeshReference);
		Rig->SetMaterial("asset://00000000000000000000000000000001");
		auto MissingMaterial = InspectCharacterComposition(CharacterValue, Assets);
		Check(MissingMaterial.Complete && Has(MissingMaterial, CharacterCompositionIssueCode::MaterialUnavailable),
			"missing material is explicit without inventing a replacement asset");
		Rig->SetMaterial("");
		Rig->SetCollisionMode(Enums::MeshCollisionMode::StaticMesh);
		Rig->SetCanCollide(true);
		auto Collision = InspectCharacterComposition(CharacterValue, Assets);
		Check(Collision.Complete && Has(Collision,
			CharacterCompositionIssueCode::StaticMeshCollisionFallsBackToBox),
			"skinned static mesh collision reports actual Box fallback");
		Rig->SetCanCollide(false);

		auto Accessory = std::make_shared<MeshPart>();
		Accessory->SetAnchored(true);
		Accessory->SetCanCollide(false);
		Accessory->SetMesh(MeshReference);
		Accessory->SetParent(Socket);
		auto UnsupportedFollow = InspectCharacterComposition(CharacterValue, Assets);
		Check(UnsupportedFollow.Complete && Has(UnsupportedFollow,
			CharacterCompositionIssueCode::VisualPartUnderAttachmentUnsupported) &&
			Has(UnsupportedFollow, CharacterCompositionIssueCode::VisualPartNotRootSynchronized),
			"visual child under semantic socket is not represented as a supported follower");
		auto MisparentedAnimator = std::make_shared<Animator>();
		MisparentedAnimator->SetParent(CharacterValue);
		auto WrongParent = InspectCharacterComposition(CharacterValue, Assets);
		Check(WrongParent.Complete && WrongParent.Valid &&
			Has(WrongParent, CharacterCompositionIssueCode::AnimatorWithoutMeshParent),
			"Animator outside direct MeshPart parent is diagnosed without policy mutation");

		auto Limited = InspectCharacterComposition(CharacterValue, Assets, {2, 1, 1, 1});
		Check(!Limited.Complete && !Limited.Valid && Limited.Issues.size() == 1 &&
			Has(Limited, CharacterCompositionIssueCode::LimitReached),
			"traversal and issue ceilings produce explicit incomplete result");
		Rig->SetParent(WorkspaceValue);
		auto Outside = InspectCharacterComposition(CharacterValue, Assets);
		Check(Outside.Complete && !Outside.Valid &&
			Has(Outside, CharacterCompositionIssueCode::RootPartOutsideCharacter),
			"RootPart reparented outside Character is detected by bounded traversal");

		auto Detached = std::make_shared<Character>();
		const auto BeforeId = Detached->Id;
		auto Unpublished = InspectCharacterComposition(Detached, nullptr);
		Check(!Unpublished.Complete && !Unpublished.Valid && !Detached->Id.IsValid() &&
			Detached->Id == BeforeId && Has(Unpublished, CharacterCompositionIssueCode::IdentityUnavailable),
			"inspection does not publish detached ObjectIds");
		CharacterValue->Destroy();
		auto Destroyed = InspectCharacterComposition(CharacterValue, Assets);
		Check(!Destroyed.Complete && !Destroyed.Valid &&
			Has(Destroyed, CharacterCompositionIssueCode::InvalidCharacter),
			"destroyed Character is rejected without graph access");
		World->Filesystem = nullptr;
		std::cout << "[Creator:Composition] PASS\n";
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Creator:Composition] FAIL " << Error.what() << '\n';
		return 1;
	}
}
