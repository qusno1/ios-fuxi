#!/usr/bin/env python3
"""Offline tests of the actual C daemon and reversible launchd-cache staging."""
import importlib.util
from pathlib import Path
import plistlib
import struct
import subprocess
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('stage_tp', ROOT / 'tools/stage-fuxi-thermald.py')
stage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage)

C_TEST = r'''
#define main thermald_main
#include "thermald.c"
#undef main
#include <assert.h>

struct sample { int error, level, short_read; };
static struct sample samples[32];
static unsigned count, index_, waits, registrations, sets, posts, cancels;
static unsigned fail_set, fail_post, fail_register;
static int sent[32], posted[32], state;
int sysctlbyname(const char *name, void *out, size_t *len, void *in, size_t inlen)
{
    assert(!strcmp(name,"kern.fuxipressure") && !in && !inlen && *len==sizeof(int));
    assert(index_<count);
    struct sample s=samples[index_++];
    if(s.error) { errno=s.error; return -1; }
    *(int *)out=s.level;
    if(s.short_read) *len=1;
    return 0;
}
uint32_t notify_register_check(const char *name, int *token)
{
    assert(!strcmp(name,PRESSURE_NAME));registrations++;
    if(fail_register)return 9;
    *token=42;return NOTIFY_STATUS_OK;
}
uint32_t notify_set_state(int token, uint64_t level)
{
    assert(token==42 && level<=4);
    sent[sets++]=(int)level;
    if(sets==fail_set)return 9;
    state=(int)level;return NOTIFY_STATUS_OK;
}
uint32_t notify_post(const char *name)
{
    assert(!strcmp(name,PRESSURE_NAME));posted[posts++]=state;
    if(posts==fail_post)return 9;
    return NOTIFY_STATUS_OK;
}
uint32_t notify_cancel(int token) {assert(token==42);cancels++;return 0;}
unsigned sleep(unsigned seconds) {assert(seconds==1);waits++;return 0;}
static void reset(void)
{
    memset(samples,0,sizeof(samples));
    count=index_=waits=registrations=sets=posts=cancels=0;
    fail_set=fail_post=fail_register=0;state=-1;
}
static void add(int error,int level,int short_read)
{ samples[count++]=(struct sample){error,level,short_read}; }
int main(void)
{
    reset();add(ENOTSUP,0,0);assert(!serve());
    assert(!registrations && !sets && !posts && !waits && !cancels);
    reset();add(EBUSY,0,0);add(0,0,0);add(0,0,0);add(0,1,0);
    add(EIO,0,0);add(0,1,0);add(0,2,0);add(0,3,0);add(0,4,0);add(ENOENT,0,0);
    assert(!serve());
    assert(registrations==1 && sets==5 && posts==5 && waits==9 && cancels==1);
    for(int i=0;i<5;i++)assert(sent[i]==i && posted[i]==i);
    /* A failed set never posts; a failed post retries the same level. */
    reset();fail_set=1;fail_post=2;
    add(0,0,0);add(0,0,0);add(0,1,0);add(0,1,0);add(0,1,0);add(ENOTSUP,0,0);
    assert(!serve() && sets==4 && posts==3 && cancels==1);
    assert(sent[0]==0 && sent[1]==0 && sent[2]==1 && sent[3]==1);
    assert(posted[0]==0 && posted[1]==1 && posted[2]==1);
    /* Malformed/unknown levels cannot overwrite notification state. */
    reset();add(0,-1,0);add(0,5,0);add(0,0,1);add(0,2,0);add(ENOTSUP,0,0);
    assert(!serve() && registrations==1 && sets==1 && posts==1 && sent[0]==2);
    reset();fail_register=1;add(0,0,0);
    assert(serve()==1 && !sets && !posts && !cancels);
    char *show[]={"thermald","show"}, *level[]={"thermald","level"};
    char *bad[]={"thermald","bad"};
    reset();add(0,3,0);assert(!thermald_main(2,show) && !registrations);
    reset();add(0,2,0);assert(!thermald_main(2,level) && !registrations);
    reset();add(EBUSY,0,0);assert(thermald_main(2,show)==1 && !registrations);
    assert(thermald_main(2,bad)==2);
    puts("PASS: daemon wire values, startup/read errors, change-only publish, failed notify retries, range/size validation, show/level and disabled exit");
}
'''

