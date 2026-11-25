#!/usr/bin/env python3
"""realinput.py W H STEPS OUTDIR [overheat args...]
Drives Overheat with real input (Hyprland pointer warp + ydotool). STEPS lines:
  click X Y          window-local left click
  key COMBO          e.g. ctrl+c, ctrl+shift+v, down, return, escape
  type TEXT          types text via ydotool
  wait MS
After every step the app is probed (state printed); 'shot NAME' also screenshots."""
import json, os, subprocess, sys, time

W, H, steps_path, outdir = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3], sys.argv[4]
extra = sys.argv[5:]
os.makedirs(outdir, exist_ok=True)
root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
binary = os.environ.get("OVERHEAT_BIN", f"{root}/build/overheat")

KEYS = {"ctrl": 29, "shift": 42, "alt": 56, "super": 125, "return": 28, "enter": 28,
        "escape": 1, "tab": 15, "backspace": 14, "delete": 111, "up": 103, "down": 108,
        "left": 105, "right": 106, "home": 102, "end": 107, "space": 57, "pageup": 104,
        "pagedown": 109, "f": 33, "grave": 41}
for i, ch in enumerate("qwertyuiop"): KEYS[ch] = 16 + i
for i, ch in enumerate("asdfghjkl"): KEYS[ch] = 30 + i
for i, ch in enumerate("zxcvbnm"): KEYS[ch] = 44 + i

def hypr(*a):
    return subprocess.run(["hyprctl", *a], capture_output=True, text=True).stdout

def window(pid):
    return next((c for c in json.loads(hypr("clients", "-j")) if c.get("pid") == pid), None)

steps = [l.strip() for l in open(steps_path) if l.strip() and not l.startswith("#")]
steps.append("wait 50")  # lets a trailing "shot" fire
script = ["wait 900"]
for i, s in enumerate(steps):
    script += [f"probe shot:step{i}", "wait 200", f"probe after{i}"]
script += ["quit"]
spath = os.path.join(outdir, "script.txt")
open(spath, "w").write("\n".join(script) + "\n")

proc = subprocess.Popen([binary, "--script", spath, *extra], cwd=root, stdin=subprocess.PIPE,
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
for _ in range(100):
    win = window(proc.pid)
    if win: break
    time.sleep(0.05)
addr = "address:" + win["address"]
hypr("dispatch", f"hl.dsp.window.float({{ action = 'enable', window = '{addr}' }})")
hypr("dispatch", f"hl.dsp.window.resize({{ x = {W}, y = {H}, window = '{addr}' }})")
hypr("dispatch", f"hl.dsp.window.move({{ x = 2580, y = 60, window = '{addr}' }})")

def run_step(step, c):
    cmd, _, arg = step.partition(" ")
    if cmd == "click":
        x, y = map(int, arg.split())
        hypr("dispatch", f"hl.dsp.cursor.move({{ x = {c['at'][0] + x}, y = {c['at'][1] + y} }})")
        subprocess.run(["ydotool", "mousemove", "-x", "1", "-y", "0"], capture_output=True)
        subprocess.run(["ydotool", "mousemove", "-x", "-1", "-y", "0"], capture_output=True)
        time.sleep(0.05)
        subprocess.run(["ydotool", "click", "0xC0"], capture_output=True)
    elif cmd == "key":
        codes = [KEYS[k] for k in arg.lower().split("+")]
        seq = [f"{k}:1" for k in codes] + [f"{k}:0" for k in reversed(codes)]
        subprocess.run(["ydotool", "key", *seq], capture_output=True)
    elif cmd == "type":
        subprocess.run(["ydotool", "type", "--key-delay", "4", arg], capture_output=True)
    elif cmd == "wait":
        time.sleep(int(arg) / 1000)
    elif cmd == "shot":
        pass
    time.sleep(0.08)

log = open(os.path.join(outdir, "log.txt"), "w")
for line in proc.stdout:
    log.write(line); log.flush()
    if line.startswith("PROBE after"):
        i = int(line.split("after", 1)[1].split(":", 1)[0])
        print(f"[{i}] {steps[i]}")
    elif line.startswith("  file=") or line.startswith("  selection="):
        print("     " + line.strip())
    if line.startswith("PROBE shot:"):
        i = int(line.split("shot:step", 1)[1].split(":", 1)[0])
        c = window(proc.pid)
        if i > 0 and steps[i - 1].startswith("shot "):
            time.sleep(0.12)
            x, y = c["at"]; w, h = c["size"]
            subprocess.run(["grim", "-g", f"{x},{y} {w}x{h}",
                            os.path.join(outdir, steps[i - 1].split()[1] + ".png")])
        run_step(steps[i], c)
        proc.stdin.write("\n"); proc.stdin.flush()
proc.wait(timeout=15)
print("exit", proc.returncode)
