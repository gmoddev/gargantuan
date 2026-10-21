"""Exercise CTest's real working-directory behavior, without building native code."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
OUTPUTS = {
    "native-ci.yml": ["build-ci/ctest-results.xml", "build-sanitizers/ctest-results.xml"],
    "gns-sanitizers.yml": ["build-gns-sanitizers/ctest-gns-results.xml"],
}


class CIJUnitTests(unittest.TestCase):
    def test_event_policy_retains_pr_checks_without_branch_push_duplicates(self):
        for Name in OUTPUTS:
            Text = (ROOT / '.github/workflows' / Name).read_text()
            Events = Text.split('\non:\n', 1)[1].split('\npermissions:', 1)[0]
            self.assertRegex(Events, r'(?m)^  push:\n    branches: \[main\]$')
            self.assertRegex(Events, r'(?m)^  pull_request:$')
            self.assertRegex(Events, r'(?m)^  workflow_dispatch:$')
            self.assertNotIn('paths:', Events)
            self.assertNotIn('paths-ignore:', Events)
            self.assertIn("cancel-in-progress: ${{ github.event_name == 'pull_request' }}", Text)
            self.assertIn('${{ github.event_name }}-${{ github.event.pull_request.number || github.run_id }}', Text)
            self.assertNotIn('github.ref', Text.split('\nconcurrency:', 1)[1].split('\njobs:', 1)[0])
        # Public docs deployment is independent of draft-PR evidence visibility.
        Docs = (ROOT / '.github/workflows/docs.yml').read_text()
        self.assertIn('branches: [main]', Docs)
        self.assertNotIn('pull_request:', Docs)

    def test_independent_jobs_keep_their_required_check_and_artifact_names(self):
        Native = (ROOT / '.github/workflows/native-ci.yml').read_text()
        Linux = Native.split('  linux-sanitizers:\n', 1)[1]
        Gns = (ROOT / '.github/workflows/gns-sanitizers.yml').read_text()
        self.assertNotIn('needs:', Linux)
        self.assertNotIn('download-artifact', Linux)
        for Name in ('Windows x64 / MSVC 19.50+ / Release',
                     'Ubuntu 24.04 / Clang 19 / ASan + UBSan / Headless'):
            self.assertIn(Name, Native)
        self.assertIn('Ubuntu 24.04 / Clang 19 / ASan + UBSan + LSan / GNS', Gns)
        for Text, Artifacts in ((Native, ('native-ci-diagnostics', 'sanitizer-ci-diagnostics')),
                                (Gns, ('gns-sanitizer-diagnostics',))):
            for Artifact in Artifacts:
                self.assertIn('name: ' + Artifact, Text)
            self.assertIn('if: always()', Text)
            self.assertIn('qualified-source-commit.txt', Text)
            self.assertIn('qualification-contract.txt', Text)

    def test_capture_mocks_have_one_ctest_owner_and_reproducer_is_explicit(self):
        Native = (ROOT / '.github/workflows/native-ci.yml').read_text()
        CMake = (ROOT / 'CMakeLists.txt').read_text()
        for Script in ('TestCaptureHooks.ps1', 'TestDumpcapFarm32Capture.ps1'):
            self.assertEqual(CMake.count(Script), 1)
            self.assertNotIn(Script, Native)
        self.assertIn('gargantuan_farm32_worker_capture_hook', CMake)
        self.assertIn('gargantuan_farm32_client_capture_source', CMake)
        self.assertIn('add_executable(gargantuan_gns_service_reproducer EXCLUDE_FROM_ALL', CMake)

    def test_qualified_envelope_checks_native_projection_after_sidecar(self):
        Text = (ROOT / '.github/workflows/native-ci.yml').read_text()
        Step = Text.split('- name: Package qualified 32-client physical candidate', 1)[1].split(
            '- name: Upload qualified 32-client physical candidate', 1)[0]
        Sidecar = Step.index("WriteAllText((Join-Path $RoleRoot 'deployment-sha256.json')")
        Projection = Step.index('& ./tests/NewPhysicalGameSessionFarmProjection.ps1')
        Native = Step.index("'build-ci/gargantuan-packager.exe') validate $RuntimeRoot")
        self.assertLess(Sidecar, Projection)
        self.assertLess(Projection, Native)
        self.assertIn("'build-ci/QualifiedScaleRuntimeCheck'", Step)
        self.assertIn('-DeploymentSha256 $DeploymentPin -SourceCommit $env:GITHUB_SHA', Step)
        self.assertIn('if ($LASTEXITCODE -ne 0)', Step[Native:])

    def test_all_workflow_outputs_are_absolute_and_match_uploaded_members(self):
        for Name, Expected in OUTPUTS.items():
            Text = (ROOT / ".github/workflows" / Name).read_text()
            Values = re.findall(r'--output-junit\s+("[^"\n]+"|\S+)', Text)
            self.assertEqual(len(Values), len(Expected), Name)
            for Value, Relative in zip(Values, Expected):
                self.assertIn(Value, ('"$GITHUB_WORKSPACE/' + Relative + '"',
                                      '"$env:GITHUB_WORKSPACE/' + Relative + '"'))
                self.assertRegex(Text, r'(?m)^\s+' + re.escape(Relative) + r'\s*$')
                if Relative.startswith("build-ci/"):
                    self.assertIn("Test-Path -LiteralPath " + Relative + " -PathType Leaf", Text)
                    self.assertIn("(Get-Item -LiteralPath " + Relative + ").Length -eq 0", Text)
                else:
                    self.assertIn('test -s "$GITHUB_WORKSPACE/' + Relative + '"', Text)

    def test_ctest_absolute_output_survives_test_directory_change_and_failure(self):
        CTest = shutil.which("ctest")
        if CTest is None:
            self.skipTest("CTest is unavailable; hosted CI runs the real subprocess regression")
        Version = subprocess.run([CTest, "--version"], check=True, capture_output=True, text=True, timeout=10)
        Match = re.search(r'ctest version (\d+)\.(\d+)', Version.stdout)
        if not Match or tuple(map(int, Match.groups())) < (3, 21):
            self.skipTest("CTest 3.21+ required for --output-junit")
        with tempfile.TemporaryDirectory(prefix="ctest junit path ") as Directory:
            Workspace = Path(Directory)
            Build = Workspace / "build-ci"
            Build.mkdir()
            # Paths with spaces exercise argument handling independently of the
            # CI shell. No configure/build, network, or production probe runs.
            (Build / "CTestTestfile.cmake").write_text(
                'add_test(Passing "' + Path(sys.executable).as_posix() + '" "-c" "print(123)")\n'
                'add_test(Failing "' + Path(sys.executable).as_posix() + '" "-c" "raise SystemExit(1)")\n')
            Output = Build / "ctest-results.xml"
            Command = [CTest, "--test-dir", str(Build), "--no-tests=error", "--output-junit", str(Output)]
            Flags = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}
            # Reproduce the historical green-CTest/missing-upload shape first.
            Legacy = subprocess.run([CTest, "--test-dir", str(Build), "--no-tests=error",
                                     "--output-junit", "build-ci/legacy-results.xml", "-R", "^Passing$"],
                                    cwd=Workspace, capture_output=True, text=True, timeout=20, **Flags)
            self.assertEqual(Legacy.returncode, 0, Legacy.stdout + Legacy.stderr)
            self.assertFalse((Build / "legacy-results.xml").exists())
            self.assertTrue((Build / "build-ci/legacy-results.xml").is_file())
            Result = subprocess.run(Command + ["-R", "^Passing$"], cwd=Workspace,
                                    capture_output=True, text=True, timeout=20, **Flags)
            self.assertEqual(Result.returncode, 0, Result.stdout + Result.stderr)
            self.assertTrue(Output.is_file())
            self.assertEqual([Case.get("name") for Case in ET.parse(Output).findall(".//testcase")], ["Passing"])
            self.assertFalse((Build / "build-ci/ctest-results.xml").exists())
            Result = subprocess.run(Command + ["-R", "^Failing$"], cwd=Workspace,
                                    capture_output=True, text=True, timeout=20, **Flags)
            self.assertNotEqual(Result.returncode, 0)
            Cases = ET.parse(Output).findall(".//testcase")
            self.assertEqual([Case.get("name") for Case in Cases], ["Failing"])
            self.assertIsNotNone(Cases[0].find("failure"))


if __name__ == "__main__":
    unittest.main()
