#!/bin/bash
# Native Mac userland build; requires the selected Xcode's iPhoneOS SDK.
set -euo pipefail
cd "$(dirname "$0")/.."
[[ $(uname -s) == Darwin ]] || { echo 'Build on the Mac with Xcode' >&2; exit 1; }
out=${1:-tools/out}
mkdir -p "$out"
sdkroot=$(xcrun --sdk iphoneos --show-sdk-path)
xcrun --sdk iphoneos clang -target arm64-apple-ios16.0 -isysroot "$sdkroot" \
    -std=c11 -O2 -Wall -Wextra -Werror -fstack-protector-strong \
    -Werror=unguarded-availability-new tools/fuxi-thermald/thermald.c -o "$out/thermald"
codesign -f -s - "$out/thermald"
codesign --verify --strict "$out/thermald"
