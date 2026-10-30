#!/usr/bin/env python3
"""Apply the SWEET bring-up changes to an iOS root made by tools/mkiosroot.sh.

  sudo sweet-root-tweaks.py [OPTIONS] MOUNTED_ROOT

MOUNTED_ROOT is the HFS+ image mounted read-write (Linux: run
`fsck.hfsplus -fy` on the image, then `mount -t hfsplus -o loop,force`).
Run as root (it writes root/mobile owned files). Idempotent. This is what
turned the plain mkiosroot.sh output into the images that reach the home
screen (image versions v8-v14 in NOTES.md):

  launchd.plist   _PanicOnCrash -> _PanicOnCrasX (a crashing daemon must not
                  panic the system); CommCenter jobs removed (no baseband: it
                  crash-loops and Setup blocks on its CoreTelephony XPC)
  containers      /private/var/containers/{Bundle,Data/System,Shared/SystemGroup,Temp}
                  (on iOS they live on the data volume)
  migration       LastSystemVersion so DataMigration does not run for an hour
                  on every boot
  audio           VirtualAudio HAL plug-in disabled (mediaserverd crash)
  MediaToolbox    StartRecorderServer=false (camera filter prewarm calls a
                  NULL OpenGL function without a GPU -> mediaserverd crash loop)
  SpringBoard     BKNoWatchdogs (one slow CPU path, no scene-create watchdog
                  kills), no cloud-configuration UI
  Setup           skipped: SetupDone, SetupFinishedAllSteps, SetupVersion 11,
                  mini-buddy flags off
  activation      mobileactivationd data ark seeded; the actual Activated state
                  comes from its Hactivation branch, selected by the /chosen
                  security-status properties in tools/mkadt.py

Opt-in steps (off unless named; docs-lanes/wallpaper.md has the evidence):

  --posterboard-oslog   diagnostic: OS_ACTIVITY_DT_MODE=YES in PosterBoard's
                        launchd job, so its os_log output also goes to stderr
                        (= /dev/null, which the sweetnull boot-arg tees into
                        the kernel log as "|PosterBoard| ...")
  --legacy-wallpaper    experiment: feature-flag override SpringBoard/Autobahn
                        off in /Library/Preferences/FeatureFlags, i.e. the
                        iPad-style iOS 16 wallpaper path without PosterBoard
                        posters
  --no-legacy-wallpaper remove that override again
  --legacy-wallpaper-image [PNG]
                        static wallpaper for that legacy path: writes
                        /var/mobile/Library/SpringBoard/{Lock,Home}Background.cpbitmap
                        (raw BGRA CPBitmap, no image decoder needed) from an
                        8-bit RGB/RGBA PNG, or a built-in light 1125x2436
                        gradient. On iPhone the legacy path never installs a
                        default wallpaper and falls back to black. The image
                        must be light (UIKit legibility style 2): a dark one
                        gets a luminance treatment rendered by SBRendererService
                        with Metal, which fails here and leaves the wallpaper
                        undrawn (white). A dark PNG is written with a warning.
                        A light one stayed white on device too (boot
                        ios-0098-metal2e): use --legacy-wallpaper-color
  --no-legacy-wallpaper-image  remove those two files again
  --legacy-wallpaper-color [#RRGGBB]
                        solid colour wallpaper for the legacy path instead of
                        an image (default #2e3a6e): an NSKeyedArchiver UIColor
                        under SBWallpaperColorDataKey in com.apple.springboard
                        SB{Lock,Home}ScreenWallpapers/none, the same colour
                        configuration as the black fallback, so no image layer,
                        treatment or blur render is involved. Also removes the
                        cpbitmaps, their thumbnail and the derived wallpaper
                        image caches. Preferred on this GPU-less system: the
                        static image stayed white on device even when light
  --no-legacy-wallpaper-color  remove that colour again
  --oslog LABEL[,LABEL...]
                        diagnostic: OS_ACTIVITY_DT_MODE=YES for these launchd
                        job labels (e.g. com.apple.chronod,com.apple.lsd,
                        com.apple.pluginkit.pkd); add the process names to the
                        sweetnull boot-arg to see the lines
  --rebuild-launchservices
                        delete the LaunchServices databases (lsd rebuilds them
                        at the next boot) and chronod's cached widget
                        snapshots. The database on the root was built before
                        the kernel answered AMFI request 0x60, so it holds no
                        extension point and no app extension: chronod and
                        PosterBoard find no widget or poster extension
  --install-staged-apps
                        move the removable system apps that iOS installs on
                        its first boot (/private/var/staged_system_apps:
                        Weather, Calendar, Maps, Notes, ...) to /Applications,
                        park their /System/Library/AppPlaceholders stubs and
                        add their extensions to the launchd.plist cache
                        (AppExtensions). The data migration that would install
                        them is skipped on this root. Use with
                        --rebuild-launchservices
  --no-install-staged-apps  undo --install-staged-apps
  --only-optional       apply just the named opt-in steps, not the base set
"""
import argparse, glob, os, plistlib, shutil, struct, sys, zlib

