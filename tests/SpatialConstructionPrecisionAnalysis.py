# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Verify raw Engine step totals and calculate the predeclared matched-block estimates."""
import argparse
import csv
import gzip
import io
import json
import math
from pathlib import Path
import random
import statistics

from SpatialConstructionWholeStep import Quantile


def Critical(Degrees, Probability):
    # Exact Student density integrated on this small positive interval with Simpson's rule.
    Factor = math.exp(math.lgamma((Degrees + 1) / 2) - math.lgamma(Degrees / 2)) / math.sqrt(Degrees * math.pi)
    def PositiveArea(End):
        Count = 128
        Step = End / Count
        Points = [(1 + (I * Step) ** 2 / Degrees) ** (-(Degrees + 1) / 2) for I in range(Count + 1)]
        return Factor * Step / 3 * (Points[0] + Points[-1] + 4*sum(Points[1:-1:2]) + 2*sum(Points[2:-1:2]))
    Low, High = 0.0, 15.0
    for _ in range(48):
        Middle = (Low + High) / 2
        if PositiveArea(Middle) < Probability - .5:
            Low = Middle
        else:
            High = Middle
    return (Low + High) / 2


def Interval(Values):
    Count = len(Values)
    Mean = statistics.mean(Values)
    Error = statistics.stdev(Values) / math.sqrt(Count)
    Two = Critical(Count-1,.975) * Error
    return dict(Mean=Mean, Low=Mean-Two, High=Mean+Two, UpperOne=Mean+Critical(Count-1,.95)*Error)


def Percent(LogRatio):
    return 100 * math.expm1(LogRatio)


def LogStep(Row, Key):
    Value = float(Row[Key])
    if Value==0 and Key=="LowerP95Ns":
        return -math.inf
    if Value==math.inf and Key=="UpperP95Ns":
        return math.inf
    if not math.isfinite(Value) or Value <= 0:
        raise ValueError("Invalid step duration")
    return math.log(Value)


def Difference(Rows, Getter):
    return statistics.mean(Getter(Row) for Row in Rows if Row["Profile"]=="current") - statistics.mean(
        Getter(Row) for Row in Rows if Row["Profile"]=="baseline")


