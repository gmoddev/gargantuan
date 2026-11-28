# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Compute additive phase contributions from the same stock or instrumented slow steps."""
import argparse
from collections import defaultdict
import csv
import gzip
import math
from pathlib import Path
import statistics

from SpatialConstructionPrecisionAnalysis import Critical
from SpatialConstructionWholeStep import Quantile
from SpatialOrdinaryDiagnosticEvidence import CheckNative, NativeRows


Phases=("EngineStepNs","PreSimulationNs","PhysicsNs","AnimationNs","RenderPublicationNs","OtherStepNs")
PollMetrics=("MainCyclesBetweenReplies","NanosecondsBetweenReplies","ReplyNs")
Groups=("All","MedianBand49To51","P95","P95Band94To96","SlowestFivePercent")


def Cut(Ordered, Group):
    N=len(Ordered)
    if Group=="All": return Ordered
    if Group=="MedianBand49To51": return Ordered[math.ceil(.49*N)-1:math.ceil(.51*N)]
    if Group=="P95": return Ordered[math.ceil(.95*N)-1:math.ceil(.95*N)]
    if Group=="P95Band94To96": return Ordered[math.ceil(.94*N)-1:math.ceil(.96*N)]
    return Ordered[math.ceil(.95*N)-1:]


def Inspect(PathValue, Native):
    with (PathValue/"Processes.csv").open(encoding="utf-8",newline="") as File:
        Processes=list(csv.DictReader(File))
    Polls=defaultdict(list)
    with gzip.open(PathValue/"Polls.csv.gz","rt",encoding="utf-8",newline="") as File:
        for Row in csv.DictReader(File):
            Key=(Row["Block"],Row["Run"],Row["Profile"])
            Polls[Key].append({K:int(V) for K,V in Row.items() if K not in ("Block","Run","Profile")})
    Values=defaultdict(list)
    Stats=defaultdict(int)
    if len(Processes)!=len(Polls) or len(Processes)%4 or len(Processes)<8:
        raise ValueError("Incomplete paired run")
    Firsts=[]
    for Index,Proc in enumerate(Processes):
        Block,Run,Name=(Proc[K] for K in ("Block","Run","Profile"))
        if (int(Block),int(Run))!=(Index//4+1,Index%4+1): raise ValueError("Missing scheduled process")
        if Index%4==0:
            Order=[P["Profile"] for P in Processes[Index:Index+4]]
            if Order not in (["baseline","current","current","baseline"],["current","baseline","baseline","current"]):
                raise ValueError("Unbalanced within-block order")
            Firsts.append(Name)
        Rows=Polls[Block,Run,Name]
        Exact=[R for R in Rows if R["Count"]==1]
        if (sum(R["Count"] for R in Rows)!=int(Proc["RingTicks"]) or len(Exact)!=int(Proc["Exact"]) or
                Quantile([R["EngineStepNs"] for R in Exact],.95)!=int(Proc["P95Ns"])):
            raise ValueError("Raw ticks differ from the stock measurement")
        for K in Phases[:-1]:
            if sum(R[K] for R in Rows)!=int(Proc[K]): raise ValueError(f"Native {K} total differs")
        if any(R["AfterTick"]-R["BeforeTick"]!=R["Count"] or sum(R[K] for K in Phases[1:])!=R[Phases[0]] for R in Rows):
            raise ValueError("Per-tick phase sum or ordinal violation")
        SampleMetrics=list(Phases+PollMetrics)
        if Native:
            NativeData=NativeRows(PathValue,f"B{Block}R{Run}-{Name}.csv")
            if len(CheckNative(Rows,NativeData))!=int(Proc["RingTicks"]):
                raise ValueError("Per-step native evidence has a gap or mismatched grouped sums")
            for R in Exact:
                Source=NativeData[R["AfterTick"]]
                if int(Source["Dropped"]) or int(Source["EngineStepNs"])!=R["EngineStepNs"]:
                    raise ValueError("Native recorder and stock diagnostic differ")
                if int(Source["ReadError"]) or int(Source["EnableError"]):
                    raise ValueError("Windows thread profiling unavailable in this run")
                for K,V in Source.items():
                    if K not in ("Tick","Dropped","ReadError","EnableError","WaitReasons") and not K.startswith("Hardware") and not K.endswith("Bits"):
                        R[K]=int(V)
                R["SwitchedTick"]=int(R["ContextSwitches"]>0)
                R["MainWaitReported"]=int(int(Source["WaitReasons"])!=0)
                R["LuauNetBytes"]=R["LuauHeapAfterBytes"]-R["LuauHeapBeforeBytes"]
                R["ClassLookupNs"]=R["ShortClassLookupNs"]+R["QualifiedClassLookupNs"]
                # The prescribed ordinary scene issues exactly one script Move and its
                # sole nested admission/world-query call. Reject a different call topology.
                if not (R["ScriptCharacterMoveCalls"]==R["CharacterAdmitMotionCalls"]==R["KinematicResolveCalls"]==1):
                    raise ValueError("This scene's nested character attribution requires one Move, admission and world collision query")
                R["MotionAdmissionOutsideWorldQueryNs"]=R["CharacterAdmitMotionNs"]-R["KinematicResolveNs"]
                R["ScriptMoveOutsideAdmissionNs"]=R["ScriptCharacterMoveNs"]-R["CharacterAdmitMotionNs"]
                if min(R["MotionAdmissionOutsideWorldQueryNs"],R["ScriptMoveOutsideAdmissionNs"])<0:
                    raise ValueError("A nested motion scope exceeds its enclosing duration")
                Stats["HardwareActiveSamples"]+=int(int(Source["HardwareCount"])>0)
            SampleMetrics += [K for K in Exact[0] if K not in Phases+PollMetrics+("BeforeTick","AfterTick","Count")]
        Ordered=sorted(Exact,key=lambda R:(R["EngineStepNs"],R["AfterTick"]))
        for Group in Groups:
            Selected=Cut(Ordered,Group)
            for K in SampleMetrics:
                Unit=1000 if K.endswith("Ns") or K=="NanosecondsBetweenReplies" else 1
                Values[(Group,K)].append(statistics.mean(R[K] for R in Selected)/Unit)
        for K in ("Exact","Attempted","PollMissing","ResetMissing","Queries"):
            Stats[K]+=int(Proc[K])
    if Firsts.count("current")!=len(Firsts)//2:
        raise ValueError("Unbalanced block orientations")
    Degrees=len(Firsts)-1
    Two,One=Critical(Degrees,.975),Critical(Degrees,.95)
    Report=[]
    for (Group,Metric),Samples in Values.items():
        Baseline=[V for P,V in zip(Processes,Samples) if P["Profile"]=="baseline"]
        Current=[V for P,V in zip(Processes,Samples) if P["Profile"]=="current"]
        Diffs=[sum((1 if Processes[J]["Profile"]=="current" else -1)*Samples[J]/2 for J in range(I,I+4))
               for I in range(0,len(Processes),4)]
        Mean=statistics.mean(Diffs); Error=statistics.stdev(Diffs)/math.sqrt(len(Diffs))
        Report.append(dict(Group=Group,Metric=Metric,Unit="us" if Metric.endswith("Ns") or Metric=="NanosecondsBetweenReplies" else
                           "native units",Baseline=statistics.mean(Baseline),Current=statistics.mean(Current),
                           Difference=Mean,Lower95=Mean-Two*Error,Upper95=Mean+Two*Error,UpperOne95=Mean+One*Error))
    return Report,dict(Blocks=len(Firsts),Processes=len(Processes),**Stats)


def Main():
    Parser=argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--input",type=Path,required=True)
    Parser.add_argument("--output",type=Path,required=True)
    Parser.add_argument("--native",action="store_true")
    Args=Parser.parse_args()
    Report,Stats=Inspect(Args.input,Args.native)
    Args.output.parent.mkdir(parents=True,exist_ok=True)
    with Args.output.open("x",encoding="utf-8",newline="") as File:
        Writer=csv.DictWriter(File,fieldnames=list(Report[0]))
        Writer.writeheader(); Writer.writerows(Report)
    print(Stats)


if __name__=="__main__":
    Main()
