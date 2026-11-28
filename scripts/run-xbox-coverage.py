#!/usr/bin/env python3
"""Run the Xbox test ISO and recover its coverage and test results from xemu."""

import argparse
import os
import re
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path


RESULT = re.compile(rb"^SUNSHINE_TEST_DONE ([01]) (\d+)\r?\n", re.MULTILINE)
GUEST_TEST_DIR = "tests/xbox/guest"


def run_xemu(build, output):
    """Wait for a complete guest report, keeping emulator logs on every exit."""
    launcher = Path(__file__).with_name("run-xemu.sh").resolve()
    shell = shutil.which("bash")
    if not shell:
        raise ValueError("Bash is needed to run the xemu launcher")
    serial = output / "serial.log"
    with (output / "xemu.log").open("wb") as log:
        command = [
            shell, launcher.as_posix(), "--iso", (build / "SunshineTests.iso").as_posix(),
            "--test-output", output.as_posix(), "--network", "none",
        ]
        with subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log, stderr=log) as xemu:
            try:
                deadline = time.monotonic() + 120
                while time.monotonic() < deadline:
                    if serial.is_file():
                        transcript = serial.read_bytes()
                        if RESULT.search(transcript):
                            return transcript
                    if xemu.poll() is not None:
                        raise ValueError(f"xemu stopped before guest tests completed (exit {xemu.returncode})")
                    time.sleep(0.25)
                raise ValueError("Timed out waiting for guest tests and coverage")
            finally:
                if xemu.poll() is None:
                    if os.name == "nt":
                        # MSYS2 exec waits for the native xemu process as a child.
                        subprocess.run(
                            ["taskkill", "/pid", str(xemu.pid), "/t", "/f"],
                            stdout=log, stderr=log, check=False,
                        )
                    else:
                        xemu.terminate()
                    try:
                        xemu.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        xemu.kill()


def write_test_results(transcript, output):
    """Write JUnit from the guest's completed tests and return its exit status."""
    status, expected = map(int, RESULT.search(transcript).groups())
    cases = re.findall(rb"^SUNSHINE_TEST_CASE (PASS|FAIL|SKIP) (\S+) (\S+)\r?$", transcript, re.MULTILINE)
    if not cases or len(cases) != expected or len({(suite, name) for _, suite, name in cases}) != expected:
        raise ValueError(f"Received {len(cases)} test records, expected {expected} unique records")

    results = ET.Element("testsuites")
    suite = ET.SubElement(results, "testsuite", name="Original Xbox in xemu", tests=str(len(cases)))
    for result, classname, name in cases:
        test = ET.SubElement(suite, "testcase", classname=classname.decode("ascii"), name=name.decode("ascii"))
        if result == b"FAIL":
            ET.SubElement(test, "failure", message="See serial.log for assertion output")
        elif result == b"SKIP":
            ET.SubElement(test, "skipped")
        print(f"{result.decode()} {classname.decode()}.{name.decode()}")
    suite.set("failures", str(sum(result == b"FAIL" for result, _, _ in cases)))
    suite.set("skipped", str(sum(result == b"SKIP" for result, _, _ in cases)))
    ET.ElementTree(results).write(output / "test_results.xml", encoding="utf-8", xml_declaration=True)
    if not any(result == b"PASS" for result, _, _ in cases):
        raise ValueError("No Xbox guest test passed")
    return status or any(result == b"FAIL" for result, _, _ in cases)


def extract_coverage(transcript, object_dir):
    """Pair complete gcda files from the guest with their build's gcno files."""
    note_files = list(object_dir.rglob("*.gcno"))
    notes = {path.with_suffix(".gcda").name: path for path in note_files}
    if not notes or len(notes) != len(note_files):
        raise ValueError("Expected uniquely named coverage notes from the instrumented build")
    data_files = {}
    current = None
    for line in transcript.splitlines():
        tag, _, payload = line.partition(b" ")
        if tag == b"SUNSHINE_GCOV_BEGIN":
            current_name = payload.decode("ascii")
            if current is not None or current_name not in notes or current_name in data_files:
                raise ValueError(f"Unexpected gcda stream for {current_name}")
            current = bytearray()
        elif tag == b"SUNSHINE_GCOV_DATA" and current is not None:
            current.extend(bytes.fromhex(payload.decode("ascii")))
        elif tag == b"SUNSHINE_GCOV_END" and current is not None:
            note = notes[current_name]
            if len(current) < 20 or current[:12] != b"adcg" + note.read_bytes()[4:12] or any(current[-8:]):
                raise ValueError(f"Invalid or mismatched gcda stream for {note.name}")
            data_files[current_name] = current
            current = None
        elif tag.startswith(b"SUNSHINE_GCOV_"):
            raise ValueError(f"Unexpected coverage record: {line!r}")
    if current is not None or data_files.keys() != notes.keys():
        raise ValueError(f"Incomplete guest coverage, missing: {sorted(notes.keys() - data_files.keys())}")
    for name, data in data_files.items():
        notes[name].with_suffix(".gcda").write_bytes(data)


def generate_report(object_dir, output, llvm_cov):
    """Export LCOV and require executed lines in both Xbox timing adapters."""
    report = output / "coverage.info"
    temporary = report.with_suffix(".tmp")
    source_root = Path(__file__).resolve().parents[1]
    subprocess.run(
        [
            sys.executable, "-m", "gcovr", str(object_dir), "--root", str(source_root),
            "--gcov-executable", f"{llvm_cov} gcov", "--filter", "src/",
            "--exclude-noncode-lines", "--exclude-throw-branches", "--exclude-unreachable-branches",
            "--lcov", str(temporary),
        ],
        cwd=source_root,
        check=True,
    )
    expected = {"src/_nxdk_compat/include/sunshine_time.h", "src/_nxdk_compat/runtime.c"}
    source = ""
    records = []
    for line in temporary.read_text(encoding="utf-8").splitlines():
        if line.startswith("SF:"):
            path = Path(line[3:])
            source = (source_root / path).relative_to(source_root).as_posix()
            line = f"SF:{source}"
        elif re.match(r"DA:\d+,[1-9]\d*(?:,|$)", line):
            expected.discard(source)
        records.append(line)
    if expected:
        raise ValueError(f"Missing executed lines in Xbox sources: {sorted(expected)}")
    temporary.write_text("\n".join(records) + "\n", encoding="utf-8", newline="\n")
    temporary.replace(report)


def main():
    """Collect one guest run from the Xbox child build."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path, help="Xbox child build directory containing SunshineTests.iso")
    parser.add_argument("--llvm-cov", default="llvm-cov", help="llvm-cov matching the nxdk Clang compiler")
    args = parser.parse_args()
    build = args.build.resolve()
    output = build / "xemu-coverage"
    output.mkdir(exist_ok=True)
    for name in ("serial.log", "test_results.xml", "coverage.info"):
        (output / name).unlink(missing_ok=True)
    object_dir = build / GUEST_TEST_DIR / "CMakeFiles/sunshine_xbox_coverage.dir"
    for data in object_dir.rglob("*.gcda"):
        data.unlink()
    try:
        transcript = run_xemu(build, output)
        failure = write_test_results(transcript, output)
        extract_coverage(transcript, object_dir)
        generate_report(object_dir, output, args.llvm_cov)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{error}. See {output / 'xemu.log'} and {output / 'serial.log'}.\n")
    return failure


if __name__ == "__main__":
    raise SystemExit(main())
