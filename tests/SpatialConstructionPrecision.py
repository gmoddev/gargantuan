# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Paired measurements of instrumented whole Engine steps in Windows EditorHost Play."""
import argparse
import csv
import ctypes
from ctypes import wintypes
import gzip
import hashlib
import math
from pathlib import Path
import random
import struct
import tempfile
import time

from SpatialConstructionWholeStep import EditorHost, Probe, Quantile, Start


class ThreadEntry(ctypes.Structure):
    _fields_ = [(Name, wintypes.DWORD) for Name in
                ("Size", "Usage", "Thread", "Process", "Priority", "DeltaPriority", "Flags")]


class GroupAffinity(ctypes.Structure):
    _fields_ = [("Mask", ctypes.c_size_t), ("Group", wintypes.WORD), ("Reserved", wintypes.WORD * 3)]


class Windows:
    """Use native handles for this benchmark process and its single EditorHost child only."""
    def __init__(self):
        self.Kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        Handle = wintypes.HANDLE
        Size = ctypes.c_size_t
        Pointer = ctypes.c_void_p
        for Name, Args, Returns in (
            ("GetCurrentProcess", [], Handle),
            ("GetSystemCpuSetInformation", [Pointer, wintypes.DWORD, Pointer, Handle, wintypes.DWORD], wintypes.BOOL),
            ("GetProcessAffinityMask", [Handle, Pointer, Pointer], wintypes.BOOL),
            ("GetPriorityClass", [Handle], wintypes.DWORD),
            ("SetProcessAffinityMask", [Handle, Size], wintypes.BOOL),
            ("CreateToolhelp32Snapshot", [wintypes.DWORD, wintypes.DWORD], Handle),
            ("Thread32First", [Handle, Pointer], wintypes.BOOL),
            ("Thread32Next", [Handle, Pointer], wintypes.BOOL),
            ("OpenThread", [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD], Handle),
            ("GetThreadTimes", [Handle, Pointer, Pointer, Pointer, Pointer], wintypes.BOOL),
            ("GetProcessTimes", [Handle, Pointer, Pointer, Pointer, Pointer], wintypes.BOOL),
            ("SetThreadAffinityMask", [Handle, Size], Size),
            ("GetThreadGroupAffinity", [Handle, Pointer], wintypes.BOOL),
            ("CloseHandle", [Handle], wintypes.BOOL),
        ):
            Api = getattr(self.Kernel, Name)
            Api.argtypes, Api.restype = Args, Returns

    @staticmethod
    def Check(Succeeded):
        if not Succeeded:
            raise ctypes.WinError(ctypes.get_last_error())

    def Layout(self):
        Required = wintypes.DWORD()
        self.Kernel.GetSystemCpuSetInformation(None, 0, ctypes.byref(Required), None, 0)
        Buffer = ctypes.create_string_buffer(Required.value)
        self.Check(self.Kernel.GetSystemCpuSetInformation(Buffer, len(Buffer), ctypes.byref(Required), None, 0))
        Rows, Cursor = [], 0
        while Cursor < Required.value:
            Length, Kind = struct.unpack_from("<II", Buffer, Cursor)
            if Length < 20 or Cursor + Length > Required.value:
                raise ValueError("Invalid CPU set descriptor")
            if Kind == 0:
                Rows.append(struct.unpack_from("<HBBB", Buffer, Cursor + 12))  # Group, logical, core, last-level cache
            Cursor += Length
        return Rows

    def ProcessMask(self, Process):
        Current, System = ctypes.c_size_t(), ctypes.c_size_t()
        self.Check(self.Kernel.GetProcessAffinityMask(Process, ctypes.byref(Current), ctypes.byref(System)))
        return Current.value

    def Times(self, Handle, Thread):
        Values = [ctypes.c_uint64() for _ in range(4)]
        Method = self.Kernel.GetThreadTimes if Thread else self.Kernel.GetProcessTimes
        self.Check(Method(Handle, *[ctypes.byref(Value) for Value in Values]))
        return Values[0].value, (Values[2].value + Values[3].value) * 100

    def MainThread(self, ProcessId):
        Snapshot = self.Kernel.CreateToolhelp32Snapshot(4, 0)  # TH32CS_SNAPTHREAD
        self.Check(Snapshot != ctypes.c_void_p(-1).value)
        Candidates = []
        try:
            Entry = ThreadEntry(Size=ctypes.sizeof(ThreadEntry))
            Present = self.Kernel.Thread32First(Snapshot, ctypes.byref(Entry))
            while Present:
                if Entry.Process == ProcessId:
                    Handle = self.Kernel.OpenThread(0x60, False, Entry.Thread)  # query and set information
                    if Handle:
                        Candidates.append((self.Times(Handle, True)[0], Entry.Thread, Handle))
                Present = self.Kernel.Thread32Next(Snapshot, ctypes.byref(Entry))
        finally:
            self.Kernel.CloseHandle(Snapshot)
        if not Candidates:
            raise RuntimeError("No live thread belongs to the EditorHost")
        Candidates.sort()
        for _, _, Handle in Candidates[1:]:
            self.Kernel.CloseHandle(Handle)
        return Candidates[0][2]

    def VerifyAffinity(self, Process, Main, Mask, Cpu):
        Affinity = GroupAffinity()
        self.Check(self.Kernel.GetThreadGroupAffinity(Main, ctypes.byref(Affinity)))
        if self.ProcessMask(Process) != Mask or Affinity.Group != 0 or Affinity.Mask != 1 << Cpu:
            raise ValueError("Engine CPU affinity changed")


