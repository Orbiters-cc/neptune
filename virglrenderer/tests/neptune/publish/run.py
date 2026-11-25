#!/usr/bin/env python3
"""Owned Windows programs on an installed release's engine, with a render server given here: no window, no game, no Steam.
  run.py --server <virgl_render_server> --bin <folder of the programs> --home <a Wine prefix's home, written to> --out <prefix of the
         logs> [--off] [--reader '<arguments of spout_reader>']... [--spout-names] -- NAME [args] [-- NAME2 [args]]
Reads the library's release under its shared lock and lays the release's Direct3D in the prefix as the launcher does. NAME2 starts 1 s
after NAME, in the same Wine session. --off: without NPT_SHARED_PUBLISH. Everything started is ended by its process group."""
import argparse, fcntl, os, pathlib, shlex, shutil, signal, subprocess, sys, tempfile, time

ap = argparse.ArgumentParser()
ap.add_argument("--server", required=True)
ap.add_argument("--bin", required=True)
ap.add_argument("--home", required=True)
ap.add_argument("--out", required=True)
ap.add_argument("--off", action="store_true")
ap.add_argument("--reader", action="append", default=[])
ap.add_argument("--reader-at", type=float, default=0.0, help="seconds after the program started")
ap.add_argument("--timeout", default="300")
ap.add_argument("--spout-names", action="store_true", help="look, while it runs, for Spout's own named memory as files of the host")
ap.add_argument("--kill-worker-at", type=float)
argv = sys.argv[1:]
cut = argv.index("--")
a = ap.parse_args(argv[:cut])
rest = argv[cut + 1:]
first, second = (rest[:rest.index("--")], rest[rest.index("--") + 1:]) if "--" in rest else (rest, [])
library = pathlib.Path.home() / "Library/Application Support/cc.orbiters.obelisk-cli"
runtime = library / "Voile/v26.3.0-27.beta1/linux"
home, programs = pathlib.Path(a.home).resolve(), pathlib.Path(a.bin).resolve()
system32 = home / "prefix/drive_c/windows/system32"
tmp = pathlib.Path(tempfile.mkdtemp(prefix="pub-", dir=os.environ.get("TMPDIR", "/tmp")))  # short: a socket's path has 104 bytes
for d in ("s", "h", "h/shared", "t", "m", "x"):
    (tmp / d).mkdir(mode=0o700)
started = []


def spawn(*args, **kw):
    p = subprocess.Popen(*args, start_new_session=True, **kw)
    started.append(p)
    return p


def end(p):
    for sig in (signal.SIGTERM, signal.SIGKILL):
        try:
            os.killpg(p.pid, sig)
        except ProcessLookupError:
            break
        try:
            p.wait(3)
        except subprocess.TimeoutExpired:
            continue
        time.sleep(0.2)
    p.poll()


def spout_names():
    """Spout's named memory, as Wine's server keeps it on the host: what each file begins with."""
    out = []
    for f in sorted((tmp / "t").glob(".wine-*/server-*/tmpmap-*")):
        try:
            data = f.read_bytes()
        except OSError:
            continue
        if data[:9] == b"VRCSender" or (len(data) == 4096 and b"spout_sender" in data[:300]):
            names = [data[i:i + 256].split(b"\0")[0].decode() for i in range(0, min(len(data), 16384), 256) if data[i]] if len(data) > 4096 else []
            out.append(f"HOSTFILE {f.relative_to(tmp / 't')} {len(data)} bytes mode {oct(f.stat().st_mode & 0o777)} born {f.stat().st_birthtime:.2f}: "
                       + (f"names {names}" if names else f"name {data[:32].split(bytes(1))[0].decode()}" if data[:9] == b"VRCSender" else
                          "info handle 0x%x %dx%d format %d %s" % (*(int.from_bytes(data[i:i + 4], "little") for i in (0, 4, 8, 12)), data[20:276].split(b"\0")[0].decode(errors="replace"))))
    return out