with tempfile.TemporaryDirectory() as tmp:
    d = Path(tmp)
    (d / 'sys').mkdir()
    (d / 'sys/sysctl.h').write_text('#include <stddef.h>\nint sysctlbyname(const char *,void *,size_t *,void *,size_t);\n')
    (d / 'notify.h').write_text('''#include <stdint.h>
#define NOTIFY_STATUS_OK 0
uint32_t notify_register_check(const char *,int *);
uint32_t notify_set_state(int,uint64_t);
uint32_t notify_post(const char *);
uint32_t notify_cancel(int);
''')
    (d / 'test.c').write_text(C_TEST)
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I', str(d),
                    '-I', str(ROOT / 'tools/fuxi-thermald'), str(d / 'test.c'),
                    '-o', str(d / 'test')], check=True)
    subprocess.run([str(d / 'test')], check=True)

    root = d / 'root'
    cache = root / stage.CACHE
    cache.parent.mkdir(parents=True)
    thermal = '/System/Library/LaunchDaemons/com.apple.thermalmonitord.plist'
    original_job = {'Label': 'com.apple.thermalmonitord',
                    'ProgramArguments': ['/usr/libexec/thermalmonitord'],
                    'KeepAlive': {'SuccessfulExit': False},
                    'MachServices': {'com.apple.thermalmonitord.watchdog': True}}
    original = {'LaunchDaemons': {thermal: original_job,
                                 '/other': {'Label': 'other', 'Disabled': False}},
                'VersionNumber': 7, 'OtherMetadata': {'preserve': True}}

    def init(disabled=None, present=True):
        data = plistlib.loads(plistlib.dumps(original))
        if not present:
            del data['LaunchDaemons'][thermal]
        elif disabled is not None:
            data['LaunchDaemons'][thermal]['Disabled'] = disabled
        cache.write_bytes(plistlib.dumps(data, fmt=plistlib.FMT_BINARY))
        cache.chmod(0o640)
        return data

    def read():
        return plistlib.loads(cache.read_bytes())

    init()
    unchanged = read()
    try:
        stage.disable.disable_jobs(unchanged, ['thermalmonitord', 'missing'])
        raise AssertionError('missing program accepted')
    except ValueError:
        pass
    assert unchanged == read()  # Validate all matches before changing any job.

    # Authored command-layout model only; no Apple or executable fixture.
    binary = d / 'thermald'
    binary.write_bytes(struct.pack('<8I', 0xfeedfacf, 0x100000c, 0, 2, 2, 40, 0, 0) +
                       struct.pack('<6I', 0x32, 24, 2, 0x100000, 0x100000, 0) +
                       struct.pack('<4I', 0x1d, 16, 72, 1) + b'S')
    for disabled, present in ((None, True), (False, True), (True, True), (None, False)):
        before = init(disabled, present)
        raw = cache.read_bytes()
        stage.stage(root)
        assert cache.read_bytes() == raw and not (root / stage.BINARY).exists()
        with patch.object(stage.os, 'geteuid', return_value=0), patch.object(stage.os, 'chown') as chown:
            stage.stage(root, binary, enable=True)
            assert chown.call_count == 3
        data = read()
        job = data['LaunchDaemons']['/' + stage.JOB]
        assert job['Label'] == stage.LABEL and job['RunAtLoad'] and not job['KeepAlive']
        assert job['ProgramArguments'] == ['/' + stage.BINARY]
        assert plistlib.loads((root / stage.JOB).read_bytes()) == job
        assert (root / stage.BINARY).read_bytes() == binary.read_bytes()
        for name, mode in ((stage.BINARY, 0o755), (stage.JOB, 0o644), (stage.NOTICE, 0o644)):
            assert (root / name).stat().st_mode & 0o777 == mode
        assert cache.stat().st_mode & 0o777 == 0o640
        assert data['OtherMetadata'] == before['OtherMetadata']
        if present:
            apple = data['LaunchDaemons'][thermal]
            assert apple['Disabled'] is True
            assert (stage.SAVED in apple) == (disabled is not True)
        stage.stage(root, binary, enable=True)
        assert read() == data  # Do not overwrite original Disabled state.
        # The existing on-demand tool must leave our job and disabled Apple job alone.
        ondemand = stage.module('ondemand_tp', 'launchd-on-demand.py')
        strip, _, _ = ondemand.plan(data, set(ondemand.KEEP))
        assert stage.LABEL not in [row[0] for row in strip]
        assert 'com.apple.thermalmonitord' not in [row[0] for row in strip]
        stage.stage(root, remove=True)
        assert read() == before
        assert all(not (root / p).exists() for p in (stage.BINARY, stage.JOB, stage.NOTICE))
        stage.stage(root, remove=True)
        assert read() == before

    init()
    for action in ('missing', 'invalid', 'symlink', 'duplicate'):
        if action == 'duplicate':
            data = read()
            data['LaunchDaemons']['/duplicate'] = {'Label': stage.LABEL}
            cache.write_bytes(plistlib.dumps(data))
        target = root / stage.BINARY
        if action == 'symlink':
            target.parent.mkdir(parents=True, exist_ok=True)
            target.symlink_to(d / 'escape')
        bad = d / 'bad'
        bad.write_bytes(b'invalid')
        raw = cache.read_bytes()
        try:
            stage.stage(root, None if action == 'missing' else bad if action == 'invalid' else binary,
                        enable=True)
            raise AssertionError(action)
        except ValueError:
            pass
        assert cache.read_bytes() == raw
        if target.is_symlink():
            target.unlink()

print('PASS: explicit opt-in, executable validation, cache/file installation, ownership/modes, idempotence, thermalmonitord conflict/restore/absent/already-disabled and destination containment')
