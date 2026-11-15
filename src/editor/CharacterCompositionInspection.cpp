#include "gargantuan/editor/CharacterCompositionInspection.hpp"

#include "gargantuan/classes/Attachment.hpp"
#include "gargantuan/classes/Animator.hpp"
#include "gargantuan/classes/BasePart.hpp"
#include "gargantuan/classes/Character.hpp"
#include "gargantuan/classes/MeshPart.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/services/AssetService.hpp"

#include <algorithm>
#include <mutex>
#include <utility>

namespace gargantuan {
	namespace {
		constexpr CharacterCompositionLimits HardLimits{4096, 512, 1024, 512};
		constexpr std::size_t MaximumAttachmentDepth = 32;
		constexpr std::size_t MaximumReferenceBytes = 256;

		ObjectId ReadPublishedId(const std::shared_ptr<Instance> &Value) {
			if (!Value) return {};
			std::scoped_lock Lock(Value->IdentityMutex);
			const auto Id = Value->Id;
			return Id.IsValid() && ObjectRegistry::Get().Lookup(Id).get() == Value.get() ? Id : ObjectId{};
		}

		std::string BoundedReference(std::string_view Value) {
			return std::string(Value.substr(0, MaximumReferenceBytes));
		}

		bool Available(const AssetRecord &Record) {
			return Record.Asset && (Record.State == AssetState::Ready || Record.State == AssetState::Stale ||
				(Record.State == AssetState::Importing && Record.ContentRevision > 0 && Record.ContentId.IsValid()));
		}

		CharacterCompositionLimits Clamp(CharacterCompositionLimits Value) {
			Value.MaximumObjects = std::clamp(Value.MaximumObjects, std::size_t{1}, HardLimits.MaximumObjects);
			Value.MaximumMeshes = std::clamp(Value.MaximumMeshes, std::size_t{1}, HardLimits.MaximumMeshes);
			Value.MaximumAttachments = std::clamp(Value.MaximumAttachments, std::size_t{1}, HardLimits.MaximumAttachments);
			Value.MaximumIssues = std::clamp(Value.MaximumIssues, std::size_t{1}, HardLimits.MaximumIssues);
			return Value;
		}

		struct Collector {
			CharacterCompositionResult &Result;
			CharacterCompositionLimits Limits;

			void Limited(ObjectId Object) {
				if (!Result.Complete) return;
				Result.Complete = false;
				Result.Valid = false;
				const CharacterCompositionIssue Issue{CharacterCompositionIssueCode::LimitReached,
					CharacterCompositionSeverity::Error, Object, {}};
				if (Result.Issues.size() < Limits.MaximumIssues) Result.Issues.push_back(Issue);
				else Result.Issues.back() = Issue;
			}

			void Add(CharacterCompositionIssueCode Code, CharacterCompositionSeverity Severity,
				ObjectId Object = {}, std::string_view Reference = {}) {
				if (!Result.Complete) return;
				if (Result.Issues.size() >= Limits.MaximumIssues) { Limited(Object); return; }
				Result.Issues.push_back({Code, Severity, Object, BoundedReference(Reference)});
				if (Severity == CharacterCompositionSeverity::Error) Result.Valid = false;
			}
		};

		std::optional<AssetRecord> InspectReference(
			Collector &Output, const std::shared_ptr<AssetService> &Assets,
			std::string_view Reference, AssetKind Kind, ObjectId Object,
			CharacterCompositionAsset &Destination, bool Material
		) {
			Destination.Reference = BoundedReference(Reference);
			if (Reference.empty()) {
				Output.Add(Material ? CharacterCompositionIssueCode::MissingMaterialReference
					: CharacterCompositionIssueCode::MissingMeshReference,
					CharacterCompositionSeverity::Info, Object);
				return std::nullopt;
			}
			const auto Parsed = AssetReference::Parse(Reference);
			if (!Parsed) {
				Output.Add(Material ? CharacterCompositionIssueCode::InvalidMaterialReference
					: CharacterCompositionIssueCode::InvalidMeshReference,
					CharacterCompositionSeverity::Warning, Object, Reference);
				return std::nullopt;
			}
			Destination.Id = Parsed->ProjectAsset;
			if (!Assets) {
				Output.Add(CharacterCompositionIssueCode::MissingAssetService,
					CharacterCompositionSeverity::Warning, Object, Reference);
				return std::nullopt;
			}
			auto Record = Assets->GetAsset(Reference);
			if (!Record) {
				Output.Add(Material ? CharacterCompositionIssueCode::MaterialUnavailable
					: CharacterCompositionIssueCode::MeshUnavailable,
					CharacterCompositionSeverity::Warning, Object, Reference);
				return std::nullopt;
			}
			Destination.State = Record->State;
			Destination.ContentRevision = Record->ContentRevision;
			Destination.ContentId = Record->ContentId;
			if (Record->Kind != Kind) {
				Output.Add(Material ? CharacterCompositionIssueCode::MaterialWrongKind
					: CharacterCompositionIssueCode::MeshWrongKind,
					CharacterCompositionSeverity::Warning, Object, Reference);
				return std::nullopt;
			}
			if (!Available(*Record)) {
				Output.Add(Material ? CharacterCompositionIssueCode::MaterialUnavailable
					: CharacterCompositionIssueCode::MeshUnavailable,
					CharacterCompositionSeverity::Warning, Object, Reference);
				return std::nullopt;
			}
			Destination.Available = true;
			return Record;
		}

