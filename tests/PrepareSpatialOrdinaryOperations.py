# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Insert event counters into exact stock/optimized native sources for a private diagnostic build."""
import argparse
from pathlib import Path


Units = {
    "PlaySession": "src/editor/PlaySession.cpp",
    "Instance": "src/classes/Instance.cpp",
    "RuntimeSchemaLifecycle": "src/reflection/RuntimeSchemaLifecycle.cpp",
    "Character": "src/classes/Character.cpp",
    "SpatialDomainMember": "src/runtime/SpatialDomainMember.cpp",
    "WorldRoot": "src/classes/WorldRoot.cpp",
    "RenderPublisher": "src/render/RenderPublisher.cpp",
    "InteractionService": "src/services/InteractionService.cpp",
}


def Replace(Text, Before, After):
    if Text.count(Before) != 1:
        raise ValueError(f"Expected one reviewed insertion, found {Text.count(Before)} for {Before!r}")
    return Text.replace(Before, After, 1)


def Call(Name):
    return f"OrdinaryOp::Record(OrdinaryOp::Operation::{Name})"


def Prefix(Text, Expression, Name):
    return Replace(Text, Expression, "(" + Call(Name) + ", " + Expression + ")")


def After(Text, Line, Name):
    return Replace(Text, Line, Line + "\n\t\t" + Call(Name) + ";")


def Section(Text, Begin, End, Action):
    StartAt = Text.index(Begin)
    StopAt = Text.index(End, StartAt + len(Begin))
    return Text[:StartAt] + Action(Text[StartAt:StopAt]) + Text[StopAt:]


def World(Text):
    # The two ordered-readback loops have different obligations during a recount.
    def Readback(Part):
        Part = After(Part, "\n\t\t\tfor (const auto &Part : Parts) {", "OrderedReadbackPartVisit")
        return After(Part, "\n\t\tfor (const auto &Part : Parts) {", "RecountReadbackPartVisit")

    def Assets(Part):
        Part = After(Part, "void WorldRoot::ObserveCollisionAssetChanges() {", "ObserveCollisionAssets")
        Part = Prefix(Part, 'DataModelValue->GetService("AssetService")', "CollisionAssetServiceLookup")
        return Prefix(Part, "Assets->ReadChanges(CollisionAssetChangeSequence)", "CollisionAssetChangeRead")

    def Impulses(Part):
        Part = After(Part, "for (const auto &Part : Parts) {", "ImpulsePartVisit")
        return Prefix(Part, "Realm->GetSpatialTransferProtection()", "ImpulseProtectionLookup")

    Text = Section(Text, "void WorldRoot::ReadbackOrderedParts(", "void WorldRoot::ReadbackDynamicBodies(", Readback)
    Text = Section(Text, "void WorldRoot::ObserveCollisionAssetChanges(", "void WorldRoot::ApplyPendingPhysicsChanges(", Assets)
    Text = Section(Text, "void WorldRoot::ApplyPendingImpulses(", "\n\t}", Impulses)
    Loop = "for (const Instance *Current = &Member; Current; Current = Current->ParentReference.lock().get()) {"
    Text = After(Text, Loop, "EffectiveWorldNode")
    Text = Replace(Text, Loop, Prefix(Loop, "Current->ParentReference.lock()", "EffectiveWorldParentLock"))
    return Prefix(Text, "TerrainRuntimeValue->Poll()", "TerrainWorkPoll")


def Render(Text):
    # The ordinary incremental path has a unique leading marker; the full resync
    # path retains its existing semantics and is excluded from these categories.
    Start = "Result->TextureRemoves = PendingTextureRemoves;\n\t\t"
    Begin = Text.rindex(Start) + len(Start)
    Tail = Text[Begin:]
    Terrain = "auto Terrain = PrepareTerrain(*Result);"
    Publish = ("if (Grids) PublishedGrids = std::move(*Grids);" if "if (Grids) PublishedGrids =" in Tail
               else "PublishedGrids = std::move(Grids);")
    if " : GridMeshes{};" in Tail:
        Tail = Replace(Tail, " : GridMeshes{};", " : (" + Call("EmptyGridMapConstruct") + ", GridMeshes{});")
    Tail = (Call("IncrementalGridPublication") + ";\n\t\tconst auto GridSetupBefore = OrdinaryOp::Allocation();\n\t\t" +
            Replace(Tail, Terrain, "OrdinaryOp::RecordAllocation(GridSetupBefore, OrdinaryOp::Operation::GridSetupNewCalls, "
                    "OrdinaryOp::Operation::GridSetupNewBytes);\n\t\t" + Terrain))
    Tail = Replace(Tail, Publish, "const auto GridCommitBefore = OrdinaryOp::Allocation();\n\t\t\t" + Publish +
                   "\n\t\t\tOrdinaryOp::RecordAllocation(GridCommitBefore, OrdinaryOp::Operation::GridCommitNewCalls, "
                   "OrdinaryOp::Operation::GridCommitNewBytes);")
    return Text[:Begin] + Tail


