#!/usr/bin/env python3
"""Disable launchd daemons in a mounted, writable iOS root.

  disable-launchd-jobs.py ROOT PROGRAM [PROGRAM ...]

Edits /System/Library/xpc/launchd.plist (the cache launchd reads when booted
with launchd_unsecure_cache=1): every job whose program basename is one of the
PROGRAMs gets Disabled=true. Each PROGRAM must match at least one job.

The kernel's sweetnoexec boot-arg refuses bluetoothd and fairplayd.H2 at exec,
but launchd keeps respawning them: each was killed 57 times in a 10-minute boot,
and every kill also ran ReportCrash (observed). A disabled job is never spawned.
sweetnoexec stays as the safety net.
"""
import os
from pathlib import Path
import plistlib
import sys


def program(job):
    args = job.get('ProgramArguments') or [None]
    return job.get('Program') or args[0] or ''


def disable_jobs(data, names):
    """Disable matching programs in a decoded cache; caller owns the write."""
    found = {name: [] for name in names}
    for job in data['LaunchDaemons'].values():
        name = os.path.basename(program(job))
        if name in found:
            found[name].append(job.get('Label'))
    missing = [name for name, labels in found.items() if not labels]
    if missing:
        raise ValueError(f'no job runs: {", ".join(missing)}')
    for job in data['LaunchDaemons'].values():
        if os.path.basename(program(job)) in found:
            job['Disabled'] = True
    return found


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    root, names = Path(sys.argv[1]), sys.argv[2:]
    cache = root/'System/Library/xpc/launchd.plist'
    data = plistlib.loads(cache.read_bytes())
    try:
        found = disable_jobs(data, names)
    except ValueError as error:
        sys.exit(str(error))
    # Keep the cache's owner and mode by replacing its contents in place.
    cache.write_bytes(plistlib.dumps(data, fmt=plistlib.FMT_BINARY))
    for name, labels in found.items():
        print(f'{name}: disabled {", ".join(labels)}')


if __name__ == '__main__':
    main()
