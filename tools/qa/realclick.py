#!/usr/bin/env python3
"""realclick.py W H OUTDIR [overheat args...]
Real-input hit test: moves the pointer with Hyprland and clicks with ydotool, then
compares the requested window-local point with what the app received and where
the cursor landed. Requires ydotoold running and /dev/uinput access."""
import json, os, subprocess, sys, time

W, H, outdir = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
extra = sys.argv[4:]
os.makedirs(outdir, exist_ok=True)

# Never inject real input into a locked session (it would land in the lock screen).
if subprocess.run(["pgrep", "-x", "hyprlock"], capture_output=True).returncode == 0:
    sys.exit("refusing to run: session is locked (hyprlock running)")
root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
binary = os.environ.get("OVERHEAT_BIN", f"{root}/build/overheat")
WX, WY = 2580, 60

def hypr(*a):
    return subprocess.run(["hyprctl", *a], capture_output=True, text=True).stdout

# Window-local click targets: editor text, gutter edge, tab bar, file tree, terminal, far corner.
targets = [(385, 142), (300, 47), (W - 40, H - 120), (60, 92), (W // 2, H - 60), (W - 5, 40)]
script = ["wait 900"]
for i in range(len(targets)):
    script += [f"probe shot:step{i}", "wait 250", f"probe after{i}"]
script += ["probe shot:final", "quit"]
spath = os.path.join(outdir, "script.txt")
open(spath, "w").write("\n".join(script) + "\n")

proc = subprocess.Popen([binary, "--script", spath, "--debug-hit", *extra], cwd=root,
                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        text=True, bufsize=1)
for _ in range(100):
    win = next((c for c in json.loads(hypr("clients", "-j")) if c.get("pid") == proc.pid), None)
    if win: break
    time.sleep(0.05)
addr = "address:" + win["address"]
hypr("dispatch", f"hl.dsp.window.float({{ action = 'enable', window = '{addr}' }})")
hypr("dispatch", f"hl.dsp.window.resize({{ x = {W}, y = {H}, window = '{addr}' }})")
hypr("dispatch", f"hl.dsp.window.move({{ x = {WX}, y = {WY}, window = '{addr}' }})")

log = open(os.path.join(outdir, "log.txt"), "w")
current = None
for line in proc.stdout:
    log.write(line); log.flush()
    if line.startswith("PROBE after"):
        current = int(line.split("after", 1)[1].split(":", 1)[0])
    elif current is not None and "last_click=" in line:
        got = line.split("last_click=", 1)[1].strip()
        print(f"step{current}: requested {targets[current]} received {got}")
    elif current is not None and "cursor=" in line:
        print(f"         {line.strip()}")
        current = None
    if line.startswith("PROBE shot:"):
        name = line.split("shot:", 1)[1].split(":", 1)[0].strip()
        time.sleep(0.12)
        c = next((c for c in json.loads(hypr("clients", "-j")) if c.get("pid") == proc.pid), None)
        if c:
            x, y = c["at"]; w, h = c["size"]
            subprocess.run(["grim", "-g", f"{x},{y} {w}x{h}", os.path.join(outdir, f"{name}.png")])
        if name.startswith("step"):
            tx, ty = targets[int(name[4:])]
            hypr("dispatch", f"hl.dsp.cursor.move({{ x = {c['at'][0] + tx}, y = {c['at'][1] + ty} }})")
            if os.environ.get("NUDGE", "1") == "1":
                # A warp alone may not reach the client as motion; a real 1px move does.
                subprocess.run(["ydotool", "mousemove", "-x", "1", "-y", "0"], capture_output=True)
                subprocess.run(["ydotool", "mousemove", "-x", "-1", "-y", "0"], capture_output=True)
            time.sleep(0.05)
            subprocess.run(["ydotool", "click", "0xC0"], capture_output=True)
            time.sleep(0.05)
        proc.stdin.write("\n"); proc.stdin.flush()
proc.wait(timeout=10)
print("exit", proc.returncode)
