#!/usr/bin/env python3
"""Build the single-CPU, RAM-root Fuxi ADT after validating a live FDT.

Uses the same iPhone X userland identity and generic providers as ios-sweet.
Hardware values come from measured Fuxi probes. UFS is absent by default; an
explicit option adds the node used by the read-only storage diagnostic.

Opt-in /product children (docs-lanes/sys-port.md), copied at build time from
the real D22AP tree (an Apple artifact, never in git; --d22ap-adt):
  --product-haptics  /product/haptics (closed-loop, supports-3rd-party-haptics):
                     MobileGestalt DeviceSupportsClosedLoopHaptics, so iOS plays
                     haptics as waveforms on an "Actuator" HAL device (FuxiAudio)
                     instead of looking for an IOKit vibration motor
  --product-camera   an empty /product/camera node, no flash (there is no torch);
                     MobileGestalt then reports a camera node; for A/B only
Both work on a fresh build and on --template.
"""
import argparse
import importlib.util
import json
import struct
from pathlib import Path

import adt
import mkadt

PHYS_BASE, BANK_SIZE = 0x8c0000000, 0x140000000
GPU_SIZE = 0x02000000
GPU_BASE = PHYS_BASE + BANK_SIZE - GPU_SIZE
MEM_SIZE = BANK_SIZE - GPU_SIZE
LEGACY_SIZE = 0x40000000
FB, FB_SIZE = 0xb8000000, 0x02b00000
GICD, GICR, GICR_SIZE = 0x17100000, 0x17180000, 0x200000
# Board identity: the model keyword the live FDT must contain, and the board
# entry appended to /compatible. Ports override these plus the geometry above
# from their own measured live FDT (see --board and the --*-base/--*-size args).
MODEL_KEY = b'fuxi'
BOARD_COMPATIBLE = b'xiaomi,fuxi\0'


def setprop(node, name, value):
    node.props = [(k, f, value if k == name else v) for k, f, v in node.props]


def validate_live(path, ufs_readonly=False):
    # Validate the entire used bank, including GPU scratch, against reservations.
    spec = importlib.util.spec_from_file_location('fuxi_fdt', Path(__file__).with_name('fuxi-inspect-dtb.py'))
    fdt = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fdt)
    nodes, reservations, _ = fdt.parse(path.read_bytes())
    if MODEL_KEY not in nodes['/'].get('model', b'').lower():
        raise ValueError(f'live FDT does not identify a {MODEL_KEY.decode()} board')
    memory = nodes.get('/memory', {}).get('reg', b'')
    if len(memory) % 16:
        raise ValueError('unexpected /memory cell layout')
    ranges = list(struct.iter_unpack('>QQ', memory))
    if not any(base <= PHYS_BASE and PHYS_BASE + BANK_SIZE <= base + size for base, size in ranges):
        raise ValueError('requested XNU RAM is outside advertised memory')
    inventory = fdt.describe(nodes, reservations)['nodes']
    for name, props in inventory.items():
        if name.startswith('/reserved-memory/'):
            for region in props.get('decoded-reg', []):
                base, size = int(region['address'], 16), int(region['size'], 16)
                if size and base < PHYS_BASE + BANK_SIZE and PHYS_BASE < base + size:
                    raise ValueError(f'XNU RAM overlaps {name}')
    for region in reservations:
        base, size = int(region['address'], 16), int(region['size'], 16)
        if size and base < PHYS_BASE + BANK_SIZE and PHYS_BASE < base + size:
            raise ValueError('XNU RAM overlaps FDT reserve map')
    gic = nodes.get('/soc/interrupt-controller@17100000', {})
    if gic.get('reg') != struct.pack('>4I', GICD, 0x10000, GICR, GICR_SIZE):
        raise ValueError('GIC layout differs from the validated Fuxi map')
    if ufs_readonly:
        ufs=nodes.get('/soc/ufshc@1d84000',{})
        if ufs.get('reg',b'')[:8] != struct.pack('>II',0x1d84000,0x3000):
            raise ValueError('UFS host layout differs from the measured Fuxi map')
    splash = nodes.get('/reserved-memory/splash_region', {}).get('reg')
    if splash != struct.pack('>QQ', FB, FB_SIZE):
        raise ValueError('splash reservation differs from the measured Fuxi map')


