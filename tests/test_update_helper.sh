#!/bin/sh
set -eu

helper=${1:?usage: test_update_helper.sh <helper>}
temp_base=${TMPDIR:-/tmp}
root=$(mktemp -d "${temp_base%/}/jvman-update-helper-test.XXXXXX")
trap 'rm -rf "$root"' EXIT HUP INT TERM

target=$root/test-update-target
replacement=$root/test-update-replacement
cp "$helper" "$target"
cp "$helper" "$replacement"
if [ "$(uname -s)" = Darwin ]; then
    codesign --remove-signature "$replacement" >/dev/null 2>&1 || true
    printf 'Z' >> "$replacement"
    codesign --force --sign - --timestamp=none "$replacement"
    codesign --verify --strict "$replacement"
else
    printf 'Z' >> "$replacement"
fi
chmod 0700 "$target" "$replacement"

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

expected=$(sha256_file "$replacement")
original=$(sha256_file "$target")
test "$expected" != "$original"
"$target" --reject-stage "$replacement"
"$target" --reject-current "$replacement"
test "$(sha256_file "$target")" = "$original"

chmod 0777 "$target"
if "$target" "$replacement"; then
    echo "world-writable update target was accepted" >&2
    exit 1
fi
chmod 0700 "$target"
test "$(sha256_file "$target")" = "$original"

chmod 0777 "$root"
if "$target" "$replacement"; then
    echo "unsafe update directory was accepted" >&2
    exit 1
fi
chmod 0700 "$root"
test "$(sha256_file "$target")" = "$original"

"$target" "$replacement"
actual=$(sha256_file "$target")
test "$actual" = "$expected"
"$target" --probe

leftover=$(find "$root" -name '*.jvman-*.tmp' -print)
test -z "$leftover"