def VerifyScene(Scene, Processes, Raw):
    if len(Raw) != len(Processes) or not Processes:
        raise ValueError("Process and raw tick receipts differ in length")
    Expected = {"ordinary": (64,1024), "rigid": (16,768)}
    Blocks, Target = Expected[Scene]
    if len(Processes)!=Blocks*4:
        raise ValueError("The fixed number of blocks is incomplete")
    Starts = ["baseline","current"]*(Blocks//2)
    random.Random(20261127).shuffle(Starts)
    Hashes = {"baseline":"0f43758828929e453d054689e81524a662e75c9e26d8ec4d4a4089396795d4aa",
              "current":"ec39cd5036d6569985af9b07f5e8fb48dfc43bab051d8a513b787ec9b34d48b4"}
    for Index,(Process,Sample) in enumerate(zip(Processes,Raw)):
        First = Starts[Index//4]
        Second = "baseline" if First=="current" else "current"
        if (Process["Profile"]!=(First,Second,Second,First)[Index%4] or Process["Scene"]!=Scene or
                Process["EngineSHA256"]!=Hashes[Process["Profile"]] or int(Process["PriorityClass"])!=32 or
                int(Process["Block"])!=Index//4+1 or int(Process["Run"])!=Index%4+1 or
                any(Sample[Key]!=Process[Key] for Key in ("Profile","Block","Run"))):
            raise ValueError("Unexpected source identity or run order")
        Ticks = list(map(int,Sample["StepNanoseconds"].split()))
        Groups = [tuple(map(int,G.split(":"))) for G in Sample["OmittedGroupsStartCountTotal"].split()]
        Attempted, Missing = int(Process["Attempted"]), int(Process["Missing"])
        if (int(Process["Warmup"])<192 or Attempted<Target or len(Ticks)+Missing!=Attempted or
                len(Ticks)!=int(Process["Exact"]) or len(Groups)!=int(Process["Gaps"]) or
                sum(Count for _,Count,_ in Groups)!=Missing or
                sum(Ticks)+sum(Total for _,_,Total in Groups)!=int(Process["EngineStepNs"]) or
                any(Start<0 or Count<2 or Total<=0 or Start+Count>Attempted for Start,Count,Total in Groups) or
                any(A[0]+A[1]>B[0] for A,B in zip(Groups,Groups[1:]))):
            raise ValueError("Raw observations disagree with native count or nanosecond totals")
        for P in (50,95,99):
            if int(Process[f"P{P}Ns"])!=Quantile(Ticks,P/100):
                raise ValueError("Reported percentile differs from raw ticks")
        Rank = math.ceil(Attempted*.95)
        Ordered = sorted(Ticks)
        Lower = Ordered[Rank-Missing-1] if Rank>Missing else 0
        Upper = Ordered[Rank-1] if Rank<=len(Ordered) else math.inf
        if Lower!=float(Process["LowerP95Ns"]) or Upper!=float(Process["UpperP95Ns"]):
            raise ValueError("Reported missing-step bounds differ from raw ticks")


def ReadSource(Folder):
    with (Folder/"Processes.csv").open(encoding="utf-8",newline="") as File:
        Processes = list(csv.DictReader(File))
    with gzip.open(Folder/"Ticks.csv.gz","rt",encoding="utf-8",newline="") as File:
        Raw = list(csv.DictReader(File))
    if not Processes:
        raise ValueError("No process receipts")
    if Raw and "Scene" not in Raw[0]:
        Scenes = {Row["Scene"] for Row in Processes}
        if len(Scenes)!=1:
            raise ValueError("Raw ticks from multiple scenes need a scene identifier")
        Raw = [dict(Scene=Processes[0]["Scene"],**Row) for Row in Raw]
    return Processes,Raw


def Analyze(Processes):
    Blocks = []
    for Offset in range(0,len(Processes),4):
        Group = Processes[Offset:Offset+4]
        Names = [Row["Profile"] for Row in Group]
        if Names not in (["baseline","current","current","baseline"],["current","baseline","baseline","current"]):
            raise ValueError("Unexpected block order")
        Block = dict(Scene=Group[0]["Scene"], Block=int(Group[0]["Block"]), First=Names[0])
        for P in (50,95,99):
            Key = f"P{P}Ns"
            Block[f"P{P}DiffUs"] = Difference(Group,lambda Row: int(Row[Key])/1000)
            Block[f"P{P}LogRatio"] = Difference(Group,lambda Row:LogStep(Row,Key))
        Block["LowerP95LogRatio"] = Difference(Group,lambda Row: LogStep(Row,
            "LowerP95Ns" if Row["Profile"]=="current" else "UpperP95Ns"))
        Block["UpperP95LogRatio"] = Difference(Group,lambda Row: LogStep(Row,
            "UpperP95Ns" if Row["Profile"]=="current" else "LowerP95Ns"))
        Blocks.append(Block)
    Count = len(Blocks)
    if sum(Block["First"]=="current" for Block in Blocks)!=Count//2:
        raise ValueError("The fixed order balance was lost")
    Values = [Block["P95LogRatio"] for Block in Blocks]
    Four = [statistics.mean(Values[Offset:Offset+4]) for Offset in range(0,Count,4)]
    Lo = [Block["LowerP95LogRatio"] for Block in Blocks]
    Hi = [Block["UpperP95LogRatio"] for Block in Blocks]
    MissingMean = [Percent(statistics.mean(Lo)), Percent(statistics.mean(Hi)) if all(map(math.isfinite,Hi)) else None]
    MissingUpper = None
    if all(map(math.isfinite,Lo+Hi)):
        Center = (statistics.mean(Lo)+statistics.mean(Hi))/2
        MaxSD = math.sqrt(sum(max((Low-Center)**2,(High-Center)**2) for Low,High in zip(Lo,Hi))/(Count-1))
        MissingUpper = Percent(statistics.mean(Hi)+Critical(Count-1,.95)*MaxSD/math.sqrt(Count))
    Result = {
        "Blocks":Count, "Processes":len(Processes), "MedianPerProcessUs":{}, "Attempts":{}, "Missing":{},
        "ProcessRange":{}, "Estimated":{}, "OrderMeanP95Percent":{}, "FourBlockP95Percent":{},
        "MissingMeanP95Percent":MissingMean, "MissingUpperOneP95Percent":MissingUpper,
        "P95LogSD":statistics.stdev(Values),
    }
    for Name in ("baseline","current"):
        Matched = [Row for Row in Processes if Row["Profile"]==Name]
        Result["MedianPerProcessUs"][Name]={
            P:statistics.median(int(Row[f"P{P}Ns"])/1000 for Row in Matched) for P in (50,95,99)}
        Result["Attempts"][Name]=sum(int(Row["Attempted"]) for Row in Matched)
        Result["Missing"][Name]=sum(int(Row["Missing"]) for Row in Matched)
        Result["ProcessRange"][Name]={
            Key:[min(float(Row[Key]) for Row in Matched),statistics.median(float(Row[Key]) for Row in Matched),
                 max(float(Row[Key]) for Row in Matched)]
            for Key in ("P95Ns","Warmup","Queries","Missing","WallSeconds","MainCpuMs","ProcessCpuMs","PriorityClass")}
        Result["OrderMeanP95Percent"][Name]=Percent(statistics.mean(
            Block["P95LogRatio"] for Block in Blocks if Block["First"]==Name))
    for P in (50,95,99):
        Result["Estimated"][P]={
            "Percent":{K:Percent(V) for K,V in Interval([Block[f"P{P}LogRatio"] for Block in Blocks]).items()},
            "Microseconds":Interval([Block[f"P{P}DiffUs"] for Block in Blocks])}
    Result["FourBlockP95Percent"]={K:Percent(V) for K,V in Interval(Four).items()}
    return Blocks,Result


def WriteRows(File, Rows):
    Writer=csv.DictWriter(File,fieldnames=list(Rows[0]))
    Writer.writeheader()
    Writer.writerows(Rows)


def Main():
    Parser=argparse.ArgumentParser(description=__doc__)
    Source=Parser.add_mutually_exclusive_group(required=True)
    Source.add_argument("--input",type=Path,action="append",help="Acquisition directory, once for each scene")
    Source.add_argument("--verify",type=Path,help="Recompute results and compare the archived Blocks.csv")
    Parser.add_argument("--output",type=Path,help="New directory for the combined receipt when using --input")
    Args=Parser.parse_args()
    if bool(Args.input)!=bool(Args.output):
        Parser.error("--output goes with --input")
    AllProcesses,AllRaw,AllBlocks,Reports=[],[],[],{}
    for Folder in Args.input or [Args.verify]:
        Processes,Raw=ReadSource(Folder)
        AllProcesses.extend(Processes)
        AllRaw.extend(Raw)
    if {Row["Scene"] for Row in AllProcesses}!={"ordinary","rigid"} or len(AllRaw)!=len(AllProcesses):
        raise ValueError("Both complete fixed scenes are required")
    for Scene in ("ordinary","rigid"):
        Processes=[Row for Row in AllProcesses if Row["Scene"]==Scene]
        Raw=[Row for Row in AllRaw if Row["Scene"]==Scene]
        VerifyScene(Scene,Processes,Raw)
        Blocks,Reports[Scene]=Analyze(Processes)
        AllBlocks.extend(Blocks)
    if Args.verify:
        with (Args.verify/"Blocks.csv").open(encoding="utf-8",newline="") as File:
            Archived=list(csv.DictReader(File))
        if len(Archived)!=len(AllBlocks):
            raise ValueError("Archived block count differs")
        for Computed,Recorded in zip(AllBlocks,Archived):
            for Key,Value in Computed.items():
                if isinstance(Value,float):
                    if math.isclose(Value,float(Recorded[Key]),rel_tol=1e-12,abs_tol=1e-12):
                        continue
                elif str(Value)==Recorded[Key]:
                    continue
                raise ValueError(f"Archived {Key} differs for {Recorded['Scene']} block {Recorded['Block']}")
        print(json.dumps(Reports,indent=2))
        return
    Args.output.mkdir(parents=True,exist_ok=True)
    for Name,Rows in (("Processes",AllProcesses),("Blocks",AllBlocks)):
        with (Args.output/(Name+".csv")).open("x",encoding="utf-8",newline="") as File:
            WriteRows(File,Rows)
    with (Args.output/"Ticks.csv.gz").open("xb") as File, gzip.GzipFile(
            filename="",fileobj=File,mode="wb",mtime=0) as Compressed, io.TextIOWrapper(
            Compressed,encoding="utf-8",newline="") as Text:
        WriteRows(Text,AllRaw)
    print(json.dumps(Reports,indent=2))


if __name__=="__main__":
    Main()
