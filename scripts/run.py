#!/usr/bin/env python3
"""Authoritative retained runs. Uses the existing C++ verification and benchmark tools."""
import argparse
import hashlib
import json
import os
import pathlib
import platform
import resource
import shutil
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]


def sha(path):
    digest = hashlib.sha256()
    with pathlib.Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def source_manifest():
    files = [ROOT / "CMakeLists.txt"]
    for directory in ("include", "src", "tests", "tools", "bench", "fuzz", "scripts"):
        files += [p for p in (ROOT / directory).rglob("*") if p.is_file() and "__pycache__" not in p.parts]
    manifest = {str(p.relative_to(ROOT)): sha(p) for p in sorted(files)}
    identity = hashlib.sha256(json.dumps(manifest, sort_keys=True).encode()).hexdigest()
    return identity, manifest


def conditions():
    result = {"load_average": os.getloadavg(), "cpu_count": os.cpu_count()}
    for name, command in (("uptime", ["uptime"]), ("memory", ["vm_stat"] if sys.platform == "darwin" else ["cat", "/proc/meminfo"])):
        try:
            result[name] = subprocess.check_output(command, text=True, stderr=subprocess.STDOUT)
        except (OSError, subprocess.CalledProcessError) as error:
            result[name] = str(error)
    command = ["sysctl", "machdep.cpu.brand_string", "hw.memsize", "hw.physicalcpu", "hw.logicalcpu"] if sys.platform == "darwin" else ["lscpu"]
    try:
        result["hardware"] = subprocess.check_output(command, text=True, stderr=subprocess.STDOUT)
    except (OSError, subprocess.CalledProcessError) as error:
        result["hardware"] = str(error)
    return result


def verify_configure_command(build, config, sanitize, compiler=None, portable_sha256=False):
    command = ["cmake", "-S", ROOT, "-B", build,
               "-DCMAKE_BUILD_TYPE=" + config,
               "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
               "-DMSIM_BUILD_TESTS=ON",
               "-DMSIM_SANITIZE=" + sanitize,
               "-DMSIM_PORTABLE_SHA256=" + ("ON" if portable_sha256 else "OFF")]
    if compiler:
        command += ["-DCMAKE_CXX_COMPILER=" + compiler]
    return command


def require_test_inventory(text, expected=None):
    try:
        tests = json.loads(text).get("tests")
    except (AttributeError, json.JSONDecodeError) as error:
        raise RuntimeError("CTest inventory is not valid JSON") from error
    if not isinstance(tests, list) or not tests:
        raise RuntimeError("CTest inventory is empty; verification ran no tests")
    names=[entry.get('name') for entry in tests if isinstance(entry,dict)]
    if expected is None:
        groups=json.loads((ROOT/'tests/inventory.json').read_text())
        declared=[name for group in groups.values() for name in group]
        if len(declared)!=len(set(declared)):raise RuntimeError('duplicate declared test obligation')
        expected=set(declared)
    if len(names)!=len(tests) or len(names)!=len(set(names)) or set(names)!=set(expected):
        raise RuntimeError('CTest inventory differs from declared obligations')
    return len(tests)


