# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Trusted launcher side of the independent Host acceptance workload."""
import ctypes
from ctypes import wintypes
import json
import os
import socket
import time
from contextlib import contextmanager
from uuid import uuid4

from SpatialConstructionPackageSession import ConsoleCapture


@contextmanager
def InvitationChannel():
    if os.name != "nt":
        Sender, Receiver = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
        with Sender, Receiver:
            Receiver.settimeout(15)
            yield (Sender.fileno(), {"pass_fds": (Sender.fileno(),)}, lambda: json.loads(Receiver.recv(4096)),
                   lambda Frame: Receiver.send(json.dumps(Frame).encode("ascii")))
    else:
        yield from WindowsChannel()


def WindowsChannel():
    Native = ctypes.WinDLL("kernel32", use_last_error=True)
    def Bind(Name, Return, *Arguments):
        Method = getattr(Native, Name)
        Method.restype, Method.argtypes = Return, Arguments
        return Method
    H, D, P = wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p
    Name = r"\\.\pipe\GargantuanLifecycle-" + uuid4().hex
    Create = Bind("CreateNamedPipeW", H, wintypes.LPCWSTR, D, D, D, D, D, D, P)
    Open = Bind("CreateFileW", H, wintypes.LPCWSTR, D, D, P, D, D, H)
    Peek = Bind("PeekNamedPipe", wintypes.BOOL, H, P, D, P, ctypes.POINTER(D), P)
    Read = Bind("ReadFile", wintypes.BOOL, H, P, D, ctypes.POINTER(D), P)
    Write = Bind("WriteFile", wintypes.BOOL, H, P, D, ctypes.POINTER(D), P)
    Close = Bind("CloseHandle", wintypes.BOOL, H)
    Sender = Create(Name, 0x00080003, 0x0C, 1, 4096, 4096, 0, None)
    Invalid = ctypes.c_void_p(-1).value
    if Sender == Invalid:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        Receiver = Open(Name, 0xC0000000, 0, None, 3, 0, None)
        if Receiver == Invalid:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            os.set_handle_inheritable(Sender, True)
            Options = ConsoleCapture()
            Options["startupinfo"].lpAttributeList = {"handle_list": [Sender]}
            def Receive():
                Available, MessageBytes = D(), D()
                Deadline = time.monotonic() + 15
                while not Available.value:
                    if not Peek(Receiver, None, 0, None, ctypes.byref(Available), ctypes.byref(MessageBytes)):
                        raise ctypes.WinError(ctypes.get_last_error())
                    if time.monotonic() >= Deadline:
                        raise TimeoutError("Host did not publish guest invitations")
                    time.sleep(0.02)
                Data, Count = ctypes.create_string_buffer(4096), D()
                if not 0 < MessageBytes.value <= len(Data):
                    raise ValueError("Invalid invitation frame length")
                if not Read(Receiver, Data, MessageBytes.value, ctypes.byref(Count), None) or Count.value != MessageBytes.value:
                    raise ctypes.WinError(ctypes.get_last_error())
                return json.loads(Data.raw[:Count.value])
            def Send(Frame):
                Message, Count = json.dumps(Frame).encode("ascii"), D()
                if len(Message) > 4096:
                    raise ValueError("Oversize invitation frame")
                if not Write(Receiver, Message, len(Message), ctypes.byref(Count), None) or Count.value != len(Message):
                    raise ctypes.WinError(ctypes.get_last_error())
            yield Sender, Options, Receive, Send
        finally:
            Close(Receiver)
    finally:
        Close(Sender)