BUILD, VERSION = '20H392', '16.7.16'
MOBILE = 501


def load(path):
    with open(path, 'rb') as f:
        return plistlib.load(f)


def save(path, data, uid, gid, mode, fmt=plistlib.FMT_BINARY):
    with open(path, 'wb') as f:
        plistlib.dump(data, f, fmt=fmt)
    os.chown(path, uid, gid)
    os.chmod(path, mode)


def merge_pref(root, name, values):
    """Merge keys into /var/mobile/Library/Preferences/NAME.plist (mobile, 0600)."""
    path = os.path.join(root, 'private/var/mobile/Library/Preferences', name + '.plist')
    data = load(path) if os.path.exists(path) else {}
    data.update(values)
    save(path, data, MOBILE, MOBILE, 0o600)
    print('pref', name, sorted(values))


AX_PREF = 'com.apple.Accessibility'
AX_REDUCE_TRANSPARENCY = 'EnhancedBackgroundContrastEnabled'


def reduce_transparency(root, enable, owner=(MOBILE, MOBILE)):
    """Settings > Accessibility > Reduce Transparency: solid fills in place of
    the blur materials behind widgets, the dock and folders (fewer GPU passes
    and CPU-encoded draws per compositor frame)."""
    path = os.path.join(root, 'private/var/mobile/Library/Preferences', AX_PREF + '.plist')
    data = load(path) if os.path.exists(path) else {}
    if enable:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        data[AX_REDUCE_TRANSPARENCY] = True
    elif data.pop(AX_REDUCE_TRANSPARENCY, None) is None:
        print('pref', AX_PREF, 'reduce transparency already off')
        return
    save(path, data, owner[0], owner[1], 0o600)
    print('pref', AX_PREF, 'reduce transparency', 'on' if enable else 'off')


MG_CACHE = 'private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist'
# Aixt/MEN2O2B7f+8m4TxUA is the deep colour answer MaterialKit, QuartzCore and
# UIKit read (MTSupportsDeepColor). True makes MaterialKit snapshot idle materials
# (App Library, Today view) into two-plane 10-bit 'b3a8' surfaces that the Fuxi GPU
# path cannot render, so they stay black; the panel scans out 8-bit BGRA anyway.
MG_DEEP_COLOR = 'Aixt/MEN2O2B7f+8m4TxUA'


def mg_cache_extra(root, specs):
    """Override MobileGestalt answers in the cache's CacheExtra (KEY=true|false).
    The cache is written by MobileGestalt itself; an absent cache is left absent."""
    values = {}
    for spec in specs:
        key, sep, value = spec.partition('=')
        if not key or not sep or value not in ('true', 'false'):
            sys.exit('bad --mg-cache-extra %r (KEY=true|false)' % spec)
        values[key] = value == 'true'
    path = os.path.join(root, MG_CACHE)
    if not os.path.exists(path):
        print('mobilegestalt cache: absent, nothing changed')
        return False
    st = os.stat(path)
    data = load(path)
    extra = data.setdefault('CacheExtra', {})
    for key, value in sorted(values.items()):
        print('mobilegestalt cache:', key, extra.get(key, '<unset>'), '->', value)
        extra[key] = value
    save(path, data, st.st_uid, st.st_gid, st.st_mode & 0o7777)
    return True


def launchd(root):
    path = os.path.join(root, 'System/Library/xpc/launchd.plist')
    st = os.stat(path)
    plist = load(path)
    jobs = plist['LaunchDaemons']
    renamed = 0
    for job in jobs.values():
        if '_PanicOnCrash' in job:
            job['_PanicOnCrasX'] = job.pop('_PanicOnCrash')
            renamed += 1
    removed = [k for k, j in jobs.items() if j.get('Label') == 'com.apple.CommCenter']
    for k in removed:
        del jobs[k]
    save(path, plist, st.st_uid, st.st_gid, st.st_mode & 0o7777)
    print(f'launchd.plist: {renamed} _PanicOnCrash renamed, {len(removed)} CommCenter jobs removed')


def containers(root):
    base = os.path.join(root, 'private/var/containers')
    for sub, uid in (('Bundle', 33), ('Data/System', 0), ('Shared/SystemGroup', 0), ('Temp', 0)):
        d = os.path.join(base, sub)
        if not os.path.isdir(d):
            os.makedirs(d)
            os.chown(d, uid, uid)
            os.chmod(d, 0o755)
            print('mkdir', d)


