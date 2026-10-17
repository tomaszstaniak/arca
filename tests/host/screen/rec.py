"""Record the guest screen as fast as QMP screendump allows, while running a
list of pointer/key actions on a timeline. Writes frames (PPM) and a timing file.

Usage: rec.py <outdir> <seconds> [action ...]
action: "t:click:x:y" | "t:wheel:x:y:down|up:steps" | "t:drag:x0:y0:x1:y1" |
        "t:modclick:qcode:x:y" | "t:key:name" | "t:type:text"   (t = seconds from start)
"""
import os, sys, time, threading
sys.path.insert(0, os.environ.get("AROS_TOOLS", os.path.expanduser("~/Work/AROS/tools")))
import vmctl

out, secs = sys.argv[1], float(sys.argv[2])
acts = sorted((float(a.split(":")[0]), a.split(":")[1:]) for a in sys.argv[3:])
os.makedirs(out, exist_ok=True)


def keyev(vm, code, down):
    vm._cmd("input-send-event", events=[{"type": "key", "data": {"down": down,
            "key": {"type": "qcode", "data": code}}}])


with vmctl.VM() as vm:
    lock = threading.Lock()
    t0 = time.time()
    stop = False
    times = []

    def actions():
        for t, a in acts:
            while time.time() - t0 < t:
                time.sleep(0.01)
            with lock:
                k = a[0]
                if k == "click":
                    vm.click(int(a[1]), int(a[2]))
                elif k == "wheel":
                    vm.wheel(int(a[1]), int(a[2]), a[3] == "up", int(a[4]))
                elif k == "drag":
                    vm.drag(*(int(v) for v in a[1:5]))
                elif k == "modclick":
                    vm.move(int(a[2]), int(a[3])); time.sleep(0.2)
                    keyev(vm, a[1], True); time.sleep(0.1)
                    vm.button(True); time.sleep(0.08); vm.button(False); time.sleep(0.1)
                    keyev(vm, a[1], False)
                elif k == "key":
                    vm.key(a[1])
                elif k == "type":
                    vm.type(a[1]); vm.key("ret")
            print(f"{time.time()-t0:6.2f} {':'.join(a)}", flush=True)

    th = threading.Thread(target=actions)
    th.start()
    n = 0
    next_t = 0.0
    while time.time() - t0 < secs:
        # At most about 30 frames a second: enough to see a flash, and the
        # raw frames are 2.3 MB each.
        while time.time() - t0 < next_t:
            time.sleep(0.002)
        next_t = time.time() - t0 + 1 / 30
        with lock:
            p = os.path.join(out, f"f{n:05d}.ppm")
            vm._cmd("screendump", filename=os.path.abspath(p))
        times.append(time.time() - t0)
        n += 1
    th.join()
    with open(os.path.join(out, "times.txt"), "w") as f:
        f.write("\n".join(f"{t:.3f}" for t in times) + "\n")
    print(f"{n} frames in {secs} s ({n/secs:.1f}/s)")