rc = 1
with (library / "Voile.locks/library.lock").open("rb") as lock:
    fcntl.flock(lock, fcntl.LOCK_SH)
    for name in ("d3d11.dll", "dxgi.dll", "d3d12.dll"):
        shutil.copyfile(runtime / "windows" / name, system32 / name)
    shutil.copyfile(runtime / "wine/lib/wine/x86_64-windows/nptunix.dll", system32 / "nptunix.dll")
    dxmt = runtime / "graphics/dxmt-native.framework/Versions/A/dxmt-native"
    env = dict(os.environ, RENDER_SERVER_EXEC_PATH=a.server, NPT_BACKEND="dxmt", **{f"NPT_{k}_LIBRARY_PATH": str(dxmt) for k in ("D3D11", "DXGI", "D3D12")})
    env.pop("NPT_SHARED_PUBLISH", None)
    if not a.off:
        env["NPT_SHARED_PUBLISH"] = "../h/shared"  # relative, as the launcher gives it: the server's folder is its sockets'
    try:
        with open(a.out + ".server.log", "wb") as slog, open(a.out + ".run", "wb") as rlog, open(a.out + ".reader", "wb") as dlog:
            spawn([str(runtime / "graphics/bin/virgl_test_server"), "--no-virgl", "--no-fork", "--socket-path", "vtest.sock"], cwd=tmp / "s", env=env, stdout=slog, stderr=subprocess.STDOUT)
            for _ in range(100):
                if (tmp / "s/vtest.sock").exists():
                    break
                time.sleep(0.05)
            genv = dict(PATH="/usr/bin:/bin", HOME=str(home), TMPDIR=str(tmp / "x"), FEXDARWIN_LKD="1", FEXDARWIN_ROOT=str(runtime / "root"), FEXDARWIN_ROOT_RO="1",
                        LKD_HOST_TMP=str(tmp / "t"), LKD_HOST_SHM=str(tmp / "m"), FEXDARWIN_SUPERVISOR_PID=str(os.getpid()),
                        FEXDARWIN_BIND=f"/opt/wine={runtime}/wine:ro;/home/user={home};/w={programs}:ro;/run/obelisk={tmp}/s")
            args = [str(runtime / "engine/run-with-timeout.sh"), a.timeout, str(runtime / "engine/fexdarwin-run")]
            for kv in ["HOME=/home/user", "USER=user", "LOGNAME=user", "PATH=/opt/wine/bin:/usr/bin:/bin", "LANG=en_US.UTF-8", "WINEPREFIX=/home/user/prefix", "WINEARCH=win64",
                       "WINEDEBUG=-all", "WINEDLLOVERRIDES=mscoree,mshtml=;winemenubuilder.exe=d;d3d11,dxgi,d3d12=n", "VTEST_SOCKET_NAME=/run/obelisk/vtest.sock", "NPT_DEBUG=force_vtest"]:
                args += ["--env", kv]
            line = lambda p: "/opt/wine/bin/wine /w/" + p[0] + ".exe " + " ".join(shlex.quote(x) for x in p[1:])
            script = "/opt/wine/bin/wineserver -p0; " + (f"{line(first)} & sleep 1; {line(second)}; wait" if second else line(first)) + "; s=$?; /opt/wine/bin/wineserver -k; exit $s"
            t0 = time.time()
            p = spawn(args + ["/bin/sh", "-c", script], env=genv, stdout=rlog, stderr=subprocess.STDOUT)
            time.sleep(a.reader_at)
            readers = [spawn([str(programs / "spout_reader"), str(tmp / "h/shared")] + r.split(), stdout=dlog, stderr=subprocess.STDOUT) for r in a.reader]
            seen = []
            while p.poll() is None:
                time.sleep(0.2)
                if a.spout_names and not any("names" in s for s in seen):
                    seen = spout_names()
                if a.kill_worker_at and time.time() - t0 > a.kill_worker_at:
                    a.kill_worker_at = None
                    # the worker is in the render server's own session: ours
                    out = subprocess.run(["/bin/ps", "-axo", "pid=,ppid=,command="], capture_output=True, text=True).stdout
                    for l in out.splitlines():
                        f = l.split(None, 2)
                        if len(f) == 3 and "--worker-context-fd" in f[2] and f[2].startswith(a.server):
                            print("killing worker", f[0])
                            os.kill(int(f[0]), signal.SIGKILL)
            rc = p.returncode
            for r in readers:
                r.wait()
                rc = rc or r.returncode
            left = sorted(x.name for x in (tmp / "h/shared").iterdir())
        print("\n".join(seen) + ("\n" if seen else "") + f"exit {rc}, {time.time() - t0:.0f} s; left in the folder: {left}")
    finally:
        for p in started[::-1]:
            end(p)
        shutil.rmtree(tmp, ignore_errors=True)
for name in (".run", ".reader"):
    for l in open(a.out + name, errors="replace"):
        if l.startswith(("WITNESS", "READER")):
            print(l.rstrip())
for l in open(a.out + ".server.log", errors="replace"):
    if "publish:" in l or "<Error>" in l:
        print("SERVER", l.split("npt: ", 1)[-1].rstrip()[:200])
sys.exit(rc)
