#!/usr/bin/env python3
"""Benign flag reload and symbolic comparison controls in a boot sector."""
import argparse
import json
import os
import pathlib
import subprocess
import tempfile
import time

from symfit_system_smoke import (
    DEFAULT_BUILD_DIR, PROJECT_ROOT, SmokeFailure, X86BootSectorBuilder,
    ia_rpc_connect, send_ia_rpc, terminate_process, x86_boot_command,
    qmp_connect, send_qmp,
)


def image():
    b = X86BootSectorBuilder()
    b.emit(0xFA, 0x31, 0xC0, 0x8E, 0xD0)  # cli; xor ax,ax; mov ss,ax
    b.emit(0xBC, 0x00, 0x70, 0xB8, 0x07, 0x00)  # stack; ax=7
    b.label("compare")
    b.emit(0x83, 0xF8, 0x07)  # cmp ax,7
    b.label("reentry")
    b.emit(0x90)
    b.label("symbolic_branch")
    b.jz_short("reload")
    b.emit(0xBB, 0xAD, 0x0B)  # wrong concrete path
    b.label("reload")
    b.emit(0x68, 0x02, 0x00, 0x9D)  # push 2; popf (ZF=0)
    b.label("concrete_branch")
    b.jz_short("after_reload")
    b.emit(0xBB, 0x33, 0x00)  # must execute
    b.label("after_reload")
    b.emit(0x90)
    b.label("control_compare")
    b.emit(0x83, 0xF8, 0x07)
    b.label("control_branch")
    b.jz_short("done")
    b.emit(0xB9, 0xAD, 0x0B)
    b.label("done")
    b.emit(0xF4, 0xEB, 0xFE)
    return b.image(), b.labels


def run(binary, work, split):
    data, labels = image()
    boot = work / "boot.img"
    boot.write_bytes(data)
    env = os.environ.copy()
    env["IA_RPC_SOCKET"] = str(work / "ia.sock")
    with (work / "stdout.log").open("w") as out, (work / "stderr.log").open("w") as err:
        proc = subprocess.Popen(x86_boot_command(binary, boot, work / "serial.log",
                                                qmp_socket=work / "qmp.sock"),
                                env=env, stdout=out, stderr=err)
        try:
            with ia_rpc_connect(work / "ia.sock", proc, 30) as sock, \
                    qmp_connect(work / "qmp.sock", proc, 30) as qmp:
                req = 0
                records = []

                def call(method, params=None):
                    nonlocal req
                    req += 1
                    result = send_ia_rpc(sock, req, method, params)
                    records.append({"method": method, "result": result})
                    return result

                def stop(name):
                    result = call("resume_until_address", {"address": hex(labels[name])})
                    if result.get("matched") is not True:
                        raise SmokeFailure(f"did not reach {name}: {result}")
                    # IA publishes the match before the asynchronous VM stop.
                    # Wait for that stop to complete before requesting re-entry.
                    deadline = time.monotonic() + 30
                    while send_qmp(qmp, {"execute": "query-status"})["return"]["running"]:
                        if time.monotonic() > deadline:
                            raise SmokeFailure("VM stop did not complete")
                        time.sleep(0.01)

                stop("compare")
                symbol = call("symbolize_register", {"register": "rax"})
                assert int(symbol["label"], 16) != 0
                assert int(symbol["value"], 16) == 7
                expr = call("get_symbolic_expression", {"label": symbol["label"]})
                assert expr["label"] == symbol["label"]
                if split:
                    stop("reentry")  # force exit/enter with the arithmetic tuple
                stop("after_reload")
                regs = call("get_registers", {"names": ["rax", "rbx"]})["registers"]
                assert int(regs["rax"], 16) & 0xffff == 7
                assert int(regs["rbx"], 16) & 0xffff == 0x33
                before = call("get_recent_path_constraints", {"limit": 256})
                stop("done")
                after = call("get_recent_path_constraints", {"limit": 256})
                branches = after["constraints"]
                # With a stop between CMP and Jcc, translation currently uses
                # the concrete flag-computation helper. Tuple preservation is
                # asserted separately by the production-object boundary test.
                required = ("control_branch",) if split else ("symbolic_branch", "control_branch")
                for name in required:
                    matches = [r for r in branches if int(r["pc"], 16) == labels[name]]
                    assert matches, (name, branches)
                    for r in matches:
                        assert int(r["label"], 16) != 0
                        assert r["op"] == "ICmp", r
                        assert r["taken"] is True, r
                        assert isinstance(r["exportable"], bool), r
                assert not any(int(r["pc"], 16) == labels["concrete_branch"]
                               for r in branches), branches
                assert not before["truncated"] and not after["truncated"]
                records.append({"split_cmp_jcc": split,
                                "first_branch_recorded": any(
                                    int(r["pc"], 16) == labels["symbolic_branch"]
                                    for r in branches)})
                (work / "records.json").write_text(json.dumps(records, indent=2) + "\n")
        finally:
            terminate_process(proc)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=pathlib.Path, default=DEFAULT_BUILD_DIR)
    args = parser.parse_args()
    root = PROJECT_ROOT / "build" / "lazy-flags"
    root.mkdir(parents=True, exist_ok=True)
    for split in (False, True):
        work = pathlib.Path(tempfile.mkdtemp(prefix="split-" if split else "continuous-", dir=root))
        run(args.build_dir / "x86_64-softmmu" / "symfit-system-x86_64", work, split)
        print(f"PASS {'split' if split else 'continuous'} flags fixture: {work}")


if __name__ == "__main__":
    main()