def find_container(root, kind, identifier):
    for meta in glob.glob(os.path.join(root, 'private/var/containers', kind, '*',
                                       '.com.apple.mobile_container_manager.metadata.plist')):
        if load(meta).get('MCMMetadataIdentifier') == identifier:
            return os.path.dirname(meta)
    return None


def activation(root):
    c = find_container(root, 'Data/System', 'com.apple.mobileactivationd')
    if c is None:
        print('warning: no mobileactivationd system container in the image; skipped data ark')
        return
    d = os.path.join(c, 'Library/internal')
    os.makedirs(d, exist_ok=True)
    os.chown(d, MOBILE, MOBILE)
    os.chmod(d, 0o755)
    save(os.path.join(d, 'data_ark.plist'), {
        '-ActivationState': 'Activated', '-BrickState': False,
        '-BuildVersion': BUILD, '-ProductVersion': VERSION,
        '-DataMigrationCompleted': True, '-PreSydroDataMigrationCompleted': True,
    }, MOBILE, MOBILE, 0o600)
    print('data ark', d)


def audio(root):
    src = os.path.join(root, 'Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin')
    dst_dir = os.path.join(root, 'Library/Audio/Plug-Ins/HAL.disabled')
    if os.path.exists(src):
        os.makedirs(dst_dir, exist_ok=True)
        shutil.move(src, dst_dir)
        print('VirtualAudio.plugin disabled')
    for conf in glob.glob(os.path.join(root, 'System/Library/Frameworks/MediaToolbox.framework/*/SubServerConfiguration.plist')):
        st = os.stat(conf)
        data = load(conf)
        data['StartRecorderServer'] = False
        save(conf, data, st.st_uid, st.st_gid, st.st_mode & 0o7777, plistlib.FMT_XML)
        print('StartRecorderServer off', os.path.relpath(conf, root))


POSTERBOARD_JOB = '/System/Library/LaunchAngels/com.apple.PosterBoard.plist'
FEATURE_OVERRIDE = 'Library/Preferences/FeatureFlags/Domain/SpringBoard.plist'


def posterboard_oslog(root):
    """PosterBoard's os_log to stderr. launchd reads the job from the
    launchd.plist cache (launchd_unsecure_cache), so the cached entry is
    changed, not the LaunchAngels plist."""
    path = os.path.join(root, 'System/Library/xpc/launchd.plist')
    st = os.stat(path)
    plist = load(path)
    job = plist['LaunchDaemons'].get(POSTERBOARD_JOB)
    if job is None or job.get('Label') != 'com.apple.PosterBoard':
        sys.exit('launchd.plist: no com.apple.PosterBoard job at ' + POSTERBOARD_JOB)
    job.setdefault('EnvironmentVariables', {})['OS_ACTIVITY_DT_MODE'] = 'YES'
    save(path, plist, st.st_uid, st.st_gid, st.st_mode & 0o7777)
    print('launchd.plist: PosterBoard OS_ACTIVITY_DT_MODE=YES')


def oslog_jobs(root, labels):
    """OS_ACTIVITY_DT_MODE=YES for the cached launchd jobs with these labels."""
    path = os.path.join(root, 'System/Library/xpc/launchd.plist')
    st = os.stat(path)
    plist = load(path)
    by_label = {j.get('Label'): j for j in plist['LaunchDaemons'].values()}
    missing = [label for label in labels if label not in by_label]
    if missing:
        sys.exit('launchd.plist: no job labelled ' + ', '.join(missing))
    for label in labels:
        by_label[label].setdefault('EnvironmentVariables', {})['OS_ACTIVITY_DT_MODE'] = 'YES'
    save(path, plist, st.st_uid, st.st_gid, st.st_mode & 0o7777)
    print('launchd.plist: OS_ACTIVITY_DT_MODE=YES for', ', '.join(labels))


def job_env(root, specs):
    """LABEL:KEY=VALUE sets one environment variable on a cached launchd job
    (e.g. TU_DEBUG for backboardd); LABEL:KEY= removes it."""
    path = os.path.join(root, 'System/Library/xpc/launchd.plist')
    st = os.stat(path)
    plist = load(path)
    by_label = {j.get('Label'): j for j in plist['LaunchDaemons'].values()}
    changes = []
    for spec in specs:
        label, sep, assignment = spec.partition(':')
        key, eq, value = assignment.partition('=')
        if not (sep and eq and label and key):
            sys.exit('--job-env expects LABEL:KEY=VALUE, got ' + spec)
        if label not in by_label:
            sys.exit('launchd.plist: no job labelled ' + label)
        changes.append((by_label[label], key, value))
    for job, key, value in changes:
        env = job.setdefault('EnvironmentVariables', {})
        if value:
            env[key] = value
        else:
            env.pop(key, None)
            if not env:
                del job['EnvironmentVariables']
    save(path, plist, st.st_uid, st.st_gid, st.st_mode & 0o7777)
    print('launchd.plist: env', ', '.join(specs))


