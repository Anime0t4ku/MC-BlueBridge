"""Run hardware-independent checks with a native C compiler (CC or gcc)."""
import os
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
cc = os.environ.get("CC") or shutil.which("gcc")
if not cc and Path("C:/msys64/mingw64/bin/gcc.exe").exists():
    cc = "C:/msys64/mingw64/bin/gcc.exe"
if not cc:
    raise SystemExit("Set CC to a native C compiler, or put gcc on PATH.")
env = dict(os.environ)
env["PATH"] = str(Path(cc).parent) + os.pathsep + env.get("PATH", "")
out = root / "build-host"
out.mkdir(exist_ok=True)
for name, sources in {
    "lightbar": ["tests/test_lightbar.c", "src/lightbar.c"],
    "lightbar-state": ["tests/test_lightbar_state.c", "src/bluebridge_state.c", "src/config_store.c"],
    "ds4-gyro": ["tests/test_ds4_gyro.c", "src/ds4_gyro.c", "src/touch_mouse.c"],
    "touch-mouse": ["tests/test_touch_mouse.c", "src/touch_mouse.c"],
}.items():
    exe = out / (name + (".exe" if os.name == "nt" else ""))
    extra = []
    subprocess.run([cc, "-std=c11", *extra, "-Wall", "-Wextra", "-Werror", "-Itests/stubs", "-Isrc",
                    *sources, "-o", str(exe)], cwd=root, env=env, check=True)
    subprocess.run([str(exe)], cwd=root, env=env, check=True)