class Run:
    def __init__(self, output, build=None, input_path=None):
        self.output = pathlib.Path(output).resolve()
        self.output.mkdir(parents=True, exist_ok=False)
        self.identity, manifest = source_manifest()
        (self.output / "source-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        self.meta = {"schema": 1, "source_id": self.identity, "system": platform.system(), "release": platform.release(), "architecture": platform.machine(), "python": platform.python_version(), "commands": [], "conditions_before": conditions(), "status": "RUNNING"}
        for label, command in (("git_head", ["git", "rev-parse", "HEAD"]), ("git_status", ["git", "status", "--short"])):
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
            self.meta[label] = result.stdout if result.returncode == 0 else "unavailable: " + result.stderr
        if input_path:
            path = pathlib.Path(input_path).resolve()
            self.meta["input"] = {"path": str(path), "bytes": path.stat().st_size, "sha256": sha(path)}
        self.build = pathlib.Path(build).resolve() if build else None
        self.save()

    def save(self):
        temporary = self.output / "run.json.tmp"
        temporary.write_text(json.dumps(self.meta, indent=2) + "\n")
        temporary.replace(self.output / "run.json")

    def command(self, label, command, expected_failure=None):
        args = [str(x) for x in command]
        stdout, stderr = self.output / (label + ".stdout"), self.output / (label + ".stderr")
        entry = {"label": label, "argv": args, "cwd": str(ROOT), "conditions": conditions()}
        executable = pathlib.Path(shutil.which(args[0]) or args[0]).resolve()
        if executable.is_file():
            entry["executable_sha256"] = sha(executable)
        started = time.monotonic()
        with stdout.open("wb") as out, stderr.open("wb") as err:
            child = subprocess.Popen(args, cwd=ROOT, stdout=out, stderr=err)
            _, status, usage = os.wait4(child.pid, 0)
            child.returncode = os.waitstatus_to_exitcode(status)
        entry.update(exit_code=child.returncode, wall_seconds=time.monotonic() - started, user_seconds=usage.ru_utime, system_seconds=usage.ru_stime, max_rss_bytes=usage.ru_maxrss * (1 if sys.platform == "darwin" else 1024), stdout=stdout.name, stderr=stderr.name)
        text = stdout.read_text(errors="replace") + stderr.read_text(errors="replace")
        passed = child.returncode == 0 if expected_failure is None else child.returncode != 0 and expected_failure in text
        entry["passed"] = passed
        if expected_failure:
            entry["expected_failure"] = expected_failure
        self.meta["commands"].append(entry)
        self.save()
        print(label, "PASS" if passed else "FAIL", flush=True)
        if not passed:
            raise RuntimeError(label + " failed; see " + str(stderr))
        return text

    def finish(self, passed):
        current, _ = source_manifest()
        self.meta["source_unchanged"] = current == self.identity
        if 'input' in self.meta:
            input_record=self.meta['input'];path=pathlib.Path(input_record['path'])
            self.meta['input_unchanged']=path.stat().st_size==input_record['bytes'] and sha(path)==input_record['sha256']
            passed=passed and self.meta['input_unchanged']
        if self.build and (self.build / "CMakeCache.txt").exists():
            self.meta["build_executables"] = executables(self.build)
            cache = (self.build / "CMakeCache.txt").read_text()
            self.meta["cmake_cache"] = {line.split(":", 1)[0]: line.split("=", 1)[1] for line in cache.splitlines() if line.startswith(("CMAKE_CXX_COMPILER:", "CMAKE_CXX_FLAGS", "CMAKE_C_COMPILER:", "CMAKE_C_FLAGS", "CMAKE_BUILD_TYPE:", "MSIM_BUILD_TESTS:", "MSIM_SANITIZE:", "MSIM_FUZZ:", "MSIM_PORTABLE_SHA256:", "_Python3_EXECUTABLE:")) and "=" in line}
            if (self.build / "compile_commands.json").exists():
                shutil.copy2(self.build / "compile_commands.json", self.output / "compile_commands.json")
            compiler = self.meta["cmake_cache"].get("CMAKE_CXX_COMPILER")
            if compiler:
                self.meta["compiler_version"] = subprocess.check_output([compiler, "--version"], text=True)
        self.meta["conditions_after"] = conditions()
        self.meta["status"] = "PASS" if passed and current == self.identity else "FAIL"
        self.save()
        if passed and current != self.identity:
            raise RuntimeError("source changed during run")


def execute(args):
    run = Run(args.out, getattr(args, "build", None), getattr(args, "input", None))
    passed = False
    try:
        build = pathlib.Path(args.build).resolve() if getattr(args, "build", None) else None
        if args.activity == "verify":
            configure = verify_configure_command(
                build, args.config, args.sanitize, args.compiler, args.portable_sha256)
            stamp = build / "verified-build.json"
            if stamp.exists():
                stamp.unlink()
            run.command("configure", configure)
            run.command("build", ["cmake", "--build", build, "-j", args.jobs])
            if source_manifest()[0] != run.identity:
                raise RuntimeError("source changed during build")
            inventory = run.command(
                "test-inventory", ["ctest", "--test-dir", build,
                                   "--show-only=json-v1"])
            run.meta["test_count"] = require_test_inventory(inventory)
            run.save()
            command = ["ctest", "--test-dir", build, "--output-on-failure", "--timeout", "1800", "-j", "1"]
            if args.bounded:
                command += ["-R", "^(order_book_test|flat_hash_test|ref_map_test|exhaustive_test|differential_test|itch_test|closed_loop_test|volume_test|volume_cli_test|obligation_test|feed_book_test|replay_test|replay_boundary_test|research_test|release_regression_test|benchmark_test)$"]
            run.command("tests", command)
            if args.sanitize == "thread":
                run.command("tsan-positive-control", [build / "tests/replay_race_probe"], "ThreadSanitizer: data race")
            (build / "verified-build.json").write_text(json.dumps({"source_id": run.identity, "executables": executables(build)}, indent=2) + "\n")
        elif args.activity == "exec":
            command = args.command[1:] if args.command[:1] == ["--"] else args.command
            run.command("command", command)
        elif args.activity in ("study","demo"):
            require_build_identity(build, run.identity)
            if args.activity=='demo':
                from make_demo import write
                source=run.output/'demo.itch';write(source);args.input=str(source)
                run.meta['input']={'path':str(source),'bytes':source.stat().st_size,'sha256':sha(source)};run.save()
                run.command('matching-example',[build/'book_demo'])
                run.command('bounded-reference-replay',[build/'itch_replay',source,'--reference','--threads',1,'--out',run.output/'canonical'])
            run.command("accounting", [build / "itch_volume", args.input, run.output / "report", "--partitions", args.partitions, "--budget-mib", args.budget_mib])
            run.command("independent-check", [sys.executable, ROOT / "scripts/check_volume.py", args.input, run.output / "report", run.output / "independent.json", "--partitions", args.partitions])
            expected=run.meta['input']
            accounting=json.loads((run.output/'report/summary.json').read_text())
            if accounting['input_sha256']!=expected['sha256'] or accounting['input_bytes']!=expected['bytes']:
                raise RuntimeError('accounting input identity changed')
            run.command('observations',[build/'itch_observe',args.input,run.output/'observations'])
            common=['--accounting',run.output/'report','--accounting-check',run.output/'independent.json']
            run.command('research',[sys.executable,ROOT/'scripts/research.py','--observations',run.output/'observations','--out',run.output/'research',*common])
            run.command('raw-state-and-research',[sys.executable,ROOT/'scripts/research.py','--raw',args.input,'--observations-check',run.output/'observations','--out',run.output/'raw-research',*common])
            primary=json.loads((run.output/'research/summary.json').read_text())
            reference=json.loads((run.output/'raw-research/summary.json').read_text())
            if primary!=reference:raise RuntimeError('raw-byte research reconstruction disagrees')
            run.meta['research']={'state_agreement':'every selected event before and after mutation','analytics':'shared definitions recomputed on independent raw-byte state; hand-ledger tests are separate','summary':primary}
        passed = True
    finally:
        run.finish(passed)


def executables(build):
    return {str(p.relative_to(build)): sha(p) for directory in (build, build / "tests") if directory.exists() for p in directory.iterdir() if p.is_file() and os.access(p, os.X_OK) and p.suffix not in (".cmake", ".txt", ".json")}


def require_build_identity(build, identity):
    stamp = build / "verified-build.json"
    if not stamp.exists():
        raise RuntimeError("verified build is missing; run the verify activity first")
    verified = json.loads(stamp.read_text())
    if verified["source_id"] != identity or verified["executables"] != executables(build):
        raise RuntimeError("build content differs; run the verify activity first")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="activity", required=True)
    for name in ("verify", "exec", "study", "demo"):
        p = sub.add_parser(name)
        p.add_argument("--out", required=True)
        if name != "exec":
            p.add_argument("--build", required=True)
        else:
            p.add_argument("--build")
            p.add_argument("--input")
            p.add_argument("command", nargs=argparse.REMAINDER)
        if name == "verify":
            p.add_argument("--config", choices=("Debug", "Release"), default="Debug")
            p.add_argument("--sanitize", default="")
            p.add_argument("--compiler")
            p.add_argument("--jobs", default="4")
            p.add_argument("--bounded", action="store_true")
            p.add_argument("--portable-sha256", action="store_true")
        if name == "study":p.add_argument("--input", required=True)
        if name in ("study","demo"):
            p.add_argument("--partitions", default="16")
            p.add_argument("--budget-mib", default="64")
    try:
        execute(parser.parse_args())
    except Exception as error:
        print("run failed:", error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