LS_DATABASES = 'private/var/mobile/Containers/Data/InternalDaemon/*/Library/Caches/*com.apple.LaunchServices-*.csstore'
CHRONO_SNAPSHOTS = 'private/var/mobile/Library/Caches/com.apple.chrono/snapshot-cache'


def rebuild_launchservices(root):
    """lsd rebuilds a missing database from the system app directories and
    asks installd for containerized content. The old one, built while AMFI
    request 0x60 failed, has no extension points (no com.apple.widgetkit-
    extension or com.apple.posterkit.provider records); chronod's cached
    snapshots are empty black placeholders of the never-run extensions."""
    removed = 0
    for path in glob.glob(os.path.join(root, LS_DATABASES)):
        os.remove(path)
        removed += 1
    snapshots = 0
    cache = os.path.join(root, CHRONO_SNAPSHOTS)
    if os.path.isdir(cache):
        for entry in os.listdir(cache):
            shutil.rmtree(os.path.join(cache, entry))
            snapshots += 1
    print(f'LaunchServices: {removed} database(s) removed; chrono: {snapshots} snapshot dir(s) removed')


STAGED_APPS = 'private/var/staged_system_apps'
PLACEHOLDERS = 'System/Library/AppPlaceholders'
PARKED_PLACEHOLDERS = 'System/Library/AppPlaceholders.fuxi-staged'
STAGED_MANIFEST = '.fuxi-installed-staged-apps.plist'
EXTENSION_DIRS = ('PlugIns', 'Extensions')
# Info.plist keys that launchd.plist's AppExtensions cache leaves out. This
# reproduces all 129 cached /Applications/*/PlugIns entries of 20H392 exactly.
CACHE_DROPPED_KEYS = {
    'BuildMachineOSBuild', 'CFBundleDevelopmentRegion', 'CFBundleDisplayName',
    'CFBundleInfoDictionaryVersion', 'CFBundleShortVersionString', 'CFBundleSignature',
    'CFBundleSupportedPlatforms', 'MinimumOSVersion', 'NSHumanReadableCopyright',
    'UIApplicationShowsViewsWhileLocked', 'UIDeviceFamily', 'UIRequiredDeviceCapabilities'}
CACHE_DROPPED_PREFIXES = ('DT', 'CFBundleIcons', 'UISupportedInterfaceOrientations')


def extension_cache_entry(bundle_path, info):
    kept = {k: v for k, v in info.items()
            if k not in CACHE_DROPPED_KEYS and not k.startswith(CACHE_DROPPED_PREFIXES)}
    return {'_executablePath': bundle_path + '/' + info['CFBundleExecutable'], '_infoPlist': kept}


def app_extensions(root, app_path):
    """(cache key, cache entry) for the extensions of /Applications/NAME.app."""
    for sub in EXTENSION_DIRS:
        for appex in sorted(glob.glob(os.path.join(root, app_path.lstrip('/'), sub, '*.appex'))):
            info = os.path.join(appex, 'Info.plist')
            if not os.path.isfile(info):
                continue
            data = load(info)
            if 'CFBundleExecutable' not in data:
                continue
            key = app_path + '/' + sub + '/' + os.path.basename(appex)
            yield key, extension_cache_entry(key, data)


def placeholders_by_id(root):
    found = {}
    for app in glob.glob(os.path.join(root, PLACEHOLDERS, '*.app')):
        info = os.path.join(app, 'Info.plist')
        if os.path.isfile(info):
            found[load(info).get('CFBundleIdentifier')] = app
    return found


