#!/usr/bin/env python3
"""Host-side checks for the legacy wallpaper step of sweet-root-tweaks.py."""
import importlib.util, os, plistlib, struct, tempfile, zlib
from pathlib import Path

spec = importlib.util.spec_from_file_location('tweaks', Path(__file__).with_name('sweet-root-tweaks.py'))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def parse_cpbitmap(blob):
    """Mirror of AppSupport CPBitmapCreateImagesFromData for the 28-byte
    trailer, format 0 path (20H392 disassembly)."""
    cs_len, extra_len, w, h, fmt, count, magic = struct.unpack('<7I', blob[-28:])
    assert magic == 0xdcb543a2 and fmt == 0 and count == 1 and cs_len == 0
    stride = (w + 15) // 16 * 16 * 4
    assert stride * h + extra_len + cs_len + 28 == len(blob), 'length check'
    extra = blob[stride * h:stride * h + extra_len]
    return w, h, stride, extra


# Trailer + extra plist exactly as in SpringBoard's own 3x7 highlight cpbitmap.
blob = m.cpbitmap(3, 7, bytes(3 * 7 * 4))
assert len(blob) == 526
assert blob[-78:] == bytes.fromhex(
    '62706c6973743030234008000000000000080000000000000101000000000000000100000000000000000000000000000011'
    '00000000' '32000000' '03000000' '07000000' '00000000' '01000000' 'a243b5dc')

w, h = m.SCREEN
blob = m.cpbitmap(w, h, m.gradient(w, h))
pw, ph, stride, extra = parse_cpbitmap(blob)
assert (pw, ph, stride) == (1125, 2436, 1136 * 4)
first = blob[:4]
last = blob[stride * (h - 1):stride * (h - 1) + 4]
(tr, tg, tb), (br, bg, bb) = m.GRADIENT
assert first == bytes((tb, tg, tr, 255)) and last == bytes((bb, bg, br, 255))

# Legibility: the built-in gradient must be "light" (style 2) with margin, so
# PaperBoardUI displays it without the Metal-only luminance treatment; the
# first, dark gradient (device: white, undrawn wallpaper) is style 1.
style, luma, _, _ = m.legibility_style(m.gradient(w, h))
assert style == 2 and luma > 0.89, (style, luma)
dark = m.gradient(64, 64, (0x1d, 0x3b, 0x72), (0x6a, 0x3d, 0x9a))
assert m.legibility_style(dark)[0] == 1
assert m.legibility_style(bytes((255, 255, 255, 255)) * 16)[0] == 2
assert m.legibility_style(bytes((0xd8, 0xd8, 0xd8, 255)) * 16)[0] == 2     # gray rule
assert m.legibility_style(bytes((0xa0, 0xa0, 0xa0, 255)) * 16)[0] == 1     # gray < 0.72


def png(w, h, rgb_rows, filters):
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    raw = b''
    prev = bytes(w * 3)
    for row, f in zip(rgb_rows, filters):
        enc = bytearray(row)
        for i in range(len(row)):
            a = row[i - 3] if i >= 3 else 0
            if f == 1:
                enc[i] = (row[i] - a) & 255
            elif f == 2:
                enc[i] = (row[i] - prev[i]) & 255
        raw += bytes((f,)) + bytes(enc)
        prev = row
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


