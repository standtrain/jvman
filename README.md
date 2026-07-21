# jvman

[简体中文](README.zh-CN.md)

`jvman` is a small Java version manager written in C11. It manages downloaded
JDKs and existing local JDK installations with one native executable and no
linked third-party runtime libraries.

This documentation describes jvman `0.2.0`.

The first release targets Windows while keeping the core portable to Linux and
macOS. On Windows, switching uses an NTFS directory junction, so it normally
does not require administrator privileges or Developer Mode.

After building, place `jvman.exe` in a user-writable directory on `PATH`, or
run it as `.\jvman.exe`. The `jvman` CLI does not edit the registry, the system
`PATH`, or shell profile files automatically. On Windows, the optional
`jvman-setup.exe` bundle below can configure the current-user or system `PATH`
after an explicit choice.

## Features

- Automatically benchmark global and China-friendly JDK sources before downloading.
- Verify remote archives with the SHA-256 published by the selected source.
- Install from a local archive for offline use.
- Register an existing JDK without copying it.
- Discover installed Java runtimes and optionally batch-register valid JDKs.
- Switch through one stable `current` path.
- Run a command with a selected JDK without changing global state.
- Diagnose `JAVA_HOME`, `PATH`, downloader, and extractor configuration.
- Update the CLI from verified assets in this project's GitHub Releases.

## Build

With GCC or Clang:

```text
make
make test
make integration-test
make installer-test       # Windows setup end-to-end test
```

With CMake:

```text
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

On this Windows machine the available toolchain is MSYS2 UCRT64, so the exact
build command is:

```powershell
$env:Path = 'C:\msys64\ucrt64\bin;' + $env:Path
mingw32-make.exe
```

On Windows, `mingw32-make.exe` builds `jvman.exe` and the self-contained
`jvman-setup.exe` bundle. The CMake Windows build produces the same bundle in
the selected build directory. A cross-compiled Windows build can still build
the setup stub and packer, but skips the final bundle because the host cannot
execute the packer. With Make, the finished artifact is `.\jvman-setup.exe`;
with CMake it is `<build>\jvman-setup.exe`.

## Windows Installer

`jvman-setup.exe` is a native Win32 installer. By default it installs for the
current user and does not request administrator privileges; choosing the system
`PATH` requires running the installer as administrator. The default locations
are:

| Item | Default |
| --- | --- |
| Program files | `%LOCALAPPDATA%\Programs\jvman` |
| jvman data | `%LOCALAPPDATA%\jvman` (or a valid `JVMAN_HOME`) |
| Registry state | `HKCU\Software\jvman\Installer` |

Interactive runs begin with a language dropdown populated from the installer's
built-in language table. The Windows UI language selects the initial option;
canceling the dialog keeps that default.

When started without switches, the installer asks whether to add the program
directory to `PATH`, whether those PATH entries should be written for only the
current user or for all users, whether a valid `current` JDK should provide
`JAVA_HOME` and `current\bin`, and whether to run `jvman discover --register`.
Existing `PATH` entries are retained and exact or canonical duplicates are not
added.

The normal command-line switches are:

```text
jvman-setup.exe /S [/DIR=<absolute-directory>] [/USER_PATH|/SYSTEM_PATH] [/NO_PATH]
jvman-setup.exe /CONFIGURE_JAVA [/REPLACE_JAVA_HOME]
jvman-setup.exe /DISCOVER
jvman-setup.exe /PORTABLE /DIR=<absolute-directory>
jvman-setup.exe /UNINSTALL [/S]
jvman-setup.exe /HELP
```

`/S`, `/SILENT`, and `/QUIET` suppress dialogs. `/ADD_TO_PATH` explicitly
enables the PATH update. `/USER_PATH` writes PATH entries to the current-user
environment and is the default; `/SYSTEM_PATH` (also `/MACHINE_PATH` or
`/ALL_USERS_PATH`) writes PATH entries to the machine environment and requires
administrator permission. `/NO_PATH` (or `/NO_ADD_TO_PATH`) disables PATH
updates and removes a PATH entry previously owned by this installer on upgrade.
`/CONFIGURE_JAVA` is opt-in and requires `<data-home>\current\bin\java.exe`
and `javac.exe`; without
`/REPLACE_JAVA_HOME`, a different existing `JAVA_HOME` is treated as a conflict
and is left unchanged. `/DISCOVER` runs discovery only after installation and
does not select a current JDK. Use a new terminal after an environment update
so that it receives the broadcast change.

`/NO_CONFIGURE_JAVA` disables Java environment configuration; on a repeat
install it restores any `JAVA_HOME` value previously managed by this installer.

For unattended provisioning, combine the desired switches with `/S`; a
non-silent run presents the same choices in the graphical prompts.

`/PORTABLE /DIR=...` extracts only `jvman.exe` to the explicitly supplied
directory. It does not create `JVMAN_HOME`, write the registry, or modify
`PATH`/`JAVA_HOME`, and is suitable for a USB or per-project copy. The regular
uninstaller is available from Add/Remove Programs or `/UNINSTALL`; it removes
only files and environment entries owned by this installation. Registered JDKs,
`JVMAN_HOME`, `jdks`, `cache`, `versions`, and `current` data are preserved.

## Quick Start

```powershell
# Download the latest Temurin build for a Java major version.
jvman install 21