def install_staged_apps(root):
    """What HackiOS does for the same 16.x images: the staged apps become
    plain system apps in /Applications. Without this their placeholders are
    what LaunchServices knows ("Waiting..." labels) and the default widgets
    (Weather, Calendar, Maps, Notes, Stocks, Tips) have no extension to run."""
    staged = os.path.join(root, STAGED_APPS)
    parked = os.path.join(root, PARKED_PLACEHOLDERS)
    manifest_path = os.path.join(parked, STAGED_MANIFEST)
    manifest = load(manifest_path) if os.path.exists(manifest_path) else []
    done = {e['name'] for e in manifest}
    placeholders = placeholders_by_id(root)
    path = os.path.join(root, 'System/Library/xpc/launchd.plist')
    st = os.stat(path)
    plist = load(path)
    cache = plist.setdefault('AppExtensions', {})
    moved = skipped = extensions = 0
    for app in sorted(glob.glob(os.path.join(staged, '*.app'))):
        name = os.path.basename(app)
        dest = os.path.join(root, 'Applications', name)
        info = os.path.join(app, 'Info.plist')
        if os.path.exists(dest) or not os.path.isfile(info):
            print('staged app skipped (exists in /Applications or no Info.plist):', name)
            skipped += 1
            continue
        bundle_id = load(info).get('CFBundleIdentifier')
        os.rename(app, dest)
        entry = {'name': name, 'bundleIdentifier': bundle_id, 'placeholder': ''}
        placeholder = placeholders.get(bundle_id)
        if placeholder:
            os.makedirs(parked, mode=0o755, exist_ok=True)
            os.rename(placeholder, os.path.join(parked, os.path.basename(placeholder)))
            entry['placeholder'] = os.path.basename(placeholder)
        for key, value in app_extensions(root, '/Applications/' + name):
            cache[key] = value
            extensions += 1
        if name not in done:
            manifest.append(entry)
        moved += 1
    save(path, plist, st.st_uid, st.st_gid, st.st_mode & 0o7777)
    if manifest:
        os.makedirs(parked, mode=0o755, exist_ok=True)
        save(manifest_path, manifest, st.st_uid, st.st_gid, 0o644, plistlib.FMT_XML)
    print(f'staged apps: {moved} moved to /Applications, {skipped} skipped, '
          f'{extensions} extension(s) added to the launchd.plist cache')


def uninstall_staged_apps(root):
    parked = os.path.join(root, PARKED_PLACEHOLDERS)
    manifest_path = os.path.join(parked, STAGED_MANIFEST)
    if not os.path.exists(manifest_path):
        print('staged apps: nothing to undo')
        return
    path = os.path.join(root, 'System/Library/xpc/launchd.plist')
    st = os.stat(path)
    plist = load(path)
    cache = plist.get('AppExtensions', {})
    restored = 0
    for entry in load(manifest_path):
        name = entry['name']
        prefix = '/Applications/' + name + '/'
        for key in [k for k in cache if k.startswith(prefix)]:
            del cache[key]
        src = os.path.join(root, 'Applications', name)
        if os.path.isdir(src):
            os.rename(src, os.path.join(root, STAGED_APPS, name))
            restored += 1
        if entry.get('placeholder'):
            p = os.path.join(parked, entry['placeholder'])
            if os.path.isdir(p):
                os.rename(p, os.path.join(root, PLACEHOLDERS, entry['placeholder']))
    save(path, plist, st.st_uid, st.st_gid, st.st_mode & 0o7777)
    os.remove(manifest_path)
    if not os.listdir(parked):
        os.rmdir(parked)
    print(f'staged apps: {restored} moved back to {STAGED_APPS}')


def legacy_wallpaper(root, enable):
    """libsystem_featureflags merges /Library/Preferences/FeatureFlags over
    /System/Library/FeatureFlags. Autobahn is the iOS 16 poster lock screen;
    with it off PosterBoard answers "Unsupported on this platform" (its iPad
    branch) and SpringBoard keeps the legacy wallpaper."""
    path = os.path.join(root, FEATURE_OVERRIDE)
    if not enable:
        if os.path.exists(path):
            data = load(path)
            data.pop('Autobahn', None)
            if data:
                save(path, data, 0, 0, 0o644)
            else:
                os.remove(path)
        print('feature flags: SpringBoard/Autobahn override removed')
        return
    d = os.path.dirname(path)
    for sub in (os.path.dirname(d), d):
        if not os.path.isdir(sub):
            os.makedirs(sub)
            os.chown(sub, 0, 0)
            os.chmod(sub, 0o755)
    data = load(path) if os.path.exists(path) else {}
    data['Autobahn'] = {'Enabled': False}
    save(path, data, 0, 0, 0o644, plistlib.FMT_XML)
    print('feature flags: SpringBoard/Autobahn disabled (legacy wallpaper)')


WALLPAPER_DIR = 'private/var/mobile/Library/SpringBoard'
WALLPAPER_FILES = ('LockBackground.cpbitmap', 'HomeBackground.cpbitmap')
SCREEN = (1125, 2436)                 # iPhone X pixels, what UIKit sees (patch 0061)
CPBITMAP_MAGIC = 0xdcb543a2           # 28-byte trailer, rows padded to 16 pixels


def cpbitmap(width, height, bgra, scale=3.0):
    """AppSupport CPBitmap as CPBitmapCreateImagesFromData reads it (checked
    against its disassembly and the cpbitmaps SpringBoard writes itself):
    BGRA rows padded to 16 pixels, a binary plist with the scale, then the
    trailer <colorspace-plist len, extra len, width, height, format 0 (32-bit
    BGRX), image count, magic> (7 x u32 LE)."""
    row = width * 4
    if len(bgra) != row * height:
        raise ValueError('pixel data is not width*height*4 bytes')
    stride = (width + 15) // 16 * 64
    pad = bytes(stride - row)
    rows = b''.join(bgra[y * row:(y + 1) * row] + pad for y in range(height))
    extra = plistlib.dumps(float(scale), fmt=plistlib.FMT_BINARY)
    return rows + extra + struct.pack('<7I', 0, len(extra), width, height, 0, 1, CPBITMAP_MAGIC)


