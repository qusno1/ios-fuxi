#!/usr/bin/env python3
"""Stage the thermal bridge into a mounted writable root, explicitly with --enable.

Without --enable/--remove, only report thermalmonitord's cache state. No phone
access or mounts. Requires launchd_unsecure_cache=1 at boot. --remove restores
only thermalmonitord Disabled keys changed by this tool.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import plistlib
import shutil

HERE = Path(__file__).resolve().parent
LABEL = 'com.fuxi.thermald'
JOB = f'System/Library/LaunchDaemons/{LABEL}.plist'
BINARY = 'usr/local/bin/thermald'
NOTICE = 'usr/local/share/fuxi-thermald/LICENSE.HackiOS'
CACHE = 'System/Library/xpc/launchd.plist'
SAVED = '_FuxiThermaldDisabled'


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, HERE / filename)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


disable = module('disable_jobs', 'disable-launchd-jobs.py')
metal = module('metal_stage', 'stage-fuxi-metal.py')


def safe(root, relative):
    path = root / relative
    if not path.resolve().is_relative_to(root.resolve()) or path.is_symlink():
        raise ValueError(f'unsafe staging destination: {relative}')
    return path


def stage(root, binary=None, enable=False, remove=False):
    if enable and remove:
        raise ValueError('--enable and --remove are mutually exclusive')
    cache = safe(root, CACHE)
    data = plistlib.loads(cache.read_bytes())
    jobs = data['LaunchDaemons']
    competitors = [j for j in jobs.values()
                   if Path(disable.program(j)).name == 'thermalmonitord']
    for job in competitors:
        print('thermalmonitord:', job.get('Label'),
              'disabled' if job.get('Disabled') else 'enabled (potential pressure publisher)')
    if not competitors:
        print('thermalmonitord: absent from cache')
    if not (enable or remove):
        return
    if any(j.get('Label') == LABEL and key != '/' + JOB for key, j in jobs.items()):
        raise ValueError('duplicate thermald label at an unexpected cache key')
    paths = {name: safe(root, name) for name in (BINARY, JOB, NOTICE)}
    if enable:
        if binary is None:
            raise ValueError('--enable requires --binary signed-arm64-executable')
        metal.macho(binary, 2)
        job_data = plistlib.loads((HERE / 'fuxi-thermald' / f'{LABEL}.plist').read_bytes())
        # The stock service can start on Mach lookup even after the cooling
        # on-demand pass. Save its Disabled key exactly, and use the existing
        # disable tool's implementation to prevent two pressure publishers.
        for job in competitors:
            if not job.get('Disabled') and SAVED not in job:
                job[SAVED] = {k: job[k] for k in ('Disabled',) if k in job}
        if competitors:
            disable.disable_jobs(data, ['thermalmonitord'])
        sources = {BINARY: binary,
                   NOTICE: HERE / 'fuxi-thermald/LICENSE.HackiOS'}
        for name, source in sources.items():
            paths[name].parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, paths[name])
        paths[JOB].parent.mkdir(parents=True, exist_ok=True)
        paths[JOB].write_bytes(plistlib.dumps(job_data))
        for name, path in paths.items():
            os.chmod(path, 0o755 if name == BINARY else 0o644)
            if os.geteuid() == 0:
                os.chown(path, 0, 0)
        jobs['/' + JOB] = job_data
    else:
        jobs.pop('/' + JOB, None)
        for job in jobs.values():
            if SAVED in job:
                saved = job.pop(SAVED)
                job.pop('Disabled', None)
                job.update(saved)
        for path in paths.values():
            path.unlink(missing_ok=True)
    # Preserve cache ownership and mode, like disable-launchd-jobs.py.
    cache.write_bytes(plistlib.dumps(data, fmt=plistlib.FMT_BINARY))
    print('enabled thermal bridge (requires fuxiperf=1 fuxitp=1)' if enable
          else 'removed thermal bridge; restored saved thermalmonitord state')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('root', type=Path)
    ap.add_argument('--binary', type=Path)
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument('--enable', action='store_true')
    mode.add_argument('--remove', action='store_true')
    args = ap.parse_args()
    stage(args.root, args.binary, args.enable, args.remove)


if __name__ == '__main__':
    main()
