#!/usr/bin/env python3
"""Inject LC_LOAD_WEAK_DYLIB into lsd without moving a single byte.

The old tools inserted the load command at the end of the command area, which
shifted __text, LC_MAIN and LC_DYLD_CHAINED_FIXUPS and produced dyld's
"unknown imports format".  This tool writes the command into the existing
padding between the load commands and __TEXT,__text, then rebuilds the code
signature in place.

It is intentionally strict: it refuses to write unless the input is an arm64
Mach-O with the expected layout and the output passes structural and hash
checks.
"""
import argparse
import hashlib
import struct
import sys
from pathlib import Path

MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64 = 0x19
LC_CODE_SIGNATURE = 0x1D
LC_LOAD_WEAK_DYLIB = 0x80000018
LC_MAIN = 0x80000028
LC_DYLD_CHAINED_FIXUPS = 0x80000034
CSMAGIC_EMBEDDED_SIGNATURE = 0xFADE0CC0
CSMAGIC_CODEDIRECTORY = 0xFADE0C02


def fail(msg):
    raise SystemExit("lsd-inject: " + msg)


def u32(b, off):
    return struct.unpack_from("<I", b, off)[0]


def be32(b, off):
    return struct.unpack_from(">I", b, off)[0]


def parse_header(b):
    if len(b) < 32:
        fail("file is too small for a Mach-O header")
    magic, cpu, sub, ftype, ncmds, sizeofcmds, flags, res = struct.unpack_from("<8I", b, 0)
    if magic != MH_MAGIC_64:
        fail("not a 64-bit little-endian Mach-O")
    if cpu != 0x0100000C:
        fail("not arm64 (cputype %#x)" % cpu)
    if 32 + sizeofcmds > len(b):
        fail("load commands extend past EOF")
    lcs = []
    off = 32
    for i in range(ncmds):
        if off + 8 > 32 + sizeofcmds:
            fail("load command %d is truncated" % i)
        cmd, cmdsize = struct.unpack_from("<II", b, off)
        if cmdsize < 8 or off + cmdsize > 32 + sizeofcmds:
            fail("load command %d has invalid cmdsize %#x" % (i, cmdsize))
        lcs.append((off, cmd, cmdsize))
        off += cmdsize
    if off != 32 + sizeofcmds:
        fail("load command sizes do not add up to sizeofcmds")
    return ncmds, sizeofcmds, lcs


def parse_sections(b, lcs):
    sections = []
    for off, cmd, cmdsize in lcs:
        if cmd != LC_SEGMENT_64:
            continue
        if cmdsize < 72:
            fail("LC_SEGMENT_64 too small")
        seg = b[off + 8:off + 24].split(b"\0", 1)[0].decode("ascii", "replace")
        nsects = u32(b, off + 64)
        if 72 + nsects * 80 > cmdsize:
            fail("segment %s has too many sections" % seg)
        so = off + 72
        for _ in range(nsects):
            sect = b[so:so + 16].split(b"\0", 1)[0].decode("ascii", "replace")
            sseg = b[so + 16:so + 32].split(b"\0", 1)[0].decode("ascii", "replace")
            addr, size = struct.unpack_from("<QQ", b, so + 32)
            fileoff, align, reloff, nreloc, flags = struct.unpack_from("<IIIII", b, so + 48)
            sections.append((sseg, sect, addr, size, fileoff, flags))
            so += 80
    return sections


def find_lc(lcs, cmd):
    return [(off, size) for off, c, size in lcs if c == cmd]