# Light blue -> light lavender: mean Rec.709 luma ~0.91, above UIKit's 0.86
# "light background" threshold, so PaperBoardUI shows it untreated (see
# legibility_style). The first gradient, #1d3b72 -> #6a3d9a (luma ~0.26),
# was style 1 and stayed undrawn on device (boot ios-0098-wlan5: white).
GRADIENT = ((0xdf, 0xe9, 0xfb), (0xf1, 0xe4, 0xfb))


def gradient(width, height, top=GRADIENT[0], bottom=GRADIENT[1]):
    """Plain vertical gradient, tight BGRA rows."""
    out = bytearray()
    for y in range(height):
        t = y / max(1, height - 1)
        r, g, b = (round(a + (c - a) * t) for a, c in zip(top, bottom))
        out += bytes((b, g, r, 255)) * width
    return bytes(out)


def legibility_style(bgra):
    """UIKit -[_UILegibilitySettingsProvider accumulateChangesToContentColor:contrast:]
    (20H392) on the mean color, which is what -[PBUIWallpaperView legibilitySettings]
    feeds it (average of PBUIColorBoxes). 2 = dark content on a light image,
    1 = white content. The contrast branch (brightness > 0.8 and contrast <
    0.09) is left out: it needs PBUI's box contrast, and the other two rules
    are enough for a light image. Returns (style, luma, brightness, saturation).

    -[PBUIStaticWallpaperView imageRequiresLuminanceTreatment] is style != 2.
    A treated image goes to PBUICARemoteRenderer -> SBRendererService ->
    PBUICALocalRenderer, which is nil without an MTLDevice, so the reply
    has no IOSurface and the display image is never set."""
    n = len(bgra) // 4
    b, g, r = (sum(bgra[i::4]) / n / 255 for i in range(3))
    luma = 0.2126 * r + 0.7152 * g + 0.0722 * b
    bright = max(r, g, b)
    sat = (bright - min(r, g, b)) / bright if bright else 0.0
    light = luma > 0.86 or (sat < 0.1 and bright + 2 * (0.1 - sat) > 0.92)
    return (2 if light else 1), luma, bright, sat