def root_extent_props(path, root_bytes):
    """/chosen props for the pinned-file root: u64 {file, partition, count} blocks."""
    table = json.loads(path.read_text())
    if root_bytes % 4096 or root_bytes > table['size']:
        raise ValueError('root size must be 4 KiB aligned and inside the pinned file')
    words, pos = [], 0
    for logical, physical, length in table['extents']:
        if logical != pos or physical % 4096 or length % 4096:
            raise ValueError('extent table is not contiguous 4 KiB extents')
        words += [logical // 4096, physical // 4096, length // 4096]
        pos += length
    if pos < root_bytes:
        raise ValueError('extents do not cover the root')
    return [('fuxi-root-extents', struct.pack(f'<{len(words)}Q', *words)),
            ('fuxi-root-blocks', struct.pack('<Q', root_bytes // 4096))]


def build(ufs_readonly=False, root_props=(), smp=False, gpu_reserve=False, mem_size=MEM_SIZE):
    mkadt.HIGH_PHYS_BASE, mkadt.HIGH_MEM_SIZE = PHYS_BASE, mem_size
    mkadt.GICD, mkadt.GICR, mkadt.GICR_SIZE = GICD, GICR, GICR_SIZE
    mkadt.FB, mkadt.FB_SIZE = FB, FB_SIZE
    mkadt.VTIMER_INTID = 27
    root = mkadt.build(highmem=True)
    setprop(root, 'compatible', b'D22AP\0iPhone10,3\0' + BOARD_COMPATIBLE + b'qcom,sm8550\0AppleARM\0')
    if ufs_readonly:
        ufs=next(n for n in root.children if n.name=='sweet-ufs')
        setprop(ufs,'name',b'fuxi-ufs\0')
        setprop(ufs,'compatible',b'qcom,sm8550-ufshc\0')
    else:
        root.children = [n for n in root.children if n.name != 'sweet-ufs']
    chosen = next(n for n in root.children if n.name == 'chosen')
    setprop(chosen, 'firmware-version', b'fuxi-shim\0')
    setprop(chosen, 'system-firmware-version', b'fuxi-shim\0')
    setprop(chosen, 'unique-chip-id', struct.pack('<Q', 0x4655584900000001))
    chosen.props += [(k, 0, v) for k, v in root_props]
    if gpu_reserve:
        chosen.props.append(('fuxi-gpu-memory', 0, struct.pack('<2Q', GPU_BASE, GPU_SIZE)))
    cpus = next(n for n in root.children if n.name == 'cpus')
    # MPIDR Aff1 = core. cpu@100 keeps running the loader's USB monitor, so XNU
    # gets core 0 and, with smp, cores 2-7 (logical IDs 0-6 in this order).
    cpus.children = [cpus.children[c] for c in ((0, 2, 3, 4, 5, 6, 7) if smp else (0,))]
    for cpu in cpus.children:
        setprop(cpu, 'compatible', b'qcom,sm8550-cpu\0ARM,v8\0')
        setprop(cpu, 'clock-frequency', struct.pack('<I', 2016000000))
    return root


def d22ap_product_child(name, path):
    """Properties of /product/NAME in the D22AP tree, minus node bookkeeping and
    the syscfg placeholders iBoot fills in (flag bit set)."""
    source, _ = adt.parse(Path(path).read_bytes())
    product = next((c for c in source.children if c.name == 'product'), None)
    child = product and next((c for c in product.children if c.name == name), None)
    if child is None:
        raise ValueError(f'{path}: no /product/{name} node')
    return [(k, v) for k, f, v in child.props if not f and k not in ('name', 'AAPL,phandle')]


def max_phandle(node):
    value = node.prop('AAPL,phandle')
    own = struct.unpack('<I', value)[0] if value and len(value) == 4 else 0
    return max([own] + [max_phandle(c) for c in node.children])


def add_product_children(root, haptics=False, camera=False, d22ap=mkadt.D22AP_ADT):
    """Add /product/haptics (from D22AP) and/or an empty /product/camera.
    Replaces a child of the same name, so a template can be rebuilt."""
    wanted = ([('haptics', d22ap_product_child('haptics', d22ap))] if haptics else []) + \
        ([('camera', [])] if camera else [])
    if not wanted:
        return root
    product = next((c for c in root.children if c.name == 'product'), None)
    if product is None:
        raise ValueError('ADT has no /product node (D22AP tree missing at build time?)')
    phandle = max_phandle(root)
    for name, props in wanted:
        product.children = [c for c in product.children if c.name != name]
        phandle += 1
        product.children.append(mkadt.node(name, [('AAPL,phandle', struct.pack('<I', phandle))] + props))
    return root


def reserve_gpu(root, mem_size=MEM_SIZE):
    """Preserve an integration ADT's identity/root extents while excluding scratch."""
    chosen = next(n for n in root.children if n.name == 'chosen')
    if (struct.unpack('<Q', chosen.prop('dram-base'))[0] != PHYS_BASE or
            struct.unpack('<Q', chosen.prop('dram-size'))[0] not in (MEM_SIZE, LEGACY_SIZE, 0x30000000)):
        raise ValueError('template ADT has unexpected RAM geometry')
    setprop(chosen, 'dram-size', struct.pack('<Q', mem_size))
    chosen.props = [(k, f, v) for k, f, v in chosen.props if k != 'fuxi-gpu-memory']
    chosen.props.append(('fuxi-gpu-memory', 0, struct.pack('<2Q', GPU_BASE, GPU_SIZE)))
    return root


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('out', type=Path)
    ap.add_argument('--live-fdt', type=Path, required=True)
    ap.add_argument('--ufs-readonly',action='store_true',help='include UFS node for the opt-in, read-only kernel probe')
    ap.add_argument('--root-extents',type=Path,help='pinned-file extent JSON (bytes, userdata-relative); needs --root-bytes')
    ap.add_argument('--root-bytes',type=int,help='root image size in bytes')
    ap.add_argument('--template', type=Path, help='preserve an existing integration ADT; requires --gpu-reserve')
    ap.add_argument('--gpu-reserve', action='store_true', help='declare the top-of-bank 32 MiB GPU scratch (always excluded from XNU)')
    ap.add_argument('--smp',action='store_true',help='add cores 2-7 (cpu@100 stays with the USB monitor)')
    ap.add_argument('--ram-layout', choices=('full', '1g'), default='full', help='full = 5088 MiB; 1g = original 1 GiB XNU window')
    ap.add_argument('--product-haptics', action='store_true', help='add /product/haptics copied from the D22AP tree')
    ap.add_argument('--product-camera', action='store_true', help='add an empty /product/camera node (no flash)')
    ap.add_argument('--d22ap-adt', type=Path, default=Path(mkadt.D22AP_ADT), help='real D22AP device tree (not in git)')
    ap.add_argument('--board', choices=('fuxi', 'vermeer'), default='fuxi',
                    help='vermeer = Redmi K70 / POCO F6 Pro (SM8550); requires the measured geometry overrides')
    ap.add_argument('--phys-base', type=lambda s: int(s, 0), help='XNU RAM base (hex) from the measured live FDT')
    ap.add_argument('--bank-size', type=lambda s: int(s, 0), help='contiguous RAM bank size (hex)')
    ap.add_argument('--fb-base', type=lambda s: int(s, 0), help='splash reservation base (hex)')
    ap.add_argument('--fb-size', type=lambda s: int(s, 0), help='splash reservation size (hex)')
    ap.add_argument('--model-key', help='keyword the live FDT /model must contain (default: the board name)')
    args = ap.parse_args()

    global PHYS_BASE, BANK_SIZE, GPU_BASE, MEM_SIZE, FB, FB_SIZE, MODEL_KEY, BOARD_COMPATIBLE
    if args.board != 'fuxi':
        missing = [n for n, v in (('phys-base', args.phys_base), ('bank-size', args.bank_size),
                                  ('fb-base', args.fb_base), ('fb-size', args.fb_size)) if v is None]
        if missing:
            ap.error(f'--board {args.board} needs measured values for: {", ".join(missing)}')
        PHYS_BASE, BANK_SIZE = args.phys_base, args.bank_size
        FB, FB_SIZE = args.fb_base, args.fb_size
        MODEL_KEY = (args.model_key or args.board).encode()
        BOARD_COMPATIBLE = f'xiaomi,{args.board}\0'.encode()
    elif any(v is not None for v in (args.phys_base, args.bank_size, args.fb_base, args.fb_size)):
        ap.error('explicit geometry overrides are only valid with a non-default --board')
    GPU_BASE = PHYS_BASE + BANK_SIZE - GPU_SIZE
    MEM_SIZE = BANK_SIZE - GPU_SIZE
    mem_size = MEM_SIZE if args.ram_layout == 'full' else LEGACY_SIZE
    try:
        root_props = ()
        if args.root_extents:
            args.ufs_readonly = True  # the extent root needs the UFS node
            root_props = root_extent_props(args.root_extents, args.root_bytes or 0)
        if args.template:
            if not args.gpu_reserve or args.root_extents:
                raise ValueError('--template requires --gpu-reserve and preserves existing root extents')
            root, _ = adt.parse(args.template.read_bytes())
            root = reserve_gpu(root, mem_size)
        else:
            root = build(args.ufs_readonly, root_props, args.smp, args.gpu_reserve, mem_size)
        root = add_product_children(root, args.product_haptics, args.product_camera, args.d22ap_adt)
        has_ufs = any(n.name == 'fuxi-ufs' for n in root.children)
        validate_live(args.live_fdt, has_ufs)
        blob = adt.serialize(root)
    except (ValueError, KeyError, struct.error, OSError) as error:
        ap.exit(1, str(error) + '\n')
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(blob)
    cpu_count = len(next(n for n in root.children if n.name == 'cpus').children)
    product = next((n for n in root.children if n.name == 'product'), None)
    extra = sorted(c.name for c in product.children) if product else []
    print(f'{args.out}: {len(blob)} bytes; RAM {PHYS_BASE:#x}+{mem_size:#x}; CPUs {cpu_count}; UFS={"present" if has_ufs else "absent"}'
          + (f'; /product children {",".join(extra)}' if extra else ''))


if __name__ == '__main__':
    main()
