# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Record the five stock EditorHost native phase deltas on the same individual ticks."""
import argparse
import csv
import ctypes
import gzip
import hashlib
import os
from pathlib import Path
import random
import tempfile
import time
from unittest.mock import patch

import SpatialConstructionPrecision as Precision


class RecordingHost(Precision.EditorHost):
    def __init__(self, Binary, Win):
        super().__init__(Binary)
        self.Win = Win
        self.Rows = []
        self.Previous = None

    def __enter__(self):
        super().__enter__()
        self.Thread = self.Win.MainThread(self.Process.pid)
        self.Cycles = ctypes.c_uint64()
        return self

    def __exit__(self, *Args):
        self.Win.Kernel.CloseHandle(self.Thread)
        return super().__exit__(*Args)

    def Call(self, Method, **Parameters):
        Begin = time.perf_counter_ns()
        Response = super().Call(Method, **Parameters)
        if Method not in ("ConfigureStreamingDiagnostics", "GetStreamingDiagnostics"):
            return Response
        ReplyNs = time.perf_counter_ns()-Begin
        Diagnostics = Response["Diagnostics"]
        State = Diagnostics.get("Execution")
        if State is None:
            self.Previous = None
            return Response
        self.Win.Check(self.Win.Kernel.QueryThreadCycleTime(self.Thread, ctypes.byref(self.Cycles)))
        Tick = int(Diagnostics["SimulationTick"])
        Totals = [State["Phases"][Name]["TotalNanoseconds"] for Name in Precision.Phases]
        Now = time.perf_counter_ns()
        if self.Previous is not None:
            Earlier, EarlierTotals, EarlierCycles, EarlierTime = self.Previous
            Gap = Tick-Earlier
            Values = [After-Before for Before,After in zip(EarlierTotals,Totals)]
            if Gap<0 or min(Values)<0 or sum(Values[1:])>Values[0] or (Gap==0 and any(Values)):
                raise ValueError("Phase totals disagree with native clock boundaries")
            self.Rows.append((Earlier,Tick,Gap,*Values,Values[0]-sum(Values[1:]),
                              self.Cycles.value-EarlierCycles,Now-EarlierTime,ReplyNs))
        self.Previous = (Tick,Totals,self.Cycles.value,Now)
        return Response


def Capture(Win, Binary, Project, Args):
    Recorder = RecordingHost(Binary,Win)
    # Wrap the released acquisition's existing EditorHost calls. Its scene, warmup,
    # counter bounds and pacing remain identical; the wrapper sends no extra RPCs.
    with patch.object(Precision,"EditorHost",return_value=Recorder):
        Summary,Steps,_,_ = Precision.Measure(Win,Binary,Project,Args)
    Rows = [R for R in Recorder.Rows if Summary["StartSimulationTick"]<=R[0] and R[1]<=Summary["EndSimulationTick"]]
    if [R[3] for R in Rows if R[2]==1]!=Steps or sum(R[2] for R in Rows)!=Summary["RingTicks"]:
        raise ValueError("Aligned phase capture disagrees with fixed native measurement")
    for Index,Name in enumerate(Precision.Phases,3):
        if sum(R[Index] for R in Rows)!=Summary[Name+"Ns"]:
            raise ValueError(f"Aligned {Name} differs from the verified phase sum")
    return Summary,Rows