# Automatic source selection is the default; fixed sources remain available.
jvman source auto
jvman source tsinghua
jvman install 17 --source adoptium

# Add an Adoptium-compatible custom metadata endpoint.
jvman source add company 'https://jdk.example.com/v3/{major}?os={os}&arch={arch}'

# Or register a JDK that already exists.
jvman add oracle-23 'C:\Program Files\Java\jdk-23'

# Preview installed Java runtimes, then register new JDKs if desired.
jvman discover
jvman discover --register

jvman list
jvman use 21

# Apply the stable JAVA_HOME/PATH to this PowerShell session.
jvman init powershell | Invoke-Expression

java -version
jvman current
```

Add the following line to the PowerShell profile once if every new shell should
use the selected JDK:

```powershell
jvman init powershell | Invoke-Expression
```

`JAVA_HOME` points to `<data-home>\current`, not a version-specific directory.
After this one-time shell initialization, `jvman use <name>` redirects the
junction and the next Java process sees the new JDK immediately.

## Commands

```text
jvman install <major> [--name <name>] [--sha256 <hex>] [--source <name>]
jvman install <name> --archive <file> [--sha256 <hex>]
jvman source [--list|--reset|<name>]
jvman source add <name> <HTTPS-template>
jvman source remove <name>
jvman add <name> <jdk-home>
jvman discover [--register]
jvman use <name>
jvman list
jvman current
jvman which [name]
jvman remove <name>
jvman exec <name> [--] <command> [args...]
jvman init [powershell|cmd|sh]
jvman doctor
jvman update [--check] [--version <version>]
jvman home
```

Aliases: `ls` for `list`, `default` for `use`, and `uninstall` for `remove`.

`jvman source` prints the active mode. The built-in sources are `tsinghua`
(TUNA Adoptium mirror), `huawei` (BiSheng), `aliyun` (Dragonwell), `adoptium`,
and `foojay`; `auto` benchmarks all available built-in and custom sources.
`jvman source <name>` persists a selection, and `--reset` restores automatic
selection. An install-level `--source` only overrides that invocation.

Custom sources use an Adoptium-compatible JSON response containing an HTTPS
package URL and SHA-256 checksum. Add one with `jvman source add <name>
<HTTPS-template>`; supported placeholders are `{major}`, `{os}`, `{arch}`, and
`{archive}`. The template must contain `{major}`, cannot contain URL credentials,
and is stored in `<data-home>/sources/<name>.conf` with private permissions.
Select another source before removing an active custom source. Do not place API
keys or other credentials in source URLs.

In `auto` mode, each install measures the complete metadata and checksum
resolution time for every source with a short timeout, ignores unavailable
sources, and downloads the JDK once from the fastest successful result. The
Tsinghua source also checks the TUNA mirror itself before it can be selected.
The benchmark does not download sample JDK archives. Metadata and package URLs
must use HTTPS, and every remote package must include and match a SHA-256
checksum.

Examples:

```powershell
jvman exec 17 -- java -version
jvman exec 8 -- mvn test
jvman install company-jdk --archive .\jdk.zip --sha256 <64-hex-digits>
```

## Updating jvman

```text
jvman update --check
jvman update
jvman update --version 0.3.0
```

Without `--version`, the command checks the latest stable release of
`github.com/standtrain/jvman`. When a newer version is found, `--check` verifies
the matching checksum entry but does not download or replace the executable.
An explicit version skips latest-release metadata lookup and must be
`MAJOR.MINOR.PATCH` (an optional leading `v` is accepted); downgrades are
refused.

The updater selects a fixed asset name for Windows x86_64, static Linux x86_64,
or macOS 11+ x86_64/aarch64. It downloads only through HTTPS, caps metadata and
binary sizes, matches the binary against the release's `SHA256SUMS`, and checks
the PE, ELF, or Mach-O architecture before publishing it. Linux and macOS
require curl 7.20.2 or newer in a fixed, root-owned system directory; a `curl`
found only in a custom `PATH` is deliberately ignored. macOS 11's system curl
meets this requirement. Temporary files use restricted permissions, and
completed success or failure paths make a best-effort cleanup. Windows can
leave a helper in the user temp directory until the next restart when immediate
self-deletion is unavailable; manual cleanup may be needed if deletion cannot
be deferred. The checksum detects corruption and mismatched assets, but because
it is published in the same GitHub Release, it is not an independent signature.

On Windows, replacement finishes in a restricted helper after the running
process exits. Self-update must run from a non-elevated terminal so the
user-writable temporary directory never becomes a privilege boundary. Use the
installer or replace the binary manually when administrator access is required,
then run `jvman version` to confirm the result. On Linux and macOS, replacement
is atomic before the command returns. The executable's directory must be
writable by the current user. Updating replaces only the running CLI:
it does not modify `JVMAN_HOME`, installed JDKs, `PATH`, `JAVA_HOME`, shell
profiles, or Windows installer state.

## Discovering Installed Java

`jvman discover` is a read-only preview. It does not create `JVMAN_HOME`, write
registrations, change `current`, or run any discovered Java executable. Its
output has these columns:

| Column | Meaning |
| --- | --- |
| `TYPE` | Detected runtime type: `JDK`, `JRE`, or `INVALID`. |
| `VERSION` | `JAVA_VERSION` from the installation's `release` file. |
| `VENDOR` | Normalized vendor slug, such as `temurin`, `corretto`, or `oracle`. |
| `NAME` | Existing registration name or the proposed `<vendor>-<version>` name. |
| `STATUS` | Whether the installation is new, registered, a JRE, or invalid. |
| `SOURCES` | All discovery sources that resolved to this same Java home. |
| `JAVA_HOME` | Canonical installation path. |

The status values are `new` for a registerable unregistered JDK,
`registered:<name>` for a home already registered under that name, `jre` for a
runtime that is not a complete JDK, and `invalid` for a candidate that cannot
be registered safely.

Discovery checks `JAVA_HOME` and every `PATH` entry, resolving quotes,
environment variables, links, and Windows Java shims to their real targets.
It also checks the following bounded locations:

- Windows JavaSoft JDK/JRE keys in HKLM and HKCU, in both 32-bit and 64-bit
  registry views, plus known vendor directories under Program Files,
  Program Files (x86), and LocalAppData Programs.
- User-managed roots such as `~/.jdks`, SDKMAN, and Jabba.
- Linux roots including `/usr/java` and `/usr/lib*/jvm`.
- macOS `JavaVirtualMachines` and fixed Homebrew roots.

Only those fixed roots and their necessary one or two child levels are
enumerated; jvman never scans an entire disk. Canonical paths are deduplicated,
and multiple origins are merged in `SOURCES`.

`jvman discover --register` revalidates candidates and registers only `new`
JDKs. A registerable JDK must contain `java`, `javac`, and a valid `release`
file no larger than 64 KiB, and its Java version must be 8 or newer. JREs
remain visible but are never registered. Name
conflicts receive stable `-2`, `-3`, and later suffixes; existing configuration
is never overwritten, repeated runs are idempotent, and registration does not
select a current version. Paths under `JVMAN_HOME/current` or `JVMAN_HOME/jdks`
only match existing registrations and do not create new external aliases.

All discovered registrations use external semantics: `jvman remove` removes
the registration record but never deletes the original JDK installation.

## Data Layout

The default data directory is `%LOCALAPPDATA%\jvman` on Windows and
`${XDG_DATA_HOME:-~/.local/share}/jvman` on Unix. Set `JVMAN_HOME` to override
it.

```text
jvman/
  cache/             temporary downloads and provider metadata
  jdks/              JDKs owned by jvman
  staging/           incomplete installs, never activated
  versions/*.conf    registered name -> JAVA_HOME records
  source.conf        selected remote source mode (absent means auto)
  sources/*.conf     custom Adoptium-compatible source definitions
  current            junction/symlink to the selected JDK
  current.version    selected registration name
  state.lock         cross-process mutation lock
```

Removing an external registration never deletes its JDK. Removing a managed
installation only deletes the exact `jdks/<name>` directory, and the active JDK
cannot be removed.

## Design References

The command model and layout draw on these established projects:

- [SDKMAN](https://sdkman.io/usage/) for `install`, `use`, `current`, and the
  stable candidate layout.
- [jEnv](https://github.com/jenv/jenv) for registering existing JDKs, `which`,
  and `doctor`.
- [Jabba](https://github.com/Jabba-Team/jabba) for a small cross-platform Java
  manager and shell-generated environment changes.
- [HMCL](https://github.com/HMCL-dev/HMCL) for bounded vendor-directory and
  JavaSoft registry discovery concepts.
- [nvm-windows](https://github.com/coreybutler/nvm-windows) for switching a
  stable path instead of repeatedly rewriting `PATH`.

No source code is copied from those projects.

## Current Scope

Version `0.2.0` automatically selects between built-in global, Tsinghua, Huawei,
Aliyun, and custom catalogs and accepts a Java major version for remote installs.
Local discovery recognizes
common JDK vendors, but multi-vendor JDK selection, managed JREs, EA builds, semantic
version ranges, project `.java-version` files, signed release manifests, and
automatic CLI edits to persistent environment settings are intentionally deferred.

The native Windows build targets Windows 10 version 1903 or later. Its embedded
UTF-8 code-page manifest allows non-ASCII data and JDK paths without requiring
wide-character command-line wrappers.

Remote archives are accepted only after matching the SHA-256 returned by the
selected source. This detects corruption and unexpected content from a mismatched
download; it is not a replacement for independent signature verification.

Archive extraction uses the operating system's `tar` implementation. The URL
comes from a built-in HTTPS source and is checksum-verified. Local
archives are trusted input unless `--sha256` is supplied.
