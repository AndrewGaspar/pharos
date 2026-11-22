#!/usr/bin/env python3
"""Test auto stereo with Proton, a private X display, and an isolated Hyprland IPC socket."""
import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import tempfile
import time

HERE = Path(__file__).resolve().parent
SRC = HERE.parents[2] / "src"


@contextmanager
def private_display(work):
    auth = work / "Xauthority"
    auth.touch(mode=0o600)
    for number in range(90, 190):
        display = f":{number}"
        if Path(f"/tmp/.X{number}-lock").exists() or Path(f"/tmp/.X11-unix/X{number}").exists():
            continue
        subprocess.run(["xauth", "-f", auth, "add", display, ".", secrets.token_hex(16)], check=True)
        cmd = ["Xvfb", display, "-displayfd", "1", "-auth", auth, "-nolisten", "tcp",
               "-screen", "0", "1024x768x24"]
        with subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True) as server:
            try:
                if server.stdout.readline().strip() == str(number):
                    yield dict(os.environ, DISPLAY=display, XAUTHORITY=str(auth))
                    return
                if not Path(f"/tmp/.X{number}-lock").exists():
                    raise RuntimeError(f"Xvfb failed to start on {display}")
            finally:
                if server.poll() is None:
                    server.terminate()
    raise RuntimeError("no free private X display")


def window_title(xid_file, env):
    xid = int(xid_file.read_text())
    prop = subprocess.check_output(["xprop", "-id", hex(xid), "_NET_WM_NAME"], env=env, text=True)
    return prop.split('"', 1)[-1].rstrip().rstrip('"')


def clients(work, stage, env, splash_tagged):
    host_title = "Origins window" if stage == "duplicate_title" else "Quiver"
    result = [dict(address="0xcafe", mapped=True, title=host_title, pid=8778, monitor=0)]
    if stage in ("headless", "removed", "duplicate_title"):
        return result
    game = dict(address="0x123", mapped=stage != "unmapped", title=window_title(work / "xid", env), pid=1144,
                monitor=1 if stage == "moved" else 2)
    if stage == "other_pid":
        game.update(title="Origins window", pid=9988)  # address reused by another process
    result.append(game)
    if stage.startswith("splash_") and stage != "splash_unmapped":
        result.append(dict(address="0x456", mapped=True, pid=1144,
                           title=window_title(work / "splash_xid", env),
                           monitor=1 if stage == "splash_mono" else 2,
                           tags=["stereo-hsbs"] if stage == "splash_existing_tag" or splash_tagged else [],
                           stereo="hsbs" if (splash_tagged and stage == "splash_stereo") or stage == "splash_existing_rule" else "off"))
    return result


def check(work, proton, display_env):
    window = work / "window.exe"
    subprocess.run(["x86_64-w64-mingw32-g++", "-std=c++20", "-O2", "-static", "-DWIN32_LEAN_AND_MEAN",
                    "-DUNICODE", "-D_UNICODE", "-I", SRC, HERE / "window.cpp", SRC / "core/hyprland.cpp",
                    SRC / "core/log.cpp", "-lws2_32", "-o", window], check=True)
    ipc = work / "hypr/live"
    ipc.mkdir(parents=True)
    (ipc / "hyprland.lock").write_text("12345\nwayland-test\n")
    env = dict(display_env, WINEPREFIX=str(work / "prefix"), WINEDLLOVERRIDES="winemenubuilder.exe=d",
               WINEDEBUG="-all", XDG_RUNTIME_DIR=str(work), HYPRLAND_INSTANCE_SIGNATURE="stale",
               WAYLAND_DISPLAY="wayland-test")
    files = proton.resolve() / "files"
    wine = files / "bin/wine"
    wineserver = files / "bin/wineserver"
    env.update(WINESERVER=str(wineserver), LD_LIBRARY_PATH=str(files / "lib/x86_64-linux-gnu"))
    monitors = [dict(id=0, description="Host XReal", focused=True, stereo="sbs"),
                dict(id=1, description="LCD", focused=False, stereo="mono"),
                dict(id=2, description="Game XReal", focused=False, stereo="sbs")]
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
        server.bind(str(ipc / ".socket.sock"))
        server.listen()
        server.settimeout(0.25)
        process = subprocess.Popen([wine, window], cwd=work, env=env)
        try:
            splash_tagged = False
            deadline = time.monotonic() + 120
            while process.poll() is None:
                if time.monotonic() >= deadline:
                    raise TimeoutError("Wine window fixture timed out")
                try:
                    client, _ = server.accept()
                except TimeoutError:
                    continue
                with client:
                    request = client.recv(4096).decode()
                    stage = (work / "stage").read_text()
                    if request == "j/clients":
                        reply = clients(work, stage, display_env, splash_tagged)
                    elif request == "j/monitors":
                        if stage in ("headless", "other_pid", "duplicate_title", "unmapped", "removed"):
                            raise AssertionError(f"queried desktop monitors without a mapped game client in {stage}")
                        reply = monitors
                    elif stage in ("splash_stereo", "splash_no_rule") and request in (
                            'dispatch hl.dsp.window.tag({ tag = "+stereo-hsbs", window = "address:0x456" })',
                            'dispatch hl.dsp.window.tag({ tag = "-stereo-hsbs", window = "address:0x456" })'):
                        splash_tagged = "+stereo-hsbs" in request
                        client.sendall(b"ok")
                        continue
                    else:
                        raise ValueError(f"unexpected compositor request {request!r}")
                    client.sendall(json.dumps(reply).encode())
            if process.returncode:
                raise SystemExit(process.returncode)
            if splash_tagged:
                raise AssertionError("splash retained its tag after cleanup")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            # This server belongs to the disposable prefix under work.
            subprocess.run([wineserver, "-k", "-w"], env=env, check=False, timeout=30)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--proton", required=True, type=Path, help="Proton installation with Wine AF_UNIX support")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="pharos-hypr-") as tmp:
        work = Path(tmp)
        with private_display(work) as display_env:
            check(work, args.proton, display_env)
