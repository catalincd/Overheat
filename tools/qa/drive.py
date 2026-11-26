#!/usr/bin/env python3
"""drive.py W H SCRIPT OUTDIR [overheat args...]
Requires Hyprland (Lua dispatch API) and grim. {expr} in SCRIPT is evaluated with W, H;
write {{ and }} for literal braces.
Launches Overheat floating at WxH, runs SCRIPT, screenshots the window on every
'PROBE shot:<name>' line, and writes the full log to OUTDIR/log.txt."""
import json, os, subprocess, sys, time

W, H, script, outdir = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3], sys.argv[4]
extra = sys.argv[5:]
os.makedirs(outdir, exist_ok=True)
root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
binary = os.environ.get("OVERHEAT_BIN", f"{root}/build/overheat")

def hypr(*a):
    return subprocess.run(["hyprctl", *a], capture_output=True, text=True).stdout

def find_window(pid):
    for c in json.loads(hypr("clients", "-j")):
        if c.get("pid") == pid:
            return c
    return None

import re
src = open(script).read()
# {{ and }} are literal braces (for typing code). Only {..} expressions using W or H
# are evaluated, so logpoint text like {i} passes through.
src = src.replace("{{", "\x01").replace("}}", "\x02")
src = re.sub(r"\{([^}]*[WH][^}]*)\}", lambda m: str(int(eval(m.group(1), {"W": W, "H": H}))), src)
src = src.replace("\x01", "{").replace("\x02", "}")
script = os.path.join(outdir, "script.txt")
open(script, "w").write(src)
# Default: the app captures its own frames (works with the screen locked or the
# window hidden). QA_CAPTURE=grim screenshots through the compositor instead.
app_capture = os.environ.get("QA_CAPTURE", "app") == "app"
# Isolate tests from the user's real settings/themes unless QA_REAL_CONFIG=1.
env = dict(os.environ)
if os.environ.get("QA_REAL_CONFIG") != "1":
    env["XDG_CONFIG_HOME"] = os.path.join(os.path.abspath(outdir), "config")
    os.makedirs(os.path.join(env["XDG_CONFIG_HOME"], "overheat", "themes"), exist_ok=True)
# Hidden, unfocused QA windows: a Hyprland rule (registered below) sends this app id
# to a silent special workspace so test runs never take the user's keyboard focus.
env["SDL_APP_ID"] = "overheat-qa"
subprocess.run(["hyprctl", "eval", "hl.window_rule({ name = 'overheat-qa', match = { class = '^overheat-qa$' }, "
                "workspace = 'special:ohqa silent', no_initial_focus = true, float = true })"], capture_output=True)
if app_capture:
    extra = ["--shot-dir", os.path.abspath(outdir), *extra]
proc = subprocess.Popen([binary, "--script", script, *extra], cwd=os.environ.get("QA_CWD", root), env=env,
                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1,
                        errors="replace")  # probes echo file text, which may be invalid UTF-8
win = None
for _ in range(100):
    win = find_window(proc.pid)
    if win: break
    time.sleep(0.05)
if not win:
    print("window never appeared"); proc.kill(); sys.exit(1)
addr = "address:" + win["address"]
hypr("dispatch", f"hl.dsp.window.float({{ action = 'enable', window = '{addr}' }})")
hypr("dispatch", f"hl.dsp.window.resize({{ x = {W}, y = {H}, window = '{addr}' }})")
hypr("dispatch", f"hl.dsp.window.move({{ x = 2580, y = 60, window = '{addr}' }})")

log = open(os.path.join(outdir, "log.txt"), "w")
for line in proc.stdout:
    log.write(line); log.flush()
    if line.startswith("PROBE") or line.startswith("  ") or "exceeded" in line or "first paint" in line or "rror" in line:
        sys.stdout.write(line)
    if line.startswith("PROBE shot:") and not app_capture:
        name = line.split("shot:", 1)[1].split(":", 1)[0].strip()
        time.sleep(0.12)  # let the presented frame reach the compositor
        c = find_window(proc.pid)
        if not c:
            print("[shot] window not found; skipping", name)
        if c:
            x, y = c["at"]; w, h = c["size"]
            path = os.path.join(outdir, f"{name}.png")
            subprocess.run(["grim", "-g", f"{x},{y} {w}x{h}", path])
            print(f"[shot] {path} ({w}x{h})")
        proc.stdin.write("\n"); proc.stdin.flush()
proc.wait(timeout=10)
for f in sorted(os.listdir(outdir)):
    if f.endswith(".ppm") or f.endswith(".pam"):  # .pam: translucent frame, RGBA
        src = os.path.join(outdir, f)
        subprocess.run(["magick", src, src[:-4] + ".png"])
        os.remove(src)
        print(f"[shot] {src[:-4]}.png")
print("exit", proc.returncode)
