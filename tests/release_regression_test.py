#!/usr/bin/env python3
"""Release-contract regressions that do not require a second project build."""
import importlib.util
import json
import pathlib
import re
import sys

root, runner = map(pathlib.Path, sys.argv[1:])
spec = importlib.util.spec_from_file_location("retained_runner", runner)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

command = module.verify_configure_command(
    pathlib.Path("/tmp/msim-release-regression"), "Release", "")
assert "-DMSIM_BUILD_TESTS=ON" in command
assert "-DMSIM_PORTABLE_SHA256=OFF" in command
portable = module.verify_configure_command(
    pathlib.Path("/tmp/msim-release-regression"), "Release", "", portable_sha256=True)
assert "-DMSIM_PORTABLE_SHA256=ON" in portable
assert module.require_test_inventory(json.dumps({"tests": [{"name": "one"}]}), {"one"}) == 1
for empty in ("{}", '{"tests": []}', "not JSON"):
    try:
        module.require_test_inventory(empty)
    except RuntimeError:
        pass
    else:
        raise AssertionError("empty or invalid CTest inventory was accepted")

for forged in ('{"tests":[{"name":"one"},{"name":"one"}]}', '{"tests":[{"name":"unrecognized"}]}'):
    try:module.require_test_inventory(forged,{'one'})
    except RuntimeError:pass
    else:raise AssertionError('duplicate or unexpected inventory accepted')
license_text=(root/'LICENSE').read_text()
assert 'Apache License' in license_text and 'Version 2.0' in license_text
assert not (root/'lmdb/mdb.c').exists() and not (root/'include/msim/grpc_storage.hpp').exists()
notice=(root/'NOTICE').read_text()
assert 'Nicholas Stokes' in notice

cmake = (root / "CMakeLists.txt").read_text()
match = re.search(r"cmake_minimum_required\(VERSION ([0-9]+\.[0-9]+)\)", cmake)
assert match and tuple(map(int, match.group(1).split("."))) >= (3, 20)
reproduce = (root / "docs/reproduce.md").read_text()
assert f"CMake {match.group(1)} or later" in reproduce

print("exact verification inventory, source license, removed legacy dependencies, CMake floor: PASS")