ReadyMarker = '''
local SetupTicks=0
local ReadyConnection
ReadyConnection=Run.PostSimulation:Connect(function()
    SetupTicks+=1
    if SetupTicks==12 then
        ReadyConnection:Disconnect()
        if Movers then
            for _,Mover in Movers do
                assert(math.abs(Mover.Position.Y-4)>.05, "ordinary rigid body did not move")
            end
        end
        print("[Spatial:Precision] 24 ordinary anchored Parts ready")
    end
end)
'''
RigidScene = '''
Movers={}
for Index=0,5 do
    local P=Instance.new("Part")
    P.Name="OrdinaryMover"
    P.Size=Vector3.new(1,1,1)
    P.Position=Vector3.new(Index*3,4,0)
    P.Anchored=false
    P.Parent=Space
    table.insert(Movers,P)
end
local Tick=0
Run.PreSimulation:Connect(function()
    Tick+=1
    if Tick%30==1 then
        for _,P in Movers do P:SetLinearVelocity(Vector3.new(0,36,0)) end
    end
end)
'''
Phases = ("EngineStep", "PreSimulation", "Physics", "Animation", "RenderPublication")


def Measure(Win, Binary, Project, Args):
    with EditorHost(Binary) as Host:
        Process = int(Host.Process._handle)
        Win.Check(Win.Kernel.SetProcessAffinityMask(Process, Args.engine_mask))
        Main = Win.MainThread(Host.Process.pid)
        try:
            Win.Check(Win.Kernel.SetThreadAffinityMask(Main, 1 << Args.main_cpu))
            Win.VerifyAffinity(Process, Main, Args.engine_mask, Args.main_cpu)
            # The original 24-Part observer stays active throughout this longer capture.
            # Its final table sort occurs after the measured sample budget.
            Scene = Probe.replace("1088", "1000000000") + "\nlocal Movers=nil\n"
            if Args.scene == "rigid":
                Scene += RigidScene
            Session = Start(Host, Project, False, Scene + ReadyMarker)
            try:
                Ready = False
                Deadline = time.monotonic() + 6
                while not Ready:
                    Rows = Host.Call("PollPlayDiagnostics", PlaySessionId=Session)["Diagnostics"]
                    if any(Row.get("Severity") == "Error" for Row in Rows):
                        raise RuntimeError("The ordinary scene failed to initialize")
                    Ready = any("[Spatial:Precision] 24 ordinary anchored Parts ready" in Row.get("Message", "") for Row in Rows)
                    if time.monotonic() > Deadline:
                        raise TimeoutError("Ordinary scene marker was absent")
                    if not Ready:
                        time.sleep(.01)
                return ReadSteps(Win, Host, Process, Main, Session, Args)
            finally:
                Host.Call("StopPlaySession", PlaySessionId=Session)
        finally:
            Win.Kernel.CloseHandle(Main)


