#!/bin/sh
set -eu

binary=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
test_root=${TMPDIR:-/tmp}/jvman-test-$$
state_root=$test_root/state
trap 'rm -rf "$test_root"' EXIT INT TERM
export JVMAN_HOME=$state_root
export JVMAN_LANG=en

zh_help=$(JVMAN_LANG=zh-CN "$binary")
printf '%s\n' "$zh_help" | grep -F '轻量级 Java 版本管理器' >/dev/null
printf '%s\n' "$zh_help" | grep -F '使用方法' >/dev/null
printf '%s\n' "$zh_help" | grep -F '  jvman uninstall [<名称>]' >/dev/null
en_help=$(JVMAN_LANG=en "$binary")
printf '%s\n' "$en_help" | grep -F 'lightweight Java version manager' >/dev/null
printf '%s\n' "$en_help" | grep -F 'Usage:' >/dev/null
printf '%s\n' "$en_help" | grep -F '  jvman uninstall [<name>]' >/dev/null
if "$binary" uninstall >/dev/null 2>&1; then
    echo 'non-Windows self-uninstall unexpectedly succeeded' >&2
    exit 1
fi
JVMAN_LANG=zh-CN "$binary" language --list | grep -F '简体中文' >/dev/null
if JVMAN_LANG=en "$binary" language zh-CN >/dev/null 2>&1; then
    echo 'non-Windows language command unexpectedly persisted a setting' >&2
    exit 1
fi
if zh_source_usage=$(JVMAN_LANG=zh-CN "$binary" source --list extra 2>&1); then
    echo 'invalid source usage unexpectedly succeeded' >&2
    exit 1
fi
printf '%s\n' "$zh_source_usage" | grep -F \
    '用法：jvman source [--list|--reset|<名称>|add <名称> <HTTPS模板>|remove <名称>]' \
    >/dev/null
if zh_source_url=$(JVMAN_LANG=zh-CN "$binary" source add invalid-url \
        'http://example.test/{major}' 2>&1); then
    echo 'invalid custom source URL unexpectedly succeeded' >&2
    exit 1
fi
printf '%s\n' "$zh_source_url" | grep -F \
    '自定义下载源 URL 必须使用 HTTPS 并包含 {major}；支持的占位符为 {major}、{os}、{arch} 和 {archive}' \
    >/dev/null

make_jdk() {
    mkdir -p "$1/bin"
    : > "$1/bin/java"
    : > "$1/bin/javac"
    chmod +x "$1/bin/java" "$1/bin/javac"
    {
        printf 'JAVA_VERSION="%s"\n' "${2:-17.0.0-test}"
        if test -n "${3:-}"; then
            printf 'IMPLEMENTOR="%s"\n' "$3"
        fi
    } > "$1/release"
}

make_jre() {
    mkdir -p "$1/bin"
    : > "$1/bin/java"
    chmod +x "$1/bin/java"
    {
        printf 'JAVA_VERSION="%s"\n' "$2"
        printf 'IMPLEMENTOR="Amazon Corretto"\n'
    } > "$1/release"
}

original_home=${HOME-}
original_java_home=${JAVA_HOME-}
original_path=$PATH
discovery_home=$test_root/user
discovered_a=$discovery_home/.jdks/discovery-a
discovered_b=$test_root/fixtures/discovery-b
discovered_c=$test_root/fixtures/discovery-c
discovered_jre=$test_root/fixtures/discovery-jre
discovered_invalid=$test_root/fixtures/discovery-invalid
name_conflict=$test_root/fixtures/name-conflict
discovery_version=91.7.3-jvman-test
discovery_name=temurin-$discovery_version
discovery_name_2=$discovery_name-2
discovery_name_3=$discovery_name-3

make_jdk "$discovered_a" "$discovery_version" 'Eclipse Adoptium'
make_jdk "$discovered_b" "$discovery_version" 'Eclipse Adoptium'
make_jdk "$discovered_c" "$discovery_version" 'Eclipse Adoptium'
make_jre "$discovered_jre" 77.0.1-jvman-test
make_jdk "$discovered_invalid" broken 'Test Vendor'
printf 'JAVA_VERSION="unterminated\n' > "$discovered_invalid/release"
make_jdk "$name_conflict" 92.0.0-jvman-test 'Test Vendor'

export HOME=$discovery_home
export JAVA_HOME=$discovered_a
PATH=$discovered_a/bin:$discovered_b/bin:$discovered_c/bin:$discovered_jre/bin:$discovered_invalid/bin:$original_path
export PATH