def read_png(path):
    """Minimal decoder for 8-bit, non-interlaced RGB/RGBA PNG -> (w, h, BGRA)."""
    data = open(path, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError(path + ': not a PNG')
    pos, idat, hdr = 8, b'', None
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if kind == b'IHDR':
            hdr = struct.unpack('>IIBBBBB', body)
        elif kind == b'IDAT':
            idat += body
        elif kind == b'IEND':
            break
    w, h, depth, ctype, _, _, interlace = hdr
    if depth != 8 or ctype not in (2, 6) or interlace:
        raise ValueError(path + ': need an 8-bit non-interlaced RGB or RGBA PNG')
    bpp = 3 if ctype == 2 else 4
    raw, stride = zlib.decompress(idat), w * bpp
    prev, out = bytearray(stride), bytearray()
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        for x in range(w):
            r, g, b = line[x * bpp:x * bpp + 3]
            out += bytes((b, g, r, 255))
        prev = line
    return w, h, bytes(out)


def legacy_wallpaper_image(root, png, owner=(MOBILE, MOBILE)):
    """PaperBoardUI's legacy wallpaper (SpringBoard with Autobahn off) reads
    a static wallpaper from PBUIWallpaperDirectoryDataStore =
    /var/mobile/Library/SpringBoard/{Lock,Home}Background.cpbitmap. Without
    one (and no color/gradient/procedural defaults) it uses
    fallbackWallpaperConfigurationForVarient: = black; restoreDefaultWallpaper
    refuses on iPhone ("Hello NOT iPad ..."). The image has to be light, see
    legibility_style."""
    if png in (None, 'gradient'):
        w, h, pixels = SCREEN[0], SCREEN[1], gradient(*SCREEN)
        src = 'built-in gradient'
    else:
        w, h, pixels = read_png(png)
        src = os.path.basename(png)
    style, luma, bright, sat = legibility_style(pixels)
    if style != 2:
        print(f'warning: {src} is dark (mean luma {luma:.2f}, brightness {bright:.2f}, '
              f'saturation {sat:.2f}): PaperBoardUI requests a luminance treatment from '
              'SBRendererService, which needs Metal; expect an undrawn (white) wallpaper. '
              'Use an image with mean luma > 0.86.', file=sys.stderr)
    blob = cpbitmap(w, h, pixels)
    d = os.path.join(root, WALLPAPER_DIR)
    if not os.path.isdir(d):
        os.makedirs(d)
        os.chown(d, *owner)
        os.chmod(d, 0o755)
    for name in WALLPAPER_FILES:
        path = os.path.join(d, name)
        with open(path, 'wb') as f:
            f.write(blob)
        os.chown(path, *owner)
        os.chmod(path, 0o644)
    set_wallpaper_color(root, None, owner)
    remove_derived(root)
    print(f'legacy wallpaper: {src} {w}x{h} (legibility style {style}, luma {luma:.2f}) -> '
          f'{", ".join(WALLPAPER_FILES)} ({len(blob)} bytes each)')


def remove_legacy_wallpaper_image(root):
    for name in WALLPAPER_FILES:
        path = os.path.join(root, WALLPAPER_DIR, name)
        if os.path.exists(path):
            os.remove(path)
    print('legacy wallpaper: cpbitmaps removed')


# PBUIWallpaperUserDefaultsDataStore = PBUIWallpaperDefaultsDomain (suite
# com.apple.springboard): SBLockScreenWallpapers (variant 0) /
# SBHomeScreenWallpapers (variant 1), each a dict keyed by
# PBUIStringForWallpaperMode (mode 0 = 'none', what SpringBoard already writes),
# read by -[PBUIWallpaperDefaultsWrapper initWithWallpaperDefaultsDict:].
# -[PBUIWallpaperConfigurationManager wallpaperConfigurationForVariant:...]
# asks every data store for procedural, colour, gradient, then image; a colour
# ends the search. The colour is decoded with
# +[NSKeyedUnarchiver unarchivedObjectOfClass:UIColor ...]; -[UIColor
# initWithCoder:] takes UIColorComponentCount 4 + UIRed/UIGreen/UIBlue/UIAlpha.
WALLPAPER_PREFS = ('SBLockScreenWallpapers', 'SBHomeScreenWallpapers')
WALLPAPER_MODE = 'none'
COLOR_KEYS = ('SBWallpaperColorDataKey', 'SBWallpaperColorNameKey', 'SBWallpaperGradientDataKey')
WALLPAPER_COLOR = '#2e3a6e'
# Regenerated by SpringBoard from the current wallpaper; stale ones are wrong.
WALLPAPER_DERIVED = ('private/var/mobile/Library/SpringBoard/LockBackgroundThumbnail.jpg',
                     'private/var/mobile/Library/SpringBoard/HomeBackgroundThumbnail.jpg',
                     'private/var/mobile/Library/Caches/MappedImageCache/Wallpaper/*',
                     'private/var/mobile/Library/Caches/com.apple.springboard/WallpaperColorBoxesCache/*')


def parse_color(text):
    t = text.lstrip('#')
    if len(t) != 6 or any(c not in '0123456789abcdefABCDEF' for c in t):
        raise ValueError('colour must be #RRGGBB: ' + text)
    return tuple(int(t[i:i + 2], 16) for i in (0, 2, 4))


def archived_uicolor(rgb):
    """NSKeyedArchiver archive of an sRGB UIColor, as -[UIColor encodeWithCoder:] writes it."""
    r, g, b = (c / 255 for c in rgb)
    return plistlib.dumps({
        '$version': 100000, '$archiver': 'NSKeyedArchiver', '$top': {'root': plistlib.UID(1)},
        '$objects': ['$null',
                     {'$class': plistlib.UID(2), 'UIColorComponentCount': 4, 'UIRed': r,
                      'UIGreen': g, 'UIBlue': b, 'UIAlpha': 1.0,
                      'NSRGB': f'{r:.6g} {g:.6g} {b:.6g}'.encode() + b'\0', 'NSColorSpace': 2},
                     {'$classname': 'UIColor', '$classes': ['UIColor', 'NSObject']}]},
        fmt=plistlib.FMT_BINARY)


def set_wallpaper_color(root, rgb, owner=(MOBILE, MOBILE)):
    """rgb None removes the colour/gradient keys (the image or black fallback applies)."""
    path = os.path.join(root, 'private/var/mobile/Library/Preferences/com.apple.springboard.plist')
    if not os.path.exists(path):
        if rgb is None:
            return
        os.makedirs(os.path.dirname(path), exist_ok=True)
    data = load(path) if os.path.exists(path) else {}
    for key in WALLPAPER_PREFS:
        entry = data.setdefault(key, {}).setdefault(WALLPAPER_MODE, {})
        for k in COLOR_KEYS:
            entry.pop(k, None)
        if rgb is not None:
            entry['SBWallpaperColorDataKey'] = archived_uicolor(rgb)
    save(path, data, *owner, 0o600)


def remove_derived(root):
    for pattern in WALLPAPER_DERIVED:
        for path in glob.glob(os.path.join(root, pattern)):
            if os.path.isfile(path):
                os.remove(path)


def legacy_wallpaper_color(root, color, owner=(MOBILE, MOBILE)):
    rgb = parse_color(color)
    set_wallpaper_color(root, rgb, owner)
    remove_legacy_wallpaper_image(root)
    remove_derived(root)
    print('legacy wallpaper: colour #%02x%02x%02x (lock + home), derived caches removed' % rgb)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('root')
    ap.add_argument('--posterboard-oslog', action='store_true')
    group = ap.add_mutually_exclusive_group()
    group.add_argument('--legacy-wallpaper', action='store_true')
    group.add_argument('--no-legacy-wallpaper', action='store_true')
    image = ap.add_mutually_exclusive_group()
    image.add_argument('--legacy-wallpaper-image', nargs='?', const='gradient', metavar='PNG')
    image.add_argument('--no-legacy-wallpaper-image', action='store_true')
    image.add_argument('--legacy-wallpaper-color', nargs='?', const=WALLPAPER_COLOR, metavar='#RRGGBB')
    image.add_argument('--no-legacy-wallpaper-color', action='store_true')
    ap.add_argument('--oslog', metavar='LABEL[,LABEL...]')
    ap.add_argument('--job-env', action='append', metavar='LABEL:KEY=VALUE')
    ap.add_argument('--rebuild-launchservices', action='store_true')
    ap.add_argument('--mg-cache-extra', action='append', metavar='KEY=true|false')
    staged = ap.add_mutually_exclusive_group()
    staged.add_argument('--install-staged-apps', action='store_true')
    staged.add_argument('--no-install-staged-apps', action='store_true')
    transparency = ap.add_mutually_exclusive_group()
    transparency.add_argument('--reduce-transparency', action='store_true')
    transparency.add_argument('--no-reduce-transparency', action='store_true')
    ap.add_argument('--only-optional', action='store_true')
    args = ap.parse_args()
    root = args.root
    if not os.path.isdir(os.path.join(root, 'System/Library/xpc')):
        ap.error('not a mounted iOS root: ' + root)
    if args.posterboard_oslog:
        posterboard_oslog(root)
    if args.oslog:
        oslog_jobs(root, [label for label in args.oslog.split(',') if label])
    if args.job_env:
        job_env(root, args.job_env)
    if args.mg_cache_extra:
        mg_cache_extra(root, args.mg_cache_extra)
    if args.install_staged_apps:
        install_staged_apps(root)
    if args.no_install_staged_apps:
        uninstall_staged_apps(root)
    if args.rebuild_launchservices:
        rebuild_launchservices(root)
    if args.reduce_transparency or args.no_reduce_transparency:
        reduce_transparency(root, args.reduce_transparency)
    if args.legacy_wallpaper or args.no_legacy_wallpaper:
        legacy_wallpaper(root, args.legacy_wallpaper)
    if args.legacy_wallpaper_image:
        legacy_wallpaper_image(root, args.legacy_wallpaper_image)
    if args.no_legacy_wallpaper_image:
        remove_legacy_wallpaper_image(root)
    if args.legacy_wallpaper_color:
        try:
            legacy_wallpaper_color(root, args.legacy_wallpaper_color)
        except ValueError as e:
            ap.error(str(e))
    if args.no_legacy_wallpaper_color:
        set_wallpaper_color(root, None)
        print('legacy wallpaper: colour removed')
    if args.only_optional:
        return
    launchd(root)
    containers(root)
    activation(root)
    audio(root)
    merge_pref(root, 'com.apple.migration', {
        'LastSystemVersion': BUILD,
        'DMUserDataDisposition': {'SyntheticDidUpgrade': False, 'buildVersion': BUILD}})
    merge_pref(root, 'com.apple.springboard', {
        'BKNoWatchdogs': True,
        'FBLaunchWatchdogExceptions': {'com.apple.purplebuddy': 100.0},
        'MCForceShowCloudConfigurationUI': False})
    merge_pref(root, 'com.apple.purplebuddy', {
        'SetupDone': True, 'SetupFinishedAllSteps': True, 'SetupVersion': 11,
        'daemonizeNeedsToRun': False})
    merge_pref(root, 'com.apple.purplebuddy.notbackedup', {
        k: False for k in ('ShouldRunDiagnosticsMiniBuddy', 'ShouldRunHSA2MiniBuddy',
                           'ShouldRunKeychainSyncMiniBuddy', 'ShouldRunMesaMiniBuddy',
                           'ShouldRunPaymentMiniBuddy', 'ShouldRunStandardMiniBuddy',
                           'SilentICDPUpgrade')})
    merge_pref(root, 'com.apple.managedconfiguration.notbackedup', {
        'LockdownActivationIndicatesCloudConfigurationAvailable': False})


if __name__ == '__main__':
    main()
