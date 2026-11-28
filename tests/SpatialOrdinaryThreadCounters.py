# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Exercise the same Windows thread counters that surround each private diagnostic Engine step."""
import argparse
import csv
import ctypes
from pathlib import Path
import time


class Counter(ctypes.Structure):
    _fields_ = [("Type", ctypes.c_uint32), ("Reserved", ctypes.c_uint32), ("Value", ctypes.c_uint64)]


class Performance(ctypes.Structure):
    _fields_ = [("Size", ctypes.c_uint16), ("Version", ctypes.c_uint8), ("HwCountersCount", ctypes.c_uint8),
                ("ContextSwitchCount", ctypes.c_uint32), ("WaitReasonBitMap", ctypes.c_uint64),
                ("CycleTime", ctypes.c_uint64), ("RetryCount", ctypes.c_uint32), ("Reserved", ctypes.c_uint32),
                ("HwCounters", Counter*16)]


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--output", type=Path, required=True)
    Args = Parser.parse_args()
    Kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    Handle = ctypes.c_void_p()
    Kernel.GetCurrentThread.restype = ctypes.c_void_p
    Kernel.EnableThreadProfiling.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint64,
                                             ctypes.POINTER(ctypes.c_void_p)]
    Kernel.ReadThreadProfilingData.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(Performance)]
    Kernel.DisableThreadProfiling.argtypes = [ctypes.c_void_p]
    Kernel.SetThreadAffinityMask.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    Kernel.SetThreadAffinityMask.restype = ctypes.c_size_t
    Thread = Kernel.GetCurrentThread()
    BeforeMask = Kernel.SetThreadAffinityMask(Thread, 1<<24)
    if not BeforeMask:
        raise ctypes.WinError(ctypes.get_last_error())
    def Check(Status):
        if Status:
            raise ctypes.WinError(Status)
    def Read():
        Value = Performance(Size=ctypes.sizeof(Performance), Version=1)
        Check(Kernel.ReadThreadProfilingData(Handle, 3, ctypes.byref(Value)))
        return Value
    try:
        Check(Kernel.EnableThreadProfiling(Thread, 1, 0xffff, ctypes.byref(Handle)))
        with Args.output.open("x", encoding="utf-8", newline="") as File:
            Table = csv.writer(File)
            Table.writerow(("Trial","Work","WallNs","ScheduledCycles","ContextSwitches","WaitReasonBitmap",
                            "MappedHardwareCounters", *("Hardware"+str(I) for I in range(16))))
            for Trial in range(1, 5):
                for Work in ("Sleep5ms", "Busy1.5ms"):
                    First = Read()
                    Start = time.perf_counter_ns()
                    if Work == "Sleep5ms":
                        time.sleep(.005)
                    else:
                        while time.perf_counter_ns() - Start < 1_500_000:
                            pass
                    End = time.perf_counter_ns()
                    Last = Read()
                    Table.writerow((Trial, Work, End - Start, Last.CycleTime - First.CycleTime,
                                    Last.ContextSwitchCount - First.ContextSwitchCount, Last.WaitReasonBitMap,
                                    Last.HwCountersCount, *(Y.Value-X.Value for X,Y in zip(First.HwCounters,Last.HwCounters))))
    finally:
        if Handle: Check(Kernel.DisableThreadProfiling(Handle))
        if not Kernel.SetThreadAffinityMask(Thread, BeforeMask):
            raise ctypes.WinError(ctypes.get_last_error())


if __name__ == "__main__":
    Main()