preview=$("$binary" discover)
test ! -e "$state_root"
test "$(printf '%s\n' "$preview" | grep -cF "$discovered_a")" -eq 1
test "$(printf '%s\n' "$preview" | grep -cF "$discovered_b")" -eq 1
row_a=$(printf '%s\n' "$preview" | grep -F "$discovered_a")
row_b=$(printf '%s\n' "$preview" | grep -F "$discovered_b")
row_jre=$(printf '%s\n' "$preview" | grep -F "$discovered_jre")
row_invalid=$(printf '%s\n' "$preview" | grep -F "$discovered_invalid")
printf '%s\n' "$row_a" | grep -Eq '^JDK[[:space:]]+.*[[:space:]]new[[:space:]]+'
printf '%s\n' "$row_a" | grep -F 'JAVA_HOME,PATH,user-jdks' >/dev/null
printf '%s\n' "$row_b" | grep -Eq '^JDK[[:space:]]+.*[[:space:]]new[[:space:]]+'
printf '%s\n' "$row_jre" | grep -Eq '^JRE[[:space:]]+.*[[:space:]]jre[[:space:]]+'
printf '%s\n' "$row_invalid" | grep -Eq '^INVALID[[:space:]]+.*[[:space:]]invalid[[:space:]]+'
if "$binary" discover --unknown >/dev/null 2>&1; then
    echo 'discover accepted an unknown option' >&2
    exit 1
fi

# Update argument validation must finish before any network or state access.
if "$binary" update --unknown >/dev/null 2>&1 ||
   "$binary" update --version >/dev/null 2>&1 ||
   "$binary" update --version 1.2 >/dev/null 2>&1 ||
   "$binary" update --check unexpected >/dev/null 2>&1 ||
   "$binary" update --check --check >/dev/null 2>&1 ||
   "$binary" update --version 0.2.0 --version 0.3.0 >/dev/null 2>&1; then
    echo 'update accepted invalid arguments' >&2
    exit 1
fi
current_version=$("$binary" version)
"$binary" update --check --version "$current_version" |
    grep -F 'already up to date' >/dev/null
"$binary" update --version "$current_version" |
    grep -F 'already up to date' >/dev/null
long_home=
long_home_index=0
while test "$long_home_index" -lt 500; do
    long_home="${long_home}xxxxxxxxxx"
    long_home_index=$((long_home_index + 1))
done
JVMAN_HOME=$long_home "$binary" update --check --version "$current_version" |
    grep -F 'already up to date' >/dev/null
test ! -e "$state_root"

test "$("$binary" source)" = auto
if "$binary" source unknown >/dev/null 2>&1; then
    echo 'source accepted an unknown provider' >&2
    exit 1
fi
"$binary" source foojay
test "$("$binary" source)" = foojay
grep -Fx foojay "$state_root/source.conf" >/dev/null
"$binary" source --list | grep -E '^\* foojay' >/dev/null
"$binary" source --list | grep -E '^  tsinghua' >/dev/null
"$binary" source --list | grep -E '^  huawei' >/dev/null
"$binary" source --list | grep -E '^  aliyun' >/dev/null
if "$binary" install 21 --source unknown >/dev/null 2>&1; then
    echo 'install accepted an unknown source' >&2
    exit 1
fi
"$binary" source auto
test "$("$binary" source)" = auto
grep -Fx auto "$state_root/source.conf" >/dev/null
"$binary" source --reset
test "$("$binary" source)" = auto
test ! -e "$state_root/source.conf"
if "$binary" source add insecure 'http://example.test/{major}' >/dev/null 2>&1 ||
   "$binary" source add incomplete 'https://example.test/latest' >/dev/null 2>&1 ||
   "$binary" source add adoptium 'https://example.test/{major}' >/dev/null 2>&1; then
    echo 'source add accepted an invalid custom source' >&2
    exit 1
fi
custom_template='https://jdk.example.test/v3/{major}?os={os}&arch={arch}&ext={archive}'
"$binary" source add company "$custom_template"
"$binary" source --list | grep -E '^  company[[:space:]]+Custom: company' >/dev/null
test "$(stat -c '%a' "$state_root/sources/company.conf")" = 600
"$binary" source company
test "$("$binary" source)" = company
if "$binary" source remove company >/dev/null 2>&1; then
    echo 'source remove removed the active custom source' >&2
    exit 1