def Generate(Unit, Text):
    if Unit == "PlaySession":
        Include = '#include "runtime/RuntimeWorkDiagnostics.hpp"'
        Body = "const runtime_detail::WorkCapture Capture(&Sample);\n\t\t\t\tRuntime.Step();"
        Text = Replace(Text, Include, Include + '\n#include "SpatialOrdinaryOperationsCapture.hpp"')
        return Replace(Text, Body, "ordinary_benchmark::operations::Step(Runtime, Sample);")
    if Unit == "Instance":
        Text = After(Text, "int Instance::LNewIndex(lua_State *L, Instance *self) {", "ScriptWrite")
        if "const auto ClassName = InstanceClassName(self);" in Text:
            Text = After(Text, "const auto ClassName = InstanceClassName(self);", "EagerWriteClassName")
        Text = Replace(Text, "if (auto Owner = OwningDataModel.lock())",
                       "if (auto Owner = (" + Call("ModelWeakLock") + ", OwningDataModel.lock()))")
        if "while (auto parent = root->ParentReference.lock())" in Text:
            Text = Prefix(Text, "root->ParentReference.lock()", "ModelFallbackParentLock")
        else:
            Text = Replace(Text, "auto Root = ParentReference.lock();",
                           "auto Root = (" + Call("ModelFallbackParentLock") + ", ParentReference.lock());")
            Text = Prefix(Text, "Root->ParentReference.lock()", "ModelFallbackParentLock")
        Text = Replace(Text, "if (const auto Realm=OwningDataModel.lock())",
                       "if (const auto Realm=(" + Call("MutationModelWeakLock") + ", OwningDataModel.lock()))")
    elif Unit == "RuntimeSchemaLifecycle":
        Text = After(Text, "const RuntimeSchemaRegistry &GetActiveRuntimeSchemaRegistry() {", "SynchronousRegistryRead")
        Text = After(Text, "std::shared_ptr<const RuntimeSchemaRegistry> RuntimeSchemaLifecycle::GetActiveRegistry() const {",
                     "SharedRegistryOwnerCopy")
    elif Unit == "Character":
        Text = Prefix(Text, "SpatialDomainMember::GetAssignmentRoot(*RootPartValue)", "MotionExplicitAssignmentWalk")
        Text = Prefix(Text, "SpatialRoot->GetDataModel() != CallerWorld.GetDataModel()", "MotionScopedModelCompare")
    elif Unit == "SpatialDomainMember":
        def AssignmentWalk(Part):
            Part = After(Part, "for (const Instance *Current = &Value; Current;) {", "AssignmentWalkNode")
            return Prefix(Part, "Current->ParentReference.lock()", "AssignmentWalkParentLock")
        Text = Section(Text, "const Instance *SpatialDomainMember::GetAssignmentRoot(",
                       "void SpatialDomainMember::Refresh(", AssignmentWalk)
    elif Unit == "WorldRoot":
        Text = World(Text)
    elif Unit == "RenderPublisher":
        Text = Render(Text)
    elif Unit == "InteractionService":
        Signature = "InteractionService::QueryNearest(const glm::vec3 &Origin, const std::shared_ptr<Player> &PlayerValue) const {"
        Text = After(Text, Signature, "NearestPromptQuery")
        Text = Section(Text, Signature, "std::size_t CandidateCount = 0;",
                       lambda Part: Prefix(Part, "PlayerWorld(PlayerValue)", "NearestPromptPlayerWorld"))
        Text = Prefix(Text, "SpatialCells.find({X, Y, Z})", "NearestPromptCellLookup")
    return ('#include "SpatialOrdinaryOperations.hpp"\nnamespace OrdinaryOp = ::gargantuan::ordinary_benchmark::operations;\n' +
            Text)


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--source", type=Path, required=True)
    Parser.add_argument("--unit", choices=Units, required=True)
    Parser.add_argument("--output", type=Path, required=True)
    Args = Parser.parse_args()
    Text = Generate(Args.unit, (Args.source / Units[Args.unit]).read_text(encoding="utf-8"))
    Args.output.parent.mkdir(parents=True, exist_ok=True)
    with Args.output.open("x", encoding="utf-8", newline="\n") as Output:
        Output.write(Text)


if __name__ == "__main__":
    Main()