def Main():
    Parser=argparse.ArgumentParser(description=__doc__)
    for Name in ("baseline","current","output"):
        Parser.add_argument("--"+Name,type=Path,required=True)
    Parser.add_argument("--blocks",type=int,default=8)
    Parser.add_argument("--frames",type=int,default=384)
    Parser.add_argument("--warmup",type=int,default=192)
    Parser.add_argument("--scene",choices=("ordinary","rigid"),default="ordinary")
    Parser.add_argument("--engine-mask",type=lambda Text:int(Text,0),default=0xffff0000)
    Parser.add_argument("--main-cpu",type=int,default=24)
    Parser.add_argument("--sampler-cpu",type=int,default=8)
    Parser.add_argument("--shared-cache",action="store_true",help="Allow a separate physical sampler core in the same last-level cache for a labeled supplemental host")
    Mode=Parser.add_mutually_exclusive_group()
    Mode.add_argument("--native",action="store_true",help="Require raw files from both privately instrumented Engine executables")
    Mode.add_argument("--observer-treatment",action="store_true",help="Use one identical diagnostic binary with the observer enabled only for current")
    Args=Parser.parse_args()
    if not 2<=Args.blocks<=64 or Args.blocks%2 or not 48<=Args.frames<=2048 or not 48<=Args.warmup<=1024:
        Parser.error("Use an even 2–64 blocks, 48–2048 measured steps and 48–1024 warmup steps")
    Args.poll_ms = 4
    Win=Precision.Windows()
    Win.Kernel.QueryThreadCycleTime.argtypes=[ctypes.c_void_p,ctypes.POINTER(ctypes.c_uint64)]
    Win.Kernel.QueryThreadCycleTime.restype=ctypes.c_int
    Layout={Logical:(Core,Last) for Group,Logical,Core,Last in Win.Layout() if Group==0}
    EngineCpus={Cpu for Cpu in range(64) if (1<<Cpu)&Args.engine_mask}
    if (not 0<Args.engine_mask<1<<64 or not EngineCpus<=Layout.keys() or Args.main_cpu not in EngineCpus or
            Args.sampler_cpu not in Layout or any(Layout[C][0]==Layout[Args.sampler_cpu][0] for C in EngineCpus)):
        raise ValueError("The Engine and sampler need valid, separate physical cores")
    EngineCache=Layout[Args.main_cpu][1]
    SamplerCache=Layout[Args.sampler_cpu][1]
    if any(Layout[C][1]!=EngineCache for C in EngineCpus) or (not Args.shared_cache and EngineCache==SamplerCache):
        raise ValueError("The specified affinity does not satisfy the selected last-level cache contract")
    Win.Check(Win.Kernel.SetProcessAffinityMask(Win.Kernel.GetCurrentProcess(),1<<Args.sampler_cpu))
    Binaries={Key:getattr(Args,Key).resolve(strict=True) for Key in ("baseline","current")}
    Hashes={Key:hashlib.sha256(Binary.read_bytes()).hexdigest() for Key,Binary in Binaries.items()}
    if Args.observer_treatment and Binaries["baseline"]!=Binaries["current"]:
        Parser.error("Observer calibration requires the identical executable and adjacent runtime on both arms")
    Starts=["baseline","current"]*(Args.blocks//2)
    random.Random(20261127).shuffle(Starts)
    Args.output.mkdir(parents=True,exist_ok=True)
    with (Args.output/"Processes.csv").open("x",encoding="utf-8",newline="") as ProcessFile, gzip.open(
            Args.output/"Polls.csv.gz","xt",encoding="utf-8",newline="") as PollFile, tempfile.TemporaryDirectory(prefix="gsp-") as Projects:
        Table=None
        Samples=csv.writer(PollFile)
        Samples.writerow(("Block","Run","Profile","BeforeTick","AfterTick","Count",*[P+"Ns" for P in Precision.Phases],
                          "OtherStepNs","MainCyclesBetweenReplies","NanosecondsBetweenReplies","ReplyNs"))
        for Block,First in enumerate(Starts,1):
            Second="baseline" if First=="current" else "current"
            for Run,Profile in enumerate((First,Second,Second,First),1):
                NativePath=(Args.output/f"B{Block}R{Run}-{Profile}.csv").resolve()
                NativeEnabled=Args.native or (Args.observer_treatment and Profile=="current")
                if NativeEnabled and NativePath.exists():
                    raise FileExistsError(NativePath)
                with patch.dict(os.environ,{"GARGANTUAN_ORDINARY_PROFILE_CSV":str(NativePath) if NativeEnabled else ""}):
                    Summary,Rows=Capture(Win,Binaries[Profile],Path(Projects)/f"B{Block}R{Run}",Args)
                if NativeEnabled:
                    with NativePath.open(encoding="utf-8",newline="") as File:
                        NativeRows={int(R["Tick"]):R for R in csv.DictReader(File)}
                    Exact={R[1]:R[3] for R in Rows if R[2]==1}
                    if any(Tick not in NativeRows or int(NativeRows[Tick]["EngineStepNs"])!=Value or
                           int(NativeRows[Tick]["Dropped"]) for Tick,Value in Exact.items()):
                        raise ValueError("Native step recorder disagrees with stock EditorHost phase totals")
                Row=dict(Scene=Args.scene,Block=Block,Run=Run,Profile=Profile,EngineSHA256=Hashes[Profile],
                         EngineMask=hex(Args.engine_mask),MainCpu=Args.main_cpu,SamplerCpu=Args.sampler_cpu,
                         EngineCache=EngineCache,SamplerCache=SamplerCache,**Summary)
                if Table is None:
                    Table=csv.DictWriter(ProcessFile,fieldnames=list(Row))
                    Table.writeheader()
                Table.writerow(Row)
                Samples.writerows((Block,Run,Profile,*R) for R in Rows)
                ProcessFile.flush()
                PollFile.flush()
                print(f"[Spatial:StepProfile] completed block {Block}/{Args.blocks} run {Run}",flush=True)


if __name__=="__main__":
    Main()