def ReadSteps(Win, Host, Process, Main, Session, Args):
    def Reset():
        Host.Call("ConfigureStreamingDiagnostics", PlaySessionId=Session, Enabled=False)
        Diagnostics = Host.Call("ConfigureStreamingDiagnostics", PlaySessionId=Session, Enabled=True)["Diagnostics"]
        State = Diagnostics["Execution"]
        if State["Accounting"] != "EditorPlayInclusiveWallTimeV1" or State["Samples"] != 0:
            raise ValueError("Native sample ring did not reset")
        return int(Diagnostics["SimulationTick"])

    def Snapshot():
        Diagnostics = Host.Call("GetStreamingDiagnostics", PlaySessionId=Session, MaximumUnits=0, MaximumTimingSamples=0)["Diagnostics"]
        return int(Diagnostics["SimulationTick"]), Diagnostics["Execution"]

    Warm = 0
    while Warm < Args.warmup:
        Reset()
        Target = min(48, Args.warmup - Warm)
        time.sleep(Target * .0075)
        Deadline = time.monotonic() + 3
        while (Count := Snapshot()[1]["Samples"]) < Target:
            if time.monotonic() > Deadline:
                raise TimeoutError("EditorHost did not progress during warmup")
            time.sleep(.004)
        Warm += Count

    Ticks, Groups, ResetGaps = [], [], []
    WindowsCount = Queries = 0
    FirstTick = LastTick = None
    WallStart = time.perf_counter_ns()
    ProcessStart, MainStart = Win.Times(Process, False)[1], Win.Times(Main, True)[1]
    Deadline = time.monotonic() + Args.frames * .04 + 15
    PhaseTotals = {Name: 0 for Name in Phases}
    Observed = 0
    while Observed < Args.frames:
        WindowStart = Reset()
        if FirstTick is None:
            FirstTick = WindowStart
        elif WindowStart < LastTick:
            raise ValueError("Simulation tick regressed between counter windows")
        elif WindowStart > LastTick:
            ResetGaps.append((LastTick-FirstTick,WindowStart-LastTick))
        LastTick = WindowStart
        WindowsCount += 1
        Count = Total = Maximum = 0
        while Count < 48 and Observed < Args.frames:
            if time.monotonic() > Deadline:
                raise TimeoutError("Timed EditorHost stopped progressing")
            time.sleep(Args.poll_ms / 1000)
            AbsoluteTick,State = Snapshot()
            Queries += 1
            Current = State["Samples"]
            Stats = State["Phases"]["EngineStep"]
            NewTotal, NewMax = Stats["TotalNanoseconds"], Stats["MaximumNanoseconds"]
            Gap, Duration = Current - Count, NewTotal - Total
            if Current >= 60 or Gap < 0 or Duration < 0 or NewMax < Maximum or AbsoluteTick-LastTick!=Gap:
                raise RuntimeError("Cannot establish the number of stepped frames")
            if Gap == 0:
                if Duration or NewMax != Maximum:
                    raise ValueError("Native duration changed without a completed step")
            elif Gap == 1:
                if Duration <= 0 or max(Maximum, Duration) != NewMax:
                    raise ValueError("Native rolling maximum contradicts the single-step delta")
                Ticks.append(Duration)
            else:
                if Duration <= 0 or Duration > Gap * NewMax:
                    raise ValueError("Unobserved step group has an impossible total")
                Groups.append((LastTick-FirstTick, Gap, Duration))
            Observed += Gap
            LastTick = AbsoluteTick
            Count, Total, Maximum = Current, NewTotal, NewMax
        for Name in Phases:
            PhaseTotals[Name] += State["Phases"][Name]["TotalNanoseconds"]
    ProcessCpu, MainCpu = Win.Times(Process, False)[1] - ProcessStart, Win.Times(Main, True)[1] - MainStart
    Wall = time.perf_counter_ns() - WallStart
    Win.VerifyAffinity(Process, Main, Args.engine_mask, Args.main_cpu)
    Priority = Win.Kernel.GetPriorityClass(Process)
    Win.Check(Priority)
    PollMissing = sum(Gap for _, Gap, _ in Groups)
    ResetMissing = sum(Gap for _, Gap in ResetGaps)
    Missing = PollMissing + ResetMissing
    Attempted = LastTick-FirstTick
    if (len(Ticks) + PollMissing != Observed or len(Ticks) + Missing != Attempted or
            sum(Ticks) + sum(Sum for _, _, Sum in Groups) != PhaseTotals["EngineStep"]):
        raise ValueError("Native steps or durations are unaccounted for")
    if not Ticks:
        raise ValueError("No individually timed steps")
    Sorted = sorted(Ticks)
    Rank = math.ceil(Attempted * .95)
    return dict(PriorityClass=Priority, Warmup=Warm, StartSimulationTick=FirstTick, EndSimulationTick=LastTick,
                Attempted=Attempted, RingTicks=Observed, Exact=len(Ticks), Missing=Missing,
                PollMissing=PollMissing, ResetMissing=ResetMissing, Gaps=len(Groups), ResetGaps=len(ResetGaps),
                Windows=WindowsCount, Queries=Queries, WallSeconds=round(Wall/1e9,4),
                MainCpuMs=round(MainCpu/1e6,3), ProcessCpuMs=round(ProcessCpu/1e6,3),
                P50Ns=Quantile(Ticks,.5), P95Ns=Quantile(Ticks,.95), P99Ns=Quantile(Ticks,.99),
                LowerP95Ns=Sorted[max(0,Rank-Missing-1)] if Rank>Missing else 0,
                UpperP95Ns=Sorted[Rank-1] if Rank<=len(Sorted) else float("inf"),
                **{Name+"Ns": Value for Name,Value in PhaseTotals.items()}), Ticks, Groups, ResetGaps


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    for Name in ("baseline", "current", "output"):
        Parser.add_argument("--"+Name,type=Path,required=True)
    Parser.add_argument("--blocks",type=int,default=64,help="Number of symmetric ABBA or BAAB four-process blocks")
    Parser.add_argument("--frames",type=int,default=1024)
    Parser.add_argument("--warmup",type=int,default=192)
    Parser.add_argument("--poll-ms",type=float,default=4)
    Parser.add_argument("--scene",choices=("ordinary","rigid"),default="ordinary")
    Parser.add_argument("--engine-mask",type=lambda Value:int(Value,0),default=0xffff0000)
    Parser.add_argument("--main-cpu",type=int,default=24)
    Parser.add_argument("--controller-cpu",type=int,default=8)
    Parser.add_argument("--seed",type=int,default=20261127)
    Args = Parser.parse_args()
    if not 1<=Args.blocks<=256 or not 48<=Args.frames<=8192 or not 48<=Args.warmup<=1024 or not 1<=Args.poll_ms<=6:
        Parser.error("Bounds are 1–256 blocks, 48–8192 timed steps, 48–1024 warmup steps and 1–6 ms polling")
    Win = Windows()
    Layout = Win.Layout()
    Cache = {Logical:Last for Group,Logical,_,Last in Layout if Group==0}
    if Args.main_cpu not in Cache or Args.controller_cpu not in Cache or not Args.engine_mask & (1<<Args.main_cpu) or any(
            Value!=Cache[Args.main_cpu] for Logical,Value in Cache.items() if Args.engine_mask & (1<<Logical)) or (
            Cache[Args.main_cpu]==Cache[Args.controller_cpu] or Args.engine_mask & (1<<Args.controller_cpu)):
        Parser.error("Choose one Engine last-level cache and a controller logical CPU on another")
    print("CPU sets (group, logical, core, last-level cache):",Layout,flush=True)
    Win.Check(Win.Kernel.SetProcessAffinityMask(Win.Kernel.GetCurrentProcess(),1<<Args.controller_cpu))
    Paths = {"baseline":Args.baseline.resolve(strict=True),"current":Args.current.resolve(strict=True)}
    Hashes = {Name:hashlib.sha256(PathValue.read_bytes()).hexdigest() for Name,PathValue in Paths.items()}
    Starts = ["baseline","current"]*(Args.blocks//2) + (["baseline"] if Args.blocks%2 else [])
    random.Random(Args.seed).shuffle(Starts)
    Args.output.mkdir(parents=True,exist_ok=True)
    with (Args.output/"Processes.csv").open("x",encoding="utf-8",newline="") as Summary, gzip.open(
            Args.output/"Ticks.csv.gz","xt",encoding="utf-8",newline="") as Raw, tempfile.TemporaryDirectory(prefix="gpp-") as Projects:
        Table = None
        Samples = csv.writer(Raw)
        Samples.writerow(("Block","Run","Profile","StepNanoseconds","OmittedGroupsStartCountTotal","ResetGapsStartCount"))
        for Block,First in enumerate(Starts,1):
            Second = "current" if First=="baseline" else "baseline"
            for Run,Name in enumerate((First,Second,Second,First),1):
                Result,Ticks,Groups,ResetGaps = Measure(Win,Paths[Name],Path(Projects)/f"B{Block}R{Run}",Args)
                Row = dict(Scene=Args.scene,Block=Block,Run=Run,Profile=Name,EngineSHA256=Hashes[Name],**Result)
                if Table is None:
                    Table=csv.DictWriter(Summary,fieldnames=Row.keys())
                    Table.writeheader()
                Table.writerow(Row)
                Samples.writerow((Block,Run,Name," ".join(map(str,Ticks)),
                                  " ".join(f"{Start}:{Gap}:{Total}" for Start,Gap,Total in Groups),
                                  " ".join(f"{Start}:{Gap}" for Start,Gap in ResetGaps)))
                Raw.flush()
                Summary.flush()
                print(f"block {Block}/{Args.blocks} run {Run} {Name} p95={Result['P95Ns']/1000:g} us "
                      f"excluded={Result['Missing']}/{Result['Attempted']}",flush=True)


if __name__ == "__main__":
    Main()
