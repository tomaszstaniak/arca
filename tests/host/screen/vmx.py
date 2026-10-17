"""Extra pointer actions over tools/vmctl.py: modifier-held clicks.
Usage: vmx.py modclick <qcode> X Y   (e.g. shift X Y)"""
import os, sys, time
sys.path.insert(0, os.environ.get("AROS_TOOLS", os.path.expanduser("~/Work/AROS/tools")))
import vmctl

def keyev(vm, code, down):
    vm._cmd("input-send-event", events=[{"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": code}}}])

with vmctl.VM() as vm:
    if sys.argv[1] == "slowdrag":
        x0, y0, x1, y1 = (int(v) for v in sys.argv[2:6])
        vm.move(x0, y0); time.sleep(0.4)
        vm.button(True); time.sleep(0.3)
        for i in range(1, 21):
            vm.move(x0 + (x1 - x0) * i // 20, y0 + (y1 - y0) * i // 20); time.sleep(0.08)
        time.sleep(0.5)
        vm.button(False); time.sleep(0.5)
    if sys.argv[1] == "modclick":
        code, x, y = sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
        vm.move(x, y); time.sleep(0.3)
        keyev(vm, code, True); time.sleep(0.15)
        vm.button(True); time.sleep(0.1); vm.button(False); time.sleep(0.15)
        keyev(vm, code, False); time.sleep(0.3)
