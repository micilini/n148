"""Observe external compute load without changing or stopping other jobs."""
from __future__ import annotations

import os
from pathlib import Path
import time


class HostMonitor:
    """Track CPU ticks; require quiet observation before accepting timing work.

    This is a noise guard, not exclusive CPU ownership. Short-lived processes
    can escape observations; raw load and frequency snapshots remain necessary.
    """

    def __init__(self):
        self.owner = os.getpid()
        self.hz = os.sysconf("SC_CLK_TCK")
        self.previous = None
        self.previous_time = None
        self.busy = []
        self.external_cores = None

    def conflicts(self):
        current, named = {}, []
        for path in Path("/proc").glob("[0-9]*/stat"):
            try:
                raw = path.read_text()
                name = raw[raw.index("(") + 1:raw.rindex(")")]
                fields = raw[raw.rindex(")") + 2:].split()
                pid, parent = int(path.parent.name), int(fields[1])
                if pid == self.owner or parent == self.owner or fields[0] in {"Z", "X"}:
                    continue
                ticks = int(fields[11]) + int(fields[12])
                current[pid] = (int(fields[19]), ticks, name)
            except (OSError, ValueError, IndexError):
                continue
            if name.startswith("qemu-system") or name in {"cc1", "cc1plus", "clang", "clang++", "ninja", "rustc"}:
                named.append({"pid": pid, "command": name, "reason": "external_qemu_or_build"})
        stamp = time.monotonic()
        if self.previous is None:
            self.previous, self.previous_time = current, stamp
        elif stamp - self.previous_time >= .75:
            elapsed = stamp - self.previous_time
            self.busy, self.external_cores = [], 0.
            for pid, (started, ticks, name) in current.items():
                before = self.previous.get(pid)
                if before is None or before[0] != started:
                    continue
                cores = max(0., (ticks - before[1]) / (self.hz * elapsed))
                self.external_cores += cores
                if cores >= .5:
                    self.busy.append({"pid": pid, "command": name,
                                      "reason": "external_cpu", "cpu_cores": cores})
            if self.external_cores >= 1.:
                self.busy.append({"reason": "aggregate_external_cpu", "cpu_cores": self.external_cores})
            self.previous, self.previous_time = current, stamp
        return named + self.busy
