# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Materialize the isolated profiling-only PlaySession TU against either exact stock tree."""
import argparse
from pathlib import Path
import re

Parser=argparse.ArgumentParser(description=__doc__)
Parser.add_argument("--source",type=Path,required=True)
Parser.add_argument("--output",type=Path,required=True)
Parser.add_argument("--unit",choices=("PlaySession","RuntimeSchema","Character","WorldRoot"),default="PlaySession")
Args=Parser.parse_args()
Paths={"PlaySession":"src/editor/PlaySession.cpp","RuntimeSchema":"src/reflection/RuntimeSchema.cpp",
       "Character":"src/classes/Character.cpp","WorldRoot":"src/classes/WorldRoot.cpp"}
Source=(Args.source/Paths[Args.unit]).read_text(encoding="utf-8")
if Args.unit=="PlaySession":
    Include='#include "runtime/RuntimeWorkDiagnostics.hpp"'
    Body="const runtime_detail::WorkCapture Capture(&Sample);\n\t\t\t\tRuntime.Step();"
    if Source.count(Include)!=1 or Source.count(Body)!=1:
        raise ValueError("PlaySession native capture source does not match the reviewed insertion")
    Source=Source.replace(Include,Include+'\n#include "SpatialOrdinaryNativeProfile.hpp"').replace(Body,
        "ordinary_benchmark::Step(Runtime,Sample);")
else:
    Source='#include "SpatialOrdinaryCallProfile.hpp"\n'+Source
    Hooks={"RuntimeSchema":[("RuntimeSchemaRegistry::FindClassByName","ShortClassLookup")],
           "Character":[("Character::Move","ScriptCharacterMove"),("Character::AdmitMotion","CharacterAdmitMotion"),
                        ("Character::CommitSimulationTransform","CharacterCommit"),("Character::SynchronizeRootPart","CharacterRootSync")],
           "WorldRoot":[("WorldRoot::ApplyPendingImpulses","PendingImpulses"),("WorldRoot::ReadbackDynamicBodies","DynamicReadback"),
                        ("WorldRoot::ResolveKinematicMotion","KinematicResolve"),("WorldRoot::GetEffectiveWorld","EffectiveWorld")]}
    for Method,Metric in Hooks[Args.unit]:
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