rows = [bytes(v for x in range(5) for v in (10 * x + y, 20 + y, 200 - x)) for y in range(3)]
with tempfile.TemporaryDirectory() as tmp:
    p = os.path.join(tmp, 'w.png')
    open(p, 'wb').write(png(5, 3, rows, (0, 1, 2)))
    pw, ph, bgra = m.read_png(p)
    assert (pw, ph) == (5, 3)
    for y in range(3):
        for x in range(5):
            r, g, b = rows[y][x * 3:x * 3 + 3]
            assert bgra[(y * 5 + x) * 4:(y * 5 + x) * 4 + 4] == bytes((b, g, r, 255))

    root = os.path.join(tmp, 'root')
    owner = (os.getuid(), os.getgid())
    m.legacy_wallpaper_image(root, p, owner)
    for name in m.WALLPAPER_FILES:
        f = os.path.join(root, m.WALLPAPER_DIR, name)
        assert parse_cpbitmap(open(f, 'rb').read())[:2] == (5, 3)
        assert os.stat(f).st_mode & 0o777 == 0o644
    m.remove_legacy_wallpaper_image(root)
    assert not any(os.path.exists(os.path.join(root, m.WALLPAPER_DIR, n)) for n in m.WALLPAPER_FILES)

    # Colour wallpaper: archived UIColor in both variants' 'none' entry, other
    # keys kept, image files and derived caches gone.
    prefs = os.path.join(root, 'private/var/mobile/Library/Preferences/com.apple.springboard.plist')
    os.makedirs(os.path.dirname(prefs))
    plistlib.dump({'BKNoWatchdogs': True, 'SBLockScreenWallpapers': {'none': {
        'SBWallpaperParallaxFactorKey': 1.0, 'SBWallpaperColorNameKey': 'red'}}},
        open(prefs, 'wb'), fmt=plistlib.FMT_BINARY)
    m.legacy_wallpaper_image(root, None, owner)
    cache = os.path.join(root, 'private/var/mobile/Library/Caches/MappedImageCache/Wallpaper')
    os.makedirs(cache)
    open(os.path.join(cache, 'WallpaperPreview-lock-120.000000.cpbitmap'), 'wb').write(b'x')
    open(os.path.join(root, m.WALLPAPER_DIR, 'LockBackgroundThumbnail.jpg'), 'wb').write(b'x')
    m.legacy_wallpaper_color(root, '#2E3A6E', owner)
    d = plistlib.load(open(prefs, 'rb'))
    assert d['BKNoWatchdogs'] and d['SBLockScreenWallpapers']['none']['SBWallpaperParallaxFactorKey'] == 1.0
    for key in m.WALLPAPER_PREFS:
        e = d[key]['none']
        assert 'SBWallpaperColorNameKey' not in e
        a = plistlib.loads(e['SBWallpaperColorDataKey'])
        top = a['$objects'][a['$top']['root'].data]
        assert a['$objects'][top['$class'].data]['$classname'] == 'UIColor'
        assert top['UIColorComponentCount'] == 4 and top['UIAlpha'] == 1.0
        assert [round(top[c] * 255) for c in ('UIRed', 'UIGreen', 'UIBlue')] == [0x2e, 0x3a, 0x6e]
    assert os.listdir(cache) == []
    assert os.listdir(os.path.join(root, m.WALLPAPER_DIR)) == []
    m.set_wallpaper_color(root, None, owner)
    d = plistlib.load(open(prefs, 'rb'))
    assert all('SBWallpaperColorDataKey' not in d[k]['none'] for k in m.WALLPAPER_PREFS)
    try:
        m.parse_color('2e3a6')
        raise AssertionError('short colour accepted')
    except ValueError:
        pass


