#!/usr/bin/env python3
"""
Run a command in a libvirt guest through the QEMU guest agent.

    tools/testing/vmexec.py <domain> powershell -NoProfile -Command "Get-Date"
    tools/testing/vmexec.py <domain> cmd.exe /c "dir C:\\"

The agent's own interface is guest-exec to start, then guest-exec-status
polled until exited, with stdout and stderr arriving base64-encoded. This
wraps that into something that behaves like a normal command: output on
stdout and stderr, and the guest's exit code as this process's exit code.

Needs no networking in the guest - it goes over the virtio-serial channel,
so it works before DHCP, and it keeps working if the guest's firewall does
something unhelpful.

Back-to-back invocations can return an earlier run's output, and a stale
value looks exactly like a real measurement. Any comparison built on this must
run each side twice and require the two to agree.
"""

import base64
import json
import subprocess
import sys
import time

POLL_INTERVAL = 0.25
TIMEOUT = 300


def agent(domain, command):
    out = subprocess.run(
        ["virsh", "--connect", "qemu:///system", "qemu-agent-command",
         domain, json.dumps(command)],
        capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit("agent error: " + (out.stderr.strip() or "unknown"))
    return json.loads(out.stdout)["return"]


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__.strip())
    domain, path, args = sys.argv[1], sys.argv[2], sys.argv[3:]

    pid = agent(domain, {"execute": "guest-exec",
                         "arguments": {"path": path, "arg": args,
                                       "capture-output": True}})["pid"]

    deadline = time.time() + TIMEOUT
    while True:
        status = agent(domain, {"execute": "guest-exec-status",
                                "arguments": {"pid": pid}})
        if status.get("exited"):
            break
        if time.time() > deadline:
            sys.exit("timed out after %ds waiting for pid %d" % (TIMEOUT, pid))
        time.sleep(POLL_INTERVAL)

    # Windows console output is UTF-16 for some programs and the OEM codepage
    # for others; errors="replace" keeps a mangled line from killing the run.
    for key, stream in (("out-data", sys.stdout), ("err-data", sys.stderr)):
        if key in status:
            stream.write(base64.b64decode(status[key]).decode("utf8", "replace"))
    sys.exit(status.get("exitcode", 0))


if __name__ == "__main__":
    main()
