#!/usr/bin/env python3
"""Pinned external NativeMetal port; generated paths/build products stay outside git.

--prepare-only verifies/applies both patch stacks and writes the cdylib manifest
on Linux. Production never embeds AIR fixtures or needs LLVM. --target host is
for Mac frontend tests; upstream fixture tests require a separate qualified setup.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PINS = dict(line.split('=', 1) for line in (ROOT/'tools/ci/pins.env').read_text().splitlines()
            if line and not line.startswith('#'))
NATIVE_PATCHES = sorted((ROOT/'tools/patches').glob('nativemetal-fuxi-*.patch'))
BRIDGE_PATCHES = [
    '0001-optional-constant-output-members.patch', '0002-resource-wait-storage-dependencies.patch',
    '0003-cache-device-entry-point-priming.patch', '0004-fold-constant-global-field-loads.patch',
    '0005-keep-module-decodes-and-profile-fingerprints.patch', '0006-skip-draws-without-a-pipeline.patch',
    '0007-accept-two-plane-rgb-surfaces.patch',
]
# AdrenoMetal bridge patches 0009-0015 (MIT; metal2vulkan draw path), applied after the NativeMetal series.
# FUXI_NATIVE_M2V_DRAW=0 leaves them out. 0008/0016 (AliasWrites policy, tile stores) are not carried: nothing selects that policy.
M2V_DRAW_PATCHES = [] if os.environ.get('FUXI_NATIVE_M2V_DRAW') == '0' else sorted((ROOT/'tools/patches').glob('metal2vulkan-adrenometal-*.patch'))


def run(*args, **kwargs):
    return subprocess.run([str(v) for v in args], check=True, **kwargs)


def git(source, *args, env=None):
    return subprocess.check_output(['git', '-C', str(source), *map(str, args)], text=True, env=env)


def checkout(source, url, pin, sparse=False):
    if not (source/'.git').exists():
        if source.exists() and any(source.iterdir()):
            raise SystemExit(f'Nonempty source destination: {source}')
        run('git', 'init', '-q', source)
        run('git', '-C', source, 'remote', 'add', 'origin', url)
        if sparse:
            run('git', '-C', source, 'config', 'remote.origin.promisor', 'true')
            run('git', '-C', source, 'config', 'remote.origin.partialclonefilter', 'blob:none')
            run('git', '-C', source, 'sparse-checkout', 'init', '--no-cone')
            run('git', '-C', source, 'sparse-checkout', 'set', '--no-cone', '/*',
                '!**/fixtures/', '!**/golden/', '!**/corpus/', '!**/*.air', '!**/*.metallib',
                '!**/*.mtlb', '!**/*.spv', '!**/*.bin')
        run('git', '-C', source, '-c', 'protocol.version=2', 'fetch',
            *(['--filter=blob:none'] if sparse else []), '--no-tags', '--depth', '1', 'origin', pin)
        run('git', '-C', source, 'checkout', '-q', '--detach', 'FETCH_HEAD')
    if git(source, 'rev-parse', 'HEAD').strip() != pin:
        raise SystemExit(f'Source revision mismatch: {source.name}')


def apply_stack(source, patches):
    """Accept only exact clean/patched prefixes, including overlapping patches."""
    if git(source, 'diff', '--cached', '--exit-code') or git(source, 'ls-files', '--others', '--exclude-standard'):
        raise SystemExit(f'Staged/untracked source changes: {source.name}')
    with tempfile.TemporaryDirectory(prefix='fuxi-native-index-') as tmp:
        env = dict(os.environ, GIT_INDEX_FILE=str(Path(tmp)/'index'))
        git(source, 'read-tree', 'HEAD', env=env)
        states = ['']
        for patch in patches:
            git(source, 'apply', '--cached', '--check', patch, env=env)
            git(source, 'apply', '--cached', patch, env=env)
            states.append(git(source, 'diff', '--cached', '--no-ext-diff', '--binary', 'HEAD', env=env))
    actual = git(source, 'diff', '--no-ext-diff', '--binary', 'HEAD')
    if actual not in states:
        raise SystemExit(f'Unexpected edits beyond pinned patch stack: {source.name}')
    for patch in patches[states.index(actual):]:
        git(source, 'apply', '--check', patch)
        git(source, 'apply', patch)
    if git(source, 'diff', '--no-ext-diff', '--binary', 'HEAD') != states[-1]:
        raise SystemExit('Patched source mismatch')


def prepare(native, m2v, build):
    checkout(native, 'https://github.com/UniXNU/NativeMetal.git', PINS['NATIVEMETAL_SHA'])
    checkout(m2v, 'https://github.com/steelbrain/metal2vulkan.git', PINS['M2V_SHA'], sparse=True)
    apply_stack(native, NATIVE_PATCHES)
    apply_stack(m2v, [native/'bridge/patches'/v for v in BRIDGE_PATCHES] + M2V_DRAW_PATCHES)
    build.mkdir(parents=True, exist_ok=True)
    manifest = build/'Cargo.toml'
    manifest.write_text('\n'.join([
        '[package]', 'name = "native-metal-bridge"', 'version = "0.1.0"',
        'edition = "2024"', 'rust-version = "1.98"', '[workspace]', '[lib]',
        'crate-type = ["cdylib"]', 'path = '+json.dumps(str(native/'bridge/NativeMetalBridge.rs')),
        '[features]', 'host-fixtures = []', '[dependencies]',
        'metal2vulkan = { path = '+json.dumps(str(m2v/'crates/metal2vulkan'))+', features = ["ash"] }',
        'metal2vulkan-runtime = { path = '+json.dumps(str(m2v/'crates/metal2vulkan-runtime'))+' }',
        'libc = "0.2"', 'sha2 = { version = "0.10", features = ["asm"] }',
        '[profile.release]', 'panic = "unwind"', 'debug = 1', '',
    ]))
    return manifest


def corresponding_source(native, m2v, build, out, manifest):
    run('cargo', '+'+PINS['RUST_VERSION'], 'vendor', '--locked', '--manifest-path', manifest,
        build/'corresponding-crates', stdout=subprocess.DEVNULL)
    # No SDKs, binary fixtures, extracted IR, build products or git objects.
    def source_only(info):
        parts = Path(info.name).parts
        if any(v in ('.git', 'target', 'fixtures', 'golden', 'corpus', '__pycache__') for v in parts):
            return None
        if Path(info.name).suffix in ('.air', '.metallib', '.mtlb', '.spv', '.bin'):
            return None
        return info
    notice = build/'NOTICE'
    notice.write_text(f'''UniXNU NativeMetal {PINS['NATIVEMETAL_SHA']}: MIT; see native/LICENSE.
metal2vulkan {PINS['M2V_SHA']}: LGPL-3.0-or-later licence choice.
The separately replaceable /usr/local/lib/libnative_metal_bridge.dylib contains
metal2vulkan; no translator objects are linked into either Objective-C bundle.
Keep callback ABI FUXI_NATIVE_ABI=1 and the pinned NativeMetal nmb_* C ABI.
Rebuild/sign replacements through the same offline root staging procedure.
Reverse engineering to debug modifications to this library is permitted.
This archive contains patched sources, transitive crate sources, Cargo.lock,
licence texts and Fuxi recipes/patches. No Apple SDK/toolchain/fixture binaries.
To rebuild the replaceable library offline: unpack, run bash REBUILD.sh.
The script relocates the manifest and uses corresponding-crates and Cargo.lock.
Mac Xcode and Rust 1.98.0 required. Recipes and patches are also included.
''')
    rustdocs = Path(subprocess.check_output(['rustc', '+'+PINS['RUST_VERSION'], '--print', 'sysroot'], text=True).strip())/'share/doc/rust'
    with tarfile.open(out/'nativemetal-source.tar.gz', 'w:gz') as archive:
        for source, name in [(native, 'native'), (m2v, 'metal2vulkan'),
                             (build/'corresponding-crates', 'corresponding-crates'),
                             (manifest, 'Cargo.toml'), (build/'Cargo.lock', 'Cargo.lock'), (notice, 'NOTICE'), (ROOT/'tools/nativemetal-rebuild.sh', 'REBUILD.sh')]:
            archive.add(source, arcname=name, filter=source_only)
        for source in [ROOT/'tools/build-fuxi-nativemetal.sh', ROOT/'tools/build-fuxi-nativemetal.py',
                       ROOT/'tools/ci/pins.env', ROOT/'tools/fuxi-metal', *NATIVE_PATCHES, *M2V_DRAW_PATCHES]:
            archive.add(source, arcname='recipes/'+str(source.relative_to(ROOT)), filter=source_only)
        for name in ['licenses', 'COPYRIGHT-library.html', 'COPYRIGHT.html']:
            archive.add(rustdocs/name, arcname='rust-notices/'+name, filter=source_only)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--native', type=Path, default=Path('deps/nativemetal'))
    ap.add_argument('--metal2vulkan', type=Path, default=Path('deps/nativemetal-metal2vulkan'))
    ap.add_argument('--build', type=Path, default=Path('deps/nativemetal-build'))
    ap.add_argument('--out', type=Path, default=Path('tools/out/nativemetal'))
    ap.add_argument('--prepare-only', action='store_true')
    ap.add_argument('--fetch-fallback-only',type=Path,help='prepare/verify a clean sparse fallback checkout and stop')
    ap.add_argument('--headers', type=Path)
    ap.add_argument('--target', choices=['ios', 'host'], default='ios')
    args = ap.parse_args()
    if args.fetch_fallback_only:
        fallback=args.fetch_fallback_only.resolve()
        checkout(fallback,'https://github.com/steelbrain/metal2vulkan.git',PINS['M2V_SHA'],sparse=True)
        apply_stack(fallback,[])
        return
    native, m2v, build, out = [p.resolve() for p in (args.native, args.metal2vulkan, args.build, args.out)]
    if len({native, m2v, build, out}) != 4:
        raise SystemExit('Source/build/output directories must be distinct')
    if any(target.is_relative_to(source) for source in (native,m2v) for target in (build,out)):
        raise SystemExit('Build/output directories must be outside both source checkouts')
    manifest = prepare(native, m2v, build)
    if args.prepare_only:
        print('PASS: exact pinned patch stacks and cdylib manifest (no fixtures)')
        return
    if platform.system() != 'Darwin' or not args.headers or not (args.headers/'vulkan/vulkan.h').is_file():
        raise SystemExit('Build requires Mac Xcode and --headers EXTERNAL_VULKAN_INCLUDE')
    out.mkdir(parents=True, exist_ok=True)
    host_arch = 'aarch64' if platform.machine() == 'arm64' else 'x86_64'
    target = 'aarch64-apple-ios' if args.target == 'ios' else host_arch+'-apple-darwin'
    sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos' if args.target == 'ios' else 'macosx', '--show-sdk-path'], text=True).strip()
    cc = subprocess.check_output(['xcrun', '--find', 'clang'], text=True).strip()
    install_name = '/usr/local/lib/libnative_metal_bridge.dylib' if args.target == 'ios' else str(out/'libnative_metal_bridge.dylib')
    env = dict(os.environ, SDKROOT=sdk, IPHONEOS_DEPLOYMENT_TARGET='16.0', MACOSX_DEPLOYMENT_TARGET='14.0')
    env['CARGO_TARGET_'+target.upper().replace('-', '_')+'_LINKER'] = cc
    env['CARGO_TARGET_'+target.upper().replace('-', '_')+'_RUSTFLAGS'] = '-C link-arg=-Wl,-install_name,'+install_name
    if args.target == 'ios':
        env['CARGO_TARGET_AARCH64_APPLE_IOS_RUSTFLAGS'] += ' -C link-arg=-miphoneos-version-min=16.0'
    run('rustup', 'toolchain', 'install', PINS['RUST_VERSION'], '--profile', 'minimal')
    run('rustup', '+'+PINS['RUST_VERSION'], 'target', 'add', target)
    run('cargo', '+'+PINS['RUST_VERSION'], 'build', '--release', '--manifest-path', manifest, '--target', target, env=env)
    bridge = out/'libnative_metal_bridge.dylib'
    shutil.copyfile(build/'target'/target/'release'/bridge.name, bridge)
    run('codesign', '-f', '-s', '-', bridge)
    run('codesign', '--verify', '--strict', bridge)
    bundle = out/'FuxiNativeMetal.bundle'
    if bundle.exists():
        raise SystemExit('Move aside existing NativeMetal bundle before rebuilding')
    bundle.mkdir()
    import plistlib
    (bundle/'Info.plist').write_bytes(plistlib.dumps(dict(
        CFBundleIdentifier='com.fuxi.NativeMetal', CFBundleExecutable='FuxiNativeMetal',
        CFBundlePackageType='BNDL', CFBundleVersion='1', MinimumOSVersion='16.0')))
    shutil.copyfile(native/'LICENSE', bundle/'LICENSE.NativeMetal')
    flags = ['-target', 'arm64-apple-ios16.0'] if args.target == 'ios' else ['-arch', 'arm64' if host_arch == 'aarch64' else 'x86_64', '-mmacosx-version-min=14.0']
    run('xcrun', 'clang++', *flags, '-isysroot', sdk, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
        '-Werror=unguarded-availability-new', '-Wno-deprecated-declarations', '-fstack-protector-strong',
        '-fobjc-arc', '-fblocks', '-dynamiclib', '-I'+str(args.headers.resolve()), '-I'+str(ROOT/'tools/fuxi-metal'),
        native/'NativeAdrenoMetalPlugin.mm', native/'NativeBridgeMetalFrontend.mm', bridge,
        '-Wl,-install_name,/System/Library/Extensions/FuxiNativeMetal.bundle/FuxiNativeMetal',
        # The bridge lives in /usr/local/lib, which a shared-cache-eligible image may not link.
        '-Wl,-not_for_dyld_shared_cache',
        '-framework', 'Foundation', '-framework', 'Metal', '-framework', 'IOSurface', '-o', bundle/'FuxiNativeMetal')
    run('codesign', '-f', '-s', '-', bundle)
    run('codesign', '--verify', '--strict', bundle)
    corresponding_source(native, m2v, build, out, manifest)
    if args.target == 'ios':
        run('python3', ROOT/'tools/audit-fuxi-nativemetal.py', bundle/'FuxiNativeMetal', bridge)
    print('PASS: NativeMetal frontend, replaceable Rust cdylib and corresponding source')


if __name__ == '__main__':
    main()