def parse_superblob(b, off, size):
    if off + 12 > len(b) or size < 12 or off + size > len(b):
        fail("code signature is outside the file")
    magic, length, count = struct.unpack_from(">III", b, off)
    if magic != CSMAGIC_EMBEDDED_SIGNATURE:
        fail("code signature is not an embedded SuperBlob")
    if length > size or 12 + 8 * count > length:
        fail("SuperBlob length/count is invalid")
    blobs = []
    for i in range(count):
        typ, boff = struct.unpack_from(">II", b, off + 12 + 8 * i)
        if boff + 8 > size:
            fail("SuperBlob entry %d is outside the blob" % i)
        bm, bl = struct.unpack_from(">II", b, off + boff)
        if bl < 8 or boff + bl > size:
            fail("SuperBlob entry %d has invalid length" % i)
        blobs.append((typ, b[off + boff:off + boff + bl]))
    return length, blobs


def parse_cd(cd):
    if len(cd) < 44:
        fail("CodeDirectory is too small")
    magic, length = struct.unpack_from(">II", cd, 0)
    if magic != CSMAGIC_CODEDIRECTORY or length != len(cd):
        fail("CodeDirectory magic/length is invalid")
    version, flags, hash_off, ident_off, nspecial, ncode, limit = struct.unpack_from(">7I", cd, 8)
    hash_size, hash_type, platform, page_shift = struct.unpack_from(">4B", cd, 36)
    if hash_size != 32 or hash_type != 2:
        fail("only SHA-256 CodeDirectories are supported")
    if ident_off >= len(cd):
        fail("CodeDirectory ident offset is out of range")
    end = cd.find(b"\0", ident_off)
    if end < 0:
        fail("CodeDirectory identifier is not NUL terminated")
    ident = cd[ident_off:end + 1]
    if hash_off < 32 * nspecial or hash_off + 32 * ncode > len(cd):
        fail("CodeDirectory hash slots are out of range")
    prefix = cd[:hash_off]
    special = cd[hash_off - 32 * nspecial:hash_off]
    code = cd[hash_off:hash_off + 32 * ncode]
    return {
        "version": version,
        "flags": flags,
        "hash_off": hash_off,
        "ident": ident,
        "nspecial": nspecial,
        "ncode": ncode,
        "limit": limit,
        "page": 1 << page_shift,
        "prefix": prefix,
        "special": special,
        "code": code,
    }


def code_hashes(b, limit, page, ncode):
    out = []
    for k in range(ncode):
        start = k * page
        end = min(start + page, limit)
        out.append(hashlib.sha256(b[start:end]).digest())
    return b"".join(out)


def build_cd(cd_info, b, limit, page, ncode):
    prefix = bytearray(cd_info["prefix"])
    struct.pack_into(">I", prefix, 28, ncode)
    struct.pack_into(">I", prefix, 32, limit)
    out = bytes(prefix) + code_hashes(b, limit, page, ncode)
    out = bytearray(out)
    struct.pack_into(">I", out, 4, len(out))
    return bytes(out)


def build_superblob(blobs):
    header = 12 + 8 * len(blobs)
    cur = header
    entries = []
    payload = bytearray()
    for typ, blob in blobs:
        entries.append((typ, cur))
        payload += blob
        cur += len(blob)
    out = bytearray(struct.pack(">III", CSMAGIC_EMBEDDED_SIGNATURE, cur, len(blobs)))
    for typ, off in entries:
        out += struct.pack(">II", typ, off)
    out += payload
    if len(out) != cur:
        fail("internal SuperBlob size mismatch")
    return bytes(out)


