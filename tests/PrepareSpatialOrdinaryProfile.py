# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Materialize the isolated profiling-only PlaySession TU against either exact stock tree."""
import argparse
from pathlib import Path
import re

Parser=argparse.ArgumentParser(description=__doc__)
Parser.add_argument("--source",type=Path,required=True)
Parser.add_argument("--output",type=Path,required=True)
Parser.add_argument("--unit",choices=("PlaySession","RuntimeSchema","Character","WorldRoot","RenderPublisher","RenderProjection"),default="PlaySession")
Args=Parser.parse_args()
Paths={"PlaySession":"src/editor/PlaySession.cpp","RuntimeSchema":"src/reflection/RuntimeSchema.cpp",
       "Character":"src/classes/Character.cpp","WorldRoot":"src/classes/WorldRoot.cpp",
       "RenderPublisher":"src/render/RenderPublisher.cpp","RenderProjection":"src/render/RenderProjection.cpp"}
Source=(Args.source/Paths[Args.unit]).read_text(encoding="utf-8")
if Args.unit=="PlaySession":
    Include='#include "runtime/RuntimeWorkDiagnostics.hpp"'
    Body="const runtime_detail::WorkCapture Capture(&Sample);\n\t\t\t\tRuntime.Step();"
    if Source.count(Include)!=1 or Source.count(Body)!=1:
        raise ValueError("PlaySession native capture source does not match the reviewed insertion")
    Source=Source.replace(Include,Include+'\n#include "SpatialOrdinaryNativeProfile.hpp"').replace(Body,
        "ordinary_benchmark::Step(Runtime,Sample);")
elif Args.unit in ("RenderPublisher", "RenderProjection"):
    Prefix = '#include "SpatialOrdinaryCallProfile.hpp"\nnamespace StepProfile = ::gargantuan::ordinary_benchmark;\n'
    if Args.unit == "RenderPublisher":
        Signature = 'AssertAuthoritativeMutation("RenderPublication extraction");'
        if Source.count(Signature) != 1:
            raise ValueError("Expected one render publication entry")
        Source = Source.replace(Signature, Signature +
            '\n\t\tconst StepProfile::FunctionScope PublishScope(StepProfile::Function::Publish);' +
            '\n\t\tProfilingEnabled = StepProfile::ActiveFunctions != nullptr;')
        Camera = 'const auto Camera = BuildCamera(CameraInput, ViewportWidth, ViewportHeight);'
        if Source.count(Camera) != 1:
            raise ValueError("Expected one publication camera extraction")
        Source = Source.replace(Camera,
            'const auto Camera = [&] { const StepProfile::FunctionScope CameraScope(StepProfile::Function::CameraBuild);'
            ' return BuildCamera(CameraInput, ViewportWidth, ViewportHeight); }();')
        Read = re.compile(r'(?:Service|Assets)->ReadChanges\((?:0|VisualAssetChangeSequence)\)')
        Source, Count = Read.subn(lambda Match: '([&] { const StepProfile::FunctionScope AssetScope(' +
            'StepProfile::Function::VisualAssetRead); return ' + Match.group() + '; }())', Source)
        if Count != 2:
            raise ValueError("Expected both attachment asset journal call sites")
        Return = 'return std::shared_ptr<const RenderPublication>(std::move(Result));'
        if Source.count(Return) != 2:
            raise ValueError("Expected both successful publication return paths")
        Stages = (("DirtyCapture", "DirtyCapture"), ("DirtyExpansion", "DirtyExpansion"),
                  ("FinalExtraction", "FinalStateExtraction"), ("PublicationBuild", "PublicationConstruction"),
                  ("PublisherReconcile", "StateCacheReconciliation"))
        Record = ''.join(f'StepProfile::RecordPhase(StepProfile::Function::{Name}, LastProfile.{Field}Nanoseconds);\n\t\t'
                         for Name, Field in Stages)
        Source = Source.replace(Return, Record + Return)
    else:
        Signature = 'RenderProjectionChanges RenderProjection::Apply(const RenderPublication &Publication) {'
        Start = Source.index(Signature)
        Stop = Source.index('return Changes;', Start)
        Body = Source[Start:Stop].replace(Signature, Signature +
            '\n\t\tconst StepProfile::FunctionScope ProjectionScope(StepProfile::Function::ProjectionApply);')
        Source = Source[:Start] + Body + (
            'StepProfile::RecordPhase(StepProfile::Function::ProjectionValidation, Changes.ValidationNanoseconds);\n\t\t'
            'StepProfile::RecordPhase(StepProfile::Function::ProjectionMutation, Changes.MutationNanoseconds);\n\t\t') + Source[Stop:]
    Source = Prefix + Source
else:
    Source='#include "SpatialOrdinaryCallProfile.hpp"\n'+Source
    Hooks={"RuntimeSchema":[("RuntimeSchemaRegistry::FindClassByName","ShortClassLookup")],
           "Character":[("Character::Move","ScriptCharacterMove"),("Character::AdmitMotion","CharacterAdmitMotion"),
                        ("Character::CommitSimulationTransform","CharacterCommit"),("Character::SynchronizeRootPart","CharacterRootSync")],
           "WorldRoot":[("WorldRoot::ApplyPendingImpulses","PendingImpulses"),("WorldRoot::ReadbackDynamicBodies","DynamicReadback"),
                        ("WorldRoot::ResolveKinematicMotion","KinematicResolve"),("WorldRoot::GetEffectiveWorld","EffectiveWorld")]}
    for Method,Metric in Hooks[Args.unit]:
        if Metric=="EffectiveWorld" and "WorldRoot::FindEffectiveWorld(" in Source:
            Method="WorldRoot::FindEffectiveWorld"
        if Metric=="CharacterAdmitMotion" and "Character::AdmitRoutedMotion(" in Source:
            Method="Character::AdmitRoutedMotion"
        Pattern=re.compile(r"(?m)^\s*[\w:<>\s*&]+?\b"+re.escape(Method)+r"\([^;{}]*\)\s*(?:const\s*)?\{")
        Matches=list(Pattern.finditer(Source))
        if not Matches and Metric=="EffectiveWorld" and "WorldRoot::GetEffectiveWorld(" not in Source:
            continue  # No named-domain resolver in the historical Engine.
        if len(Matches)!=1:
            raise ValueError(f"Expected a single definition of {Method}, got {len(Matches)}")
        End=Matches[0].end()
        Value="ordinary_benchmark::Function::"+Metric
        if Metric=="ShortClassLookup":
            Value="name.find('.')==std::string_view::npos ? "+Value+" : ordinary_benchmark::Function::QualifiedClassLookup"
        Source=Source[:End]+"\n\t\tconst ordinary_benchmark::FunctionScope NativeFunctionProfile("+Value+");"+Source[End:]
    if Args.unit=="Character":
        Check="if (!Result.Succeeded()) throw std::runtime_error"
        if Source.count(Check)!=1: raise ValueError("Expected one script-facing character motion result")
        Source=Source.replace(Check,"ordinary_benchmark::RecordMotion(Translation.y,Velocity.y,Result.Velocity.y,\n"
                              "\t\t\tResult.AppliedTranslation.y,Result.Position.y,Result.HasFloor,Result.Collided);\n\t\t"+Check)
Args.output.parent.mkdir(parents=True,exist_ok=True)
with Args.output.open("x",encoding="utf-8",newline="\n") as Output:
    Output.write(Source)