		std::shared_ptr<MeshPart> OwningMesh(const std::shared_ptr<Instance> &Value, bool &DepthLimited) {
			auto Parent = Value->GetParent();
			for (std::size_t Depth = 0; Parent && *Parent && Depth < MaximumAttachmentDepth; ++Depth) {
				if (auto Part = std::dynamic_pointer_cast<BasePart>(*Parent))
					return std::dynamic_pointer_cast<MeshPart>(Part);
				Parent = (*Parent)->GetParent();
			}
			DepthLimited = Parent && *Parent;
			return nullptr;
		}
	}

	CharacterCompositionResult InspectCharacterComposition(
		const std::shared_ptr<Character> &CharacterValue,
		const std::shared_ptr<AssetService> &Assets,
		CharacterCompositionLimits Limits
	) {
		CharacterCompositionResult Result;
		Collector Output{Result, Clamp(Limits)};
		if (GetCurrentExecutionDomain() != ExecutionDomain::Main) {
			Output.Add(CharacterCompositionIssueCode::WrongExecutionDomain, CharacterCompositionSeverity::Error);
			Result.Complete = false;
			return Result;
		}
		if (!CharacterValue || CharacterValue->GetDestroyed() || CharacterValue->IsDestroying()) {
			Output.Add(CharacterCompositionIssueCode::InvalidCharacter, CharacterCompositionSeverity::Error);
			Result.Complete = false;
			return Result;
		}
		Result.Character = ReadPublishedId(CharacterValue);
		if (!Result.Character.IsValid()) {
			Output.Add(CharacterCompositionIssueCode::IdentityUnavailable, CharacterCompositionSeverity::Error);
			Result.Complete = false;
			return Result;
		}

		const auto RootValue = CharacterValue->GetRootPart();
		const auto Root = RootValue ? *RootValue : nullptr;
		if (!Root) Output.Add(CharacterCompositionIssueCode::MissingRootPart,
			CharacterCompositionSeverity::Info, Result.Character);
		else {
			const auto RootId = ReadPublishedId(Root);
			if (!RootId.IsValid()) {
				Output.Add(CharacterCompositionIssueCode::IdentityUnavailable,
					CharacterCompositionSeverity::Error, Result.Character);
				Result.Complete = false;
				return Result;
			}
			Result.RootPart = RootId;
			if (Root->GetCanCollide()) Output.Add(CharacterCompositionIssueCode::RootPartCollidesIndependently,
				CharacterCompositionSeverity::Warning, RootId);
		}

		std::vector<std::shared_ptr<Instance>> Pending{CharacterValue};
		Pending.reserve(Output.Limits.MaximumObjects);
		bool FoundRoot = false;
		while (!Pending.empty() && Result.Complete) {
			auto Current = std::move(Pending.back());
			Pending.pop_back();
			if (!Current || Current->GetDestroyed() || Current->IsDestroying()) continue;
			if (Result.ObjectsVisited >= Output.Limits.MaximumObjects) {
				Output.Limited(Result.Character);
				break;
			}
			++Result.ObjectsVisited;
			const auto Object = ReadPublishedId(Current);
			if (!Object.IsValid()) {
				Output.Add(CharacterCompositionIssueCode::IdentityUnavailable,
					CharacterCompositionSeverity::Error, Result.Character);
				Result.Complete = false;
				break;
			}
			if (Pending.size() + Current->Children.size() > Output.Limits.MaximumObjects) {
				Output.Limited(Object);
				break;
			}
			if (Root && Current.get() == Root.get()) FoundRoot = true;
			if (auto Mesh = std::dynamic_pointer_cast<MeshPart>(Current)) {
				if (Result.Meshes.size() >= Output.Limits.MaximumMeshes) { Output.Limited(Object); break; }
				CharacterCompositionMesh Entry;
				Entry.Object = Object;
				Entry.IsRootPart = Root && Mesh.get() == Root.get();
				Entry.Anchored = Mesh->GetAnchored();
				Entry.CanCollide = Mesh->GetCanCollide();
				Entry.StaticMeshCollision = Mesh->GetCollisionMode() == Enums::MeshCollisionMode::StaticMesh;
				const auto MeshReference = Mesh->GetMesh();
				auto MeshRecord = InspectReference(Output, Assets, MeshReference, AssetKind::Mesh,
					Object, Entry.Mesh, false);
				const ImportedMesh *CanonicalMesh = MeshRecord
					? std::get_if<ImportedMesh>(MeshRecord->Asset.get()) : nullptr;
				if (CanonicalMesh && CanonicalMesh->Skeleton && CanonicalMesh->Skeleton->Joints &&
					CanonicalMesh->SkinInfluences) {
					Entry.SkeletonCompatibilityId = CanonicalMesh->Skeleton->CompatibilityId;
					Entry.JointCount = CanonicalMesh->Skeleton->Joints->size();
				}
				if (const auto MaterialReference = Mesh->GetMaterial(); !MaterialReference.empty()) {
					Entry.Material.emplace();
					(void)InspectReference(Output, Assets, MaterialReference, AssetKind::Material,
						Object, *Entry.Material, true);
				}
				for (const auto &Child : Mesh->Children)
					if (Child && !Child->GetDestroyed() && !Child->IsDestroying() &&
						std::dynamic_pointer_cast<Animator>(Child)) ++Entry.AnimatorCount;
				if (Entry.AnimatorCount > 1) Output.Add(CharacterCompositionIssueCode::MultipleAnimators,
					CharacterCompositionSeverity::Warning, Object);
				if (Entry.AnimatorCount && !Entry.SkeletonCompatibilityId.IsValid())
					Output.Add(CharacterCompositionIssueCode::AnimatorWithoutSkinnedMesh,
						CharacterCompositionSeverity::Warning, Object);
				if (Entry.AnimatorCount && Entry.SkeletonCompatibilityId.IsValid())
					Output.Add(CharacterCompositionIssueCode::ClipCompatibilityNotAuthored,
						CharacterCompositionSeverity::Info, Object, MeshReference);
				if (!Entry.IsRootPart) Output.Add(CharacterCompositionIssueCode::VisualPartNotRootSynchronized,
					CharacterCompositionSeverity::Warning, Object);
				if (auto Parent = Mesh->GetParent(); Parent && *Parent &&
					std::dynamic_pointer_cast<Attachment>(*Parent))
					Output.Add(CharacterCompositionIssueCode::VisualPartUnderAttachmentUnsupported,
						CharacterCompositionSeverity::Warning, Object);
				if (Entry.StaticMeshCollision && Entry.CanCollide) {
					const auto Size = Mesh->GetSize();
					const bool ScaleSupported = Size.x >= 0.01f && Size.y >= 0.01f && Size.z >= 0.01f;
					const auto Collision = Assets && Entry.Anchored && ScaleSupported && CanonicalMesh &&
						!CanonicalMesh->Skeleton ? Assets->ResolveCollisionForMesh(MeshReference) : std::nullopt;
					if (!Entry.Anchored || !ScaleSupported || !Collision)
						Output.Add(CharacterCompositionIssueCode::StaticMeshCollisionFallsBackToBox,
							CharacterCompositionSeverity::Warning, Object, MeshReference);
				}
				Result.Meshes.push_back(std::move(Entry));
			} else if (auto AttachmentValue = std::dynamic_pointer_cast<Attachment>(Current)) {
				if (Result.Attachments.size() >= Output.Limits.MaximumAttachments) { Output.Limited(Object); break; }
				CharacterCompositionAttachment Entry;
				Entry.Object = Object;
				Entry.JointPath = AttachmentValue->GetJointPath();
				if (!Entry.JointPath.empty()) {
					bool DepthLimited = false;
					auto Owner = OwningMesh(AttachmentValue, DepthLimited);
					if (DepthLimited) { Output.Limited(Object); break; }
					if (!Owner) Output.Add(CharacterCompositionIssueCode::JointPathWithoutMesh,
						CharacterCompositionSeverity::Warning, Object, Entry.JointPath);
					else {
						Entry.OwnerMesh = ReadPublishedId(Owner);
						const auto Record = Assets ? Assets->GetAsset(Owner->GetMesh()) : std::nullopt;
						const auto *Imported = Record && Record->Kind == AssetKind::Mesh && Available(*Record)
							? std::get_if<ImportedMesh>(Record->Asset.get()) : nullptr;
						if (!Imported || !Imported->Skeleton || !Imported->Skeleton->Joints)
							Output.Add(CharacterCompositionIssueCode::JointPathWithoutSkeleton,
								CharacterCompositionSeverity::Warning, Object, Entry.JointPath);
						else {
							Entry.JointPathExists = std::ranges::any_of(*Imported->Skeleton->Joints,
								[&](const ImportedSkeletonJoint &Joint) { return Joint.Path == Entry.JointPath; });
							if (!Entry.JointPathExists) Output.Add(CharacterCompositionIssueCode::JointPathUnavailable,
									CharacterCompositionSeverity::Warning, Object, Entry.JointPath);
						}
					}
				}
				Result.Attachments.push_back(std::move(Entry));
			} else if (std::dynamic_pointer_cast<Animator>(Current)) {
				const auto Parent = Current->GetParent();
				if (!Parent || !*Parent || !std::dynamic_pointer_cast<MeshPart>(*Parent))
					Output.Add(CharacterCompositionIssueCode::AnimatorWithoutMeshParent,
						CharacterCompositionSeverity::Warning, Object);
			}
			if (!Result.Complete) break;
			for (auto It = Current->Children.rbegin(); It != Current->Children.rend(); ++It)
				Pending.push_back(*It);
		}
		if (Result.Complete && Root && !FoundRoot) Output.Add(CharacterCompositionIssueCode::RootPartOutsideCharacter,
			CharacterCompositionSeverity::Error, *Result.RootPart);
		return Result;
	}
}
