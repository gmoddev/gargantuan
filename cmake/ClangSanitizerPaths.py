# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0

# Clang's sanitizer pass records the actual input name in executable data,
# independently of the paths used by the preprocessor and DWARF.
import os
import re
import shlex
import subprocess
import sys
import tempfile
from contextlib import ExitStack
from pathlib import Path


def MakeName(Name):
    return os.fsencode(Name.replace("$", "$$").replace("#", r"\#").replace(" ", r"\ "))


def Compile():
    AliasDirectory = Path(sys.argv[1])
    Separator = sys.argv.index("--gargantuan-compiler")
    Command = [*sys.argv[2:Separator], sys.argv[Separator + 1]]
    Build = AliasDirectory.parent.parent
    Roots = {str(Build): os.path.relpath(Build)}
    OriginalNames = {}
    for Alias in AliasDirectory.iterdir():
        if Alias.is_symlink():
            Source = os.readlink(Alias)
            Local = os.path.relpath(Alias)
            OriginalNames[Local] = Source
            for Root in (Source, os.path.realpath(Source)):
                Roots[Root] = Local
    OrderedRoots = sorted(Roots, key=len, reverse=True)

    def InputName(Value):
        Absolute = os.path.abspath(Value)
        for Root in OrderedRoots:
            if Absolute == Root or Absolute.startswith(Root + "/"):
                return os.path.normpath(os.path.join(Roots[Root], os.path.relpath(Absolute, Root)))
        return Value

    Depfile = None
    IncludeOptions = ("-isystem", "-iquote", "-idirafter", "-imacros", "-include", "-I")
    JoinedOptions = ("-fmodule-map-file=", *IncludeOptions)
    with ExitStack() as Files:
        def Arguments(Values):
            nonlocal Depfile
            Result = []
            Iterator = iter(Values)
            for Value in Iterator:
                if Value.startswith("@"):
                    with open(Value[1:], encoding="utf-8") as Input:
                        Nested = Arguments(shlex.split(Input.read()))
                    Response = Files.enter_context(tempfile.NamedTemporaryFile(mode="w", encoding="utf-8"))
                    Response.write(shlex.join(Nested))
                    Response.flush()
                    Result.append("@" + Response.name)
                elif Value in ("-MT", "-MQ", "-o", "-MF"):
                    Operand = next(Iterator)
                    if Value == "-MF":
                        Depfile = Path(Operand)
                    Result.extend((Value, Operand))
                elif Value in IncludeOptions:
                    Result.extend((Value, InputName(next(Iterator))))
                elif not Value.startswith("-") and os.path.isfile(Value):
                    Result.append(InputName(Value))
                else:
                    for Option in JoinedOptions:
                        if Value.startswith(Option):
                            Operand = Value[len(Option):]
                            if Operand and not Operand.startswith(("-", "=")):
                                Value = Option + InputName(Operand)
                                break
                    Result.append(Value)
            return Result

        CompilerArguments = Arguments(sys.argv[Separator + 2:])
        for Root in OrderedRoots:
            CompilerArguments.append("-ffile-prefix-map=" + Root + "=" + Roots[Root])
        CompilerArguments.append("-fdebug-compilation-dir=.")
        Status = subprocess.call([*Command, *CompilerArguments])
    if Status == 0 and Depfile and Depfile.exists():
        # Keep generated-output identities identical to the build graph, including
        # when generated headers live under a mapped external source directory.
        Before = Depfile.read_bytes()
        After = Before
        for Local, Source in OriginalNames.items():
            Start = rb"(?<![^\t\n ])" + re.escape(MakeName(Local + "/"))
            After = re.sub(Start, lambda Match: MakeName(Source + "/"), After)
        if Before != After:
            Depfile.write_bytes(After)
    return Status


if __name__ == "__main__":
    sys.exit(Compile())