fi
"$binary" source auto
"$binary" source remove company
test ! -e "$state_root/sources/company.conf"
printf 'invalid=true\n' > "$state_root/sources/broken.conf"
if "$binary" source --list >/dev/null 2>&1; then
    echo 'source list accepted a malformed custom source' >&2
    exit 1
fi
"$binary" source remove broken
test ! -e "$state_root/sources/broken.conf"
limit_index=0
while test "$limit_index" -lt 32; do
    printf 'type=adoptium\nurl=https://limit%s.example.test/{major}\n' \
        "$limit_index" > "$state_root/sources/limit${limit_index}.conf"
    limit_index=$((limit_index + 1))
done
if zh_source_limit=$(JVMAN_LANG=zh-CN "$binary" source add overflow \
        'https://overflow.example.test/{major}' 2>&1); then
    echo 'source add exceeded the custom source limit' >&2
    exit 1
fi
printf '%s\n' "$zh_source_limit" | grep -F \
    '自定义下载源数量已达上限' >/dev/null
test ! -e "$state_root/sources/overflow.conf"
rm "$state_root"/sources/limit*.conf

"$binary" add "$discovery_name" "$name_conflict"
"$binary" add manual-discovered "$discovered_a"
registered_preview=$("$binary" discover)
printf '%s\n' "$registered_preview" | grep -F "$discovered_a" |
    grep -F 'registered:manual-discovered' >/dev/null
JVMAN_LANG=zh-CN "$binary" discover |
    grep -F '已注册:manual-discovered' >/dev/null
"$binary" discover --register
test "$("$binary" which manual-discovered)" = "$discovered_a"
test "$("$binary" which "$discovery_name_2")" = "$discovered_b"
test "$("$binary" which "$discovery_name_3")" = "$discovered_c"
registered_list=$("$binary" list)
case $registered_list in
    *"$discovered_jre"*|*"$discovered_invalid"*)
        echo 'discover registered a JRE or invalid candidate' >&2
        exit 1
        ;;
esac
grep -Fx 'managed=0' "$state_root/versions/$discovery_name_2.conf" >/dev/null
test ! -e "$state_root/current"
test ! -e "$state_root/current.version"
set -- "$state_root"/versions/*.conf
configuration_count=$#
"$binary" discover --register
set -- "$state_root"/versions/*.conf
test "$#" -eq "$configuration_count"
"$binary" remove "$discovery_name_2"
"$binary" remove "$discovery_name_3"
test -f "$discovered_b/bin/javac"
test -f "$discovered_c/bin/javac"

HOME=$original_home
export HOME
PATH=$original_path
export PATH
if test -n "$original_java_home"; then
    JAVA_HOME=$original_java_home
    export JAVA_HOME
else
    unset JAVA_HOME
fi

make_jdk "$test_root/fixtures/jdk-a"
make_jdk "$test_root/fixtures/jdk-b"
make_jdk "$test_root/fixtures/jdk-legacy-uninstall"
"$binary" add a "$test_root/fixtures/jdk-a"
"$binary" add b "$test_root/fixtures/jdk-b"
"$binary" add legacy-uninstall "$test_root/fixtures/jdk-legacy-uninstall"
"$binary" uninstall legacy-uninstall
test ! -e "$state_root/versions/legacy-uninstall.conf"
test -f "$test_root/fixtures/jdk-legacy-uninstall/bin/javac"
use_output=$("$binary" use a)
printf '%s\n' "$use_output" | grep -F 'jvman init' >/dev/null
test "$("$binary" current)" = a
test -f "$state_root/current/bin/java"
initialized_use_output=$(PATH="$state_root/current/bin:$PATH" "$binary" use a)
case $initialized_use_output in
    *'jvman init'*)
        echo 'use printed an initialization hint for an initialized shell' >&2
        exit 1
        ;;
esac
"$binary" exec a -- sh -c 'test "$JAVA_HOME" = "$1"' sh "$test_root/fixtures/jdk-a"
"$binary" use b
"$binary" remove a

archive_source=$test_root/archive-source
archive=$test_root/fake-jdk.tar
make_jdk "$archive_source/fake-jdk"
tar -cf "$archive" -C "$archive_source" fake-jdk
"$binary" install packed --archive "$archive"
if "$binary" install another --archive "$archive" --source foojay >/dev/null 2>&1; then
    echo 'local archive install accepted --source' >&2
    exit 1
fi
test -f "$archive"
test -f "$("$binary" which packed)/bin/javac"
"$binary" remove packed
echo "All CLI integration tests passed."
