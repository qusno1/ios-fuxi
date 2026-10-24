#!/system/bin/sh
# vermeer-collect.sh — run as ROOT on the Redmi K70 (vermeer), e.g.
#   adb push vermeer-collect.sh /data/local/tmp/
#   adb shell su -c 'sh /data/local/tmp/vermeer-collect.sh'
#   adb pull /data/local/tmp/vermeer-dump.tgz
# Collects everything the ios-fuxi vermeer port needs from the live device.
# Read-only: nothing is written outside /data/local/tmp.
set -u
OUT=/data/local/tmp/vermeer-dump
rm -rf "$OUT" "$OUT.tgz"
mkdir -p "$OUT"
# /proc/device-tree is a symlink on vermeer: use the real target and copy
# with -L so the dump contains the tree, not a dangling link.
if [ -d /sys/firmware/devicetree/base ]; then
	DT=/sys/firmware/devicetree/base
else
	DT=/proc/device-tree
fi

# 1. The live device tree: memory banks, reserved-memory (splash region),
#    UFS host node, touch controller, PMIC GPIOs, panel DSI node.
# Prefer the raw FDT blob when the kernel exposes it (best --live-fdt input).
if [ -f /sys/firmware/fdt ]; then
	cp /sys/firmware/fdt "$OUT/live.dtb"
fi
if cp -rL "$DT" "$OUT/live-dt" 2>/dev/null; then
	chmod -R u+rw "$OUT/live-dt"
else
	(cd "$DT" && tar cfh "$OUT/live-dt.tar" .)
fi

# 2. Kernel's view of the memory map and reservations.
cat /proc/iomem > "$OUT/iomem.txt" 2>/dev/null
cat /proc/meminfo > "$OUT/meminfo.txt"
dmesg > "$OUT/dmesg.txt" 2>/dev/null

# 3. Panel / framebuffer geometry (BIOS-inherited scanout, resolution, stride).
cat "$DT/model" > "$OUT/model.txt" 2>/dev/null
for f in /sys/class/graphics/fb0/virtual_size /sys/class/graphics/fb0/stride \
         /sys/class/graphics/fb0/modes /sys/class/graphics/fb0/name; do
	[ -f "$f" ] && echo "== $f" && cat "$f" >> "$OUT/fb0.txt"
done

# 4. GPU zap shader (device-specific; the port pins its SHA256).
ls -la /vendor/firmware > "$OUT/vendor-fw-list.txt" 2>/dev/null
for f in /vendor/firmware/a740_zap.mbn /vendor/firmware/sm8550*.mbn \
         /vendor/firmware/gpu*.mbn /vendor/firmware/*zap*; do
	[ -f "$f" ] || continue
	cp "$f" "$OUT/"
	sha256sum "$f" >> "$OUT/zap-sha256.txt"
done

# 5. Touch / buttons nodes, decoded as hex so nothing is lost in the pull.
{
	find -L "$DT" -name '*touch*' -o -name '*fts*' -o -name '*goodix*' \
		-o -name '*novatek*' -o -name '*synaptics*' -o -name '*pon*' \
		-o -name '*gpio-keys*' -o -name '*qup*' -o -name '*geni*'
} | sort > "$OUT/node-list.txt"
: > "$OUT/nodes-hex.txt"
while IFS= read -r n; do
	[ -d "$n" ] || continue
	echo "== $n" >> "$OUT/nodes-hex.txt"
	for p in "$n"/*; do
		[ -f "$p" ] || continue
		printf '  %s: ' "${p##*/}" >> "$OUT/nodes-hex.txt"
		od -An -tx1 -v "$p" | tr -d '\n' >> "$OUT/nodes-hex.txt"
		echo >> "$OUT/nodes-hex.txt"
	done
done < "$OUT/node-list.txt"

# 6. Running kernel release (for matching the Xiaomi open-source tree).
uname -a > "$OUT/uname.txt"
getprop ro.product.device ro.build.fingerprint > "$OUT/device.txt" 2>/dev/null

tar cf "$OUT.tgz" -C /data/local/tmp vermeer-dump
echo "done: $OUT.tgz ($(wc -c < "$OUT.tgz") bytes) — adb pull it"
