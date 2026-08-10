#!/usr/bin/env python3
"""Send timed keystrokes to a running QEMU over QMP.

    tools/qmp-sendkeys.py <qmp-unix-socket> "<secs>:<keys>[,<secs>:<keys>...]"

`keys` is a QEMU send-key spec: qcode names joined by '-', e.g. `ctrl_r-alt_r-right`.
`secs` is seconds from when this script starts, not from the previous key.

Why this exists (#363): hype's operator controls are a PS/2 keyboard chord
(Right-Ctrl+Right-Alt+key). The claim that those controls survive a wedged guest cannot be
validated by reading a log -- something has to actually press the keys. QEMU runs with
`-display none` on this rig, so QMP send-key is the only way in, and it injects at the i8042,
which is exactly the device hype's host-input path owns.

Prints one line per key sent so the run log can be correlated against hype's own VIEWSWITCH and
KBDIRQ lines.
"""
import json
import socket
import sys
import time


def qmp_connect(path, deadline_s=30.0):
    """Wait for QEMU to create the socket, then complete the capabilities handshake."""
    start = time.monotonic()
    while True:
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(path)
            break
        except OSError:
            if time.monotonic() - start > deadline_s:
                raise
            time.sleep(0.2)
    f = s.makefile('rw', encoding='utf-8', newline='\n')
    f.readline()  # greeting
    f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n")
    f.flush()
    f.readline()
    return s, f


def send_key(f, keys, hold_ms):
    cmd = {
        "execute": "send-key",
        "arguments": {
            "keys": [{"type": "qcode", "data": k} for k in keys.split('-')],
            # A chord needs the modifiers held while the action key is pressed. QEMU presses every
            # key in the list, holds, then releases all of them -- which is precisely the
            # make/make/make + break sequence hype's chord decoder expects.
            "hold-time": hold_ms,
        },
    }
    f.write(json.dumps(cmd) + "\n")
    f.flush()
    # Read until the reply to this command, skipping any asynchronous events.
    while True:
        line = f.readline()
        if not line:
            return "disconnected"
        msg = json.loads(line)
        if "return" in msg or "error" in msg:
            return "error" if "error" in msg else "ok"


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    sock_path, spec = sys.argv[1], sys.argv[2]
    hold_ms = int(sys.argv[3]) if len(sys.argv) > 3 else 200

    steps = []
    for item in spec.split(','):
        item = item.strip()
        if not item:
            continue
        at, keys = item.split(':', 1)
        steps.append((float(at), keys))
    steps.sort()

    s, f = qmp_connect(sock_path)
    start = time.monotonic()
    for at, keys in steps:
        wait = at - (time.monotonic() - start)
        if wait > 0:
            time.sleep(wait)
        status = send_key(f, keys, hold_ms)
        print("sendkey t=%.1fs %s -> %s" % (time.monotonic() - start, keys, status), flush=True)
        if status == "disconnected":
            break
    f.close()
    s.close()


if __name__ == "__main__":
    main()