def verify(b, path, text_off, text_size, entry_off, entry_bytes,
           fix_off, fix_size, fix_bytes, sig_off, sig_size, expect_path):
    ncmds, sizeofcmds, lcs = parse_header(b)
    if 32 + sizeofcmds > text_off:
        fail("load commands overlap __TEXT,__text")
    if b[entry_off:entry_off + 4] != entry_bytes:
        fail("LC_MAIN entry bytes changed")
    if b[fix_off:fix_off + fix_size] != fix_bytes:
        fail("LC_DYLD_CHAINED_FIXUPS bytes changed")
    if sig_off + sig_size != len(b):
        fail("code signature does not end at EOF")

    weak = find_lc(lcs, LC_LOAD_WEAK_DYLIB)
    if len(weak) != 1:
        fail("expected exactly one LC_LOAD_WEAK_DYLIB, found %d" % len(weak))
    off, size = weak[0]
    cmd, cmdsize, nameoff = struct.unpack_from("<III", b, off)
    if cmd != LC_LOAD_WEAK_DYLIB or cmdsize != 72 or nameoff != 24:
        fail("weak dylib command fields are wrong")
    name = b[off + nameoff:off + cmdsize].split(b"\0", 1)[0]
    if name.decode("utf-8", "replace") != expect_path:
        fail("weak dylib path is %r, expected %r" % (name, expect_path))

    length, blobs = parse_superblob(b, sig_off, sig_size)
    cds = [blob for typ, blob in blobs if be32(blob, 0) == CSMAGIC_CODEDIRECTORY]
    if len(cds) != 1:
        fail("expected exactly one CodeDirectory, found %d" % len(cds))
    cd_info = parse_cd(cds[0])
    if cd_info["limit"] != sig_off:
        fail("CodeDirectory limit is %#x, expected %#x" % (cd_info["limit"], sig_off))
    if cd_info["ncode"] != (sig_off + cd_info["page"] - 1) // cd_info["page"]:
        fail("CodeDirectory nCode is inconsistent with its limit")
    hashes = cd_info["code"]
    if len(hashes) != 32 * cd_info["ncode"]:
        fail("CodeDirectory hash area is malformed")
    for k in range(cd_info["ncode"]):
        start = k * cd_info["page"]
        end = min(start + cd_info["page"], sig_off)
        if hashlib.sha256(b[start:end]).digest() != hashes[32 * k:32 * (k + 1)]:
            fail("CodeDirectory hash %d does not match" % k)
    print("verify: %s" % path)
    print("  size %d, ncmds %d, sizeofcmds %#x" % (len(b), ncmds, sizeofcmds))
    print("  weak dylib at %#x: %s" % (off, name.decode("utf-8", "replace")))
    print("  __text %d bytes at %#x, entry bytes %s" % (text_size, text_off, entry_bytes.hex()))
    print("  chained fixups %d bytes at %#x" % (fix_size, fix_off))
    print("  signature %d bytes at %#x, SuperBlob %d, CD %d bytes, %d/%d hashes" %
          (sig_size, sig_off, length, len(cds[0]), cd_info["ncode"], cd_info["ncode"]))