def write_plist(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    plistlib.dump(data, open(path, 'wb'), fmt=plistlib.FMT_BINARY)


# Staged system apps, LaunchServices reset and os_log jobs on a synthetic root.
with tempfile.TemporaryDirectory() as root:
    xpc = os.path.join(root, 'System/Library/xpc/launchd.plist')
    batteries = '/Applications/Batteries.app/PlugIns/BatteriesWidgetExtension.appex'
    write_plist(xpc, {'LaunchDaemons': {
        '/System/Library/LaunchDaemons/com.apple.chronod.plist': {
            'Label': 'com.apple.chronod', 'EnvironmentVariables': {'MallocStackLoggingDirectory': '/x'}},
        '/System/Library/LaunchDaemons/com.apple.lsd.plist': {'Label': 'com.apple.lsd'}},
        'AppExtensions': {batteries: {'_executablePath': batteries + '/BatteriesWidgetExtension',
                                      '_infoPlist': {'CFBundleIdentifier': 'com.apple.Batteries.BatteriesWidget'}}}})
    widget = {'CFBundleExecutable': 'WeatherWidget', 'CFBundleIdentifier': 'com.apple.weather.widget',
              'CFBundleName': 'WeatherWidget', 'CFBundlePackageType': 'XPC!', 'CFBundleVersion': '359',
              'NSExtension': {'NSExtensionPointIdentifier': 'com.apple.widgetkit-extension'},
              'NSWidgetBackgroundColorName': 'WidgetBackground', 'DTSDKBuild': '20H218',
              'CFBundleDisplayName': 'WeatherWidget', 'UIDeviceFamily': [1, 2],
              'CFBundleIcons': {}, 'MinimumOSVersion': '16.7'}
    staged = os.path.join(root, m.STAGED_APPS, 'Weather.app')
    write_plist(os.path.join(staged, 'Info.plist'), {'CFBundleIdentifier': 'com.apple.weather', 'CFBundleExecutable': 'Weather'})
    write_plist(os.path.join(staged, 'PlugIns/WeatherWidget.appex/Info.plist'), widget)
    write_plist(os.path.join(staged, 'Extensions/Focus.appex/Info.plist'),
                {'CFBundleExecutable': 'Focus', 'CFBundleIdentifier': 'com.apple.weather.focus'})
    write_plist(os.path.join(root, m.PLACEHOLDERS, 'Weather.app/Info.plist'), {'CFBundleIdentifier': 'com.apple.weather'})
    write_plist(os.path.join(root, m.PLACEHOLDERS, 'Books.app/Info.plist'), {'CFBundleIdentifier': 'com.apple.iBooks'})
    write_plist(os.path.join(root, m.STAGED_APPS, 'Books.app/Info.plist'), {'CFBundleIdentifier': 'com.apple.iBooks'})
    os.makedirs(os.path.join(root, 'Applications/Books.app'))         # already present: left alone

    m.install_staged_apps(root)
    m.install_staged_apps(root)                                        # idempotent
    assert os.path.isdir(os.path.join(root, 'Applications/Weather.app/PlugIns/WeatherWidget.appex'))
    assert not os.path.exists(staged)
    assert not os.path.exists(os.path.join(root, m.PLACEHOLDERS, 'Weather.app'))
    assert os.path.isdir(os.path.join(root, m.PARKED_PLACEHOLDERS, 'Weather.app'))
    assert os.path.isdir(os.path.join(root, m.PLACEHOLDERS, 'Books.app'))
    assert os.path.isdir(os.path.join(root, m.STAGED_APPS, 'Books.app'))
    cache = plistlib.load(open(xpc, 'rb'))['AppExtensions']
    key = '/Applications/Weather.app/PlugIns/WeatherWidget.appex'
    assert cache[key]['_executablePath'] == key + '/WeatherWidget'
    assert cache[key]['_infoPlist'] == {k: widget[k] for k in (
        'CFBundleExecutable', 'CFBundleIdentifier', 'CFBundleName', 'CFBundlePackageType',
        'CFBundleVersion', 'NSExtension', 'NSWidgetBackgroundColorName')}, cache[key]['_infoPlist']
    assert '/Applications/Weather.app/Extensions/Focus.appex' in cache and batteries in cache
    manifest = plistlib.load(open(os.path.join(root, m.PARKED_PLACEHOLDERS, m.STAGED_MANIFEST), 'rb'))
    assert [e['name'] for e in manifest] == ['Weather.app']

    m.uninstall_staged_apps(root)
    assert os.path.isdir(staged) and os.path.isdir(os.path.join(root, m.PLACEHOLDERS, 'Weather.app'))
    assert not os.path.exists(os.path.join(root, 'Applications/Weather.app'))
    assert not os.path.exists(os.path.join(root, m.PARKED_PLACEHOLDERS))
    assert set(plistlib.load(open(xpc, 'rb'))['AppExtensions']) == {batteries}

    caches = os.path.join(root, 'private/var/mobile/Containers/Data/InternalDaemon/FE51/Library/Caches')
    os.makedirs(caches)
    for name in ('com.apple.LaunchServices-4035-v2.csstore',
                 'SystemDataOnly-com.apple.LaunchServices-4035-v2.csstore', 'keep.db'):
        open(os.path.join(caches, name), 'wb').write(b'x')
    snap = os.path.join(root, m.CHRONO_SNAPSHOTS, 'com.apple.weather.widget/com.apple.weather')
    os.makedirs(snap)
    open(os.path.join(snap, 'systemSmall.snapshot'), 'wb').write(b'x')
    m.rebuild_launchservices(root)
    assert os.listdir(caches) == ['keep.db']
    assert os.listdir(os.path.join(root, m.CHRONO_SNAPSHOTS)) == []

    m.oslog_jobs(root, ['com.apple.chronod', 'com.apple.lsd'])
    jobs = plistlib.load(open(xpc, 'rb'))['LaunchDaemons']
    env = jobs['/System/Library/LaunchDaemons/com.apple.chronod.plist']['EnvironmentVariables']
    assert env == {'MallocStackLoggingDirectory': '/x', 'OS_ACTIVITY_DT_MODE': 'YES'}
    assert jobs['/System/Library/LaunchDaemons/com.apple.lsd.plist']['EnvironmentVariables']['OS_ACTIVITY_DT_MODE'] == 'YES'
    try:
        m.oslog_jobs(root, ['com.apple.nonexistent'])
        raise AssertionError('missing label accepted')
    except SystemExit:
        pass

    m.job_env(root, ['com.apple.lsd:TU_DEBUG=sysmem'])
    jobs = plistlib.load(open(xpc, 'rb'))['LaunchDaemons']
    assert jobs['/System/Library/LaunchDaemons/com.apple.lsd.plist']['EnvironmentVariables']['TU_DEBUG'] == 'sysmem'
    m.job_env(root, ['com.apple.lsd:TU_DEBUG='])
    jobs = plistlib.load(open(xpc, 'rb'))['LaunchDaemons']
    assert 'TU_DEBUG' not in jobs['/System/Library/LaunchDaemons/com.apple.lsd.plist']['EnvironmentVariables']
    for bad in (['com.apple.lsd'], ['com.apple.nonexistent:A=1'], [':A=1']):
        try:
            m.job_env(root, bad)
            raise AssertionError('bad spec accepted: %r' % bad)
        except SystemExit:
            pass

with tempfile.TemporaryDirectory() as root:
    owner = (os.getuid(), os.getgid())
    ax = os.path.join(root, 'private/var/mobile/Library/Preferences/com.apple.Accessibility.plist')
    m.reduce_transparency(root, False, owner)                     # absent: no file created
    assert not os.path.exists(ax)
    m.reduce_transparency(root, True, owner)
    assert plistlib.load(open(ax, 'rb')) == {'EnhancedBackgroundContrastEnabled': True}
    assert os.stat(ax).st_mode & 0o777 == 0o600
    d = plistlib.load(open(ax, 'rb')); d['Other'] = 1
    plistlib.dump(d, open(ax, 'wb'), fmt=plistlib.FMT_BINARY)
    m.reduce_transparency(root, False, owner)
    assert plistlib.load(open(ax, 'rb')) == {'Other': 1}             # other keys kept

with tempfile.TemporaryDirectory() as root:
    assert m.mg_cache_extra(root, [m.MG_DEEP_COLOR + '=false']) is False      # absent: not created
    path = os.path.join(root, m.MG_CACHE)
    assert not os.path.exists(path)
    os.makedirs(os.path.dirname(path))
    plistlib.dump({'CacheVersion': '20H392', 'CacheData': b'x', 'CacheExtra': {'other': 1}}, open(path, 'wb'), fmt=plistlib.FMT_BINARY)
    os.chmod(path, 0o644)
    assert m.mg_cache_extra(root, [m.MG_DEEP_COLOR + '=false'])
    d = plistlib.load(open(path, 'rb'))
    assert d == {'CacheVersion': '20H392', 'CacheData': b'x', 'CacheExtra': {'other': 1, m.MG_DEEP_COLOR: False}}
    assert os.stat(path).st_mode & 0o777 == 0o644
    for bad in ['x', 'k=1', '=true']:
        try:
            m.mg_cache_extra(root, [bad])
            raise AssertionError('bad spec accepted: %r' % bad)
        except SystemExit:
            pass

print('PASS: cpbitmap trailer/length/pixels, legibility style, PNG decode, wallpaper files, colour wallpaper prefs, '
      'staged apps install/undo, LaunchServices reset, oslog jobs, job env, reduce transparency, MobileGestalt CacheExtra')