def main():
    ap = argparse.ArgumentParser(description="Inject a weak dylib into lsd in place")
    ap.add_argument("--input", required=True, type=Path, help="pristine arm64 lsd")
    ap.add_argument("--output", required=True, type=Path, help="patched lsd")
    ap.add_argument("--dylib", default="/usr/local/lib/fuxi/libfuxi-lsd-bypass.dylib",
                    help="install path for the weak dylib")
    ap.add_argument("--verify-only", action="store_true", help="only verify an already patched file")
    args = ap.parse_args()

    b = bytearray(args.input.read_bytes())
    ncmds, sizeofcmds, lcs = parse_header(b)
    if not args.verify_only and find_lc(lcs, LC_LOAD_WEAK_DYLIB):
        fail("input already has LC_LOAD_WEAK_DYLIB; refusing to add a second one")

    sections = parse_sections(b, lcs)
    texts = [s for s in sections if s[0] == "__TEXT" and s[1] == "__text"]
    if len(texts) != 1:
        fail("expected exactly one __TEXT,__text section")
    _, _, text_addr, text_size, text_off, text_flags = texts[0]
    if text_off < 32 + sizeofcmds:
        fail("__TEXT,__text starts inside the load commands")

    mains = find_lc(lcs, LC_MAIN)
    if len(mains) != 1:
        fail("expected exactly one LC_MAIN")
    entry_off = struct.unpack_from("<Q", b, mains[0][0] + 8)[0]
    if entry_off + 4 > len(b):
        fail("LC_MAIN entry offset is out of range")
    entry_bytes = bytes(b[entry_off:entry_off + 4])
    if entry_bytes == b"\0\0\0\0":
        fail("LC_MAIN points at zero bytes; input is already damaged")

    fixes = find_lc(lcs, LC_DYLD_CHAINED_FIXUPS)
    if len(fixes) != 1:
        fail("expected exactly one LC_DYLD_CHAINED_FIXUPS")
    fix_off, fix_size = struct.unpack_from("<II", b, fixes[0][0] + 8)
    if fix_off + fix_size > len(b):
        fail("chained fixups are outside the file")
    fix_bytes = bytes(b[fix_off:fix_off + fix_size])
    if fix_bytes[:4] != b"\0\0\0\0":
        fail("unexpected chained-fixups version")

    sigs = find_lc(lcs, LC_CODE_SIGNATURE)
    if len(sigs) != 1:
        fail("expected exactly one LC_CODE_SIGNATURE")
    sig_off, sig_size = struct.unpack_from("<II", b, sigs[0][0] + 8)
    if sig_off + sig_size != len(b):
        fail("code signature does not end at EOF")

    path = args.dylib.encode("utf-8")
    if b"\0" in path:
        fail("dylib path contains NUL")
    padded = path + b"\0"
    padded += b"\0" * ((8 - len(padded) % 8) % 8)
    cmdsize = 24 + len(padded)
    if cmdsize < 24 or cmdsize % 8:
        fail("invalid weak dylib cmdsize")

    slack = text_off - (32 + sizeofcmds)
    if cmdsize > slack:
        fail("need %d bytes of slack, only %d available" % (cmdsize, slack))
    lc = struct.pack("<IIIIII", LC_LOAD_WEAK_DYLIB, cmdsize, 24, 0, 0x10000, 0x10000) + padded
    if len(lc) != cmdsize:
        fail("internal weak dylib size mismatch")

    if args.verify_only:
        verify(b, str(args.input), text_off, text_size, entry_off, entry_bytes,
               fix_off, fix_size, fix_bytes, sig_off, sig_size, args.dylib)
        return

    # Overwrite the existing padding.  Do not insert: every offset after this
    # point (including LC_MAIN and the chained fixups) must stay valid.
    lc_off = 32 + sizeofcmds
    b[lc_off:lc_off + cmdsize] = lc
    struct.pack_into("<I", b, 16, ncmds + 1)
    struct.pack_into("<I", b, 20, sizeofcmds + cmdsize)

    # Rebuild the CodeDirectory in place.  The signature allocation and the
    # file size stay unchanged.
    length, blobs = parse_superblob(b, sig_off, sig_size)
    cd_blobs = [blob for typ, blob in blobs if be32(blob, 0) == CSMAGIC_CODEDIRECTORY]
    if len(cd_blobs) != 1:
        fail("expected exactly one CodeDirectory in the input")
    cd_info = parse_cd(cd_blobs[0])
    limit = sig_off
    page = cd_info["page"]
    ncode = (limit + page - 1) // page
    new_cd = build_cd(cd_info, b, limit, page, ncode)
    new_blobs = [(typ, new_cd if be32(blob, 0) == CSMAGIC_CODEDIRECTORY else blob)
                 for typ, blob in blobs]
    new_sb = build_superblob(new_blobs)
    if len(new_sb) > sig_size:
        fail("new signature is %d bytes, allocation is %d" % (len(new_sb), sig_size))
    b[sig_off:sig_off + sig_size] = new_sb + b"\0" * (sig_size - len(new_sb))

    verify(b, str(args.output), text_off, text_size, entry_off, entry_bytes,
           fix_off, fix_size, fix_bytes, sig_off, sig_size, args.dylib)

    args.output.write_bytes(bytes(b))
    print("wrote %s (%d bytes)" % (args.output, len(b)))


if __name__ == "__main__":
    main()
