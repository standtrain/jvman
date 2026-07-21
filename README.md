# jvman

[简体中文](README.zh-CN.md)

A small Java version manager written in C11. One native executable, no linked
third-party runtime, NTFS junctions for switching (no admin, no Developer Mode).

Version: `0.2.1`. Primary target: Windows 10 1903+. Linux/macOS core is
portable but the persistent-activation path differs (see below).

## Install

Pick one:

- **Setup bundle (Windows)** — download `jvman-setup.exe`, run interactively or
  silently. Handles PATH, `JAVA_HOME`, ARP entry, uninstall.
- **Single binary** — drop `jvman.exe` anywhere on `PATH`. First
  `jvman install` / `jvman use` will self-persist `JAVA_HOME` and `Path`
  (opt-out below). No admin required.
- **Portable** — `jvman-setup.exe /PORTABLE /DIR=<abs-path>` extracts the
  binary only; no registry, no PATH, no `JAVA_HOME`.

## Quick start

```powershell
jvman install 21                    # download Temurin 21, persist JAVA_HOME/PATH
jvman use 21                        # switch (also persists if not yet)
jvman list                          # what's registered
jvman current                       # what's active
java -version                       # new terminals: works immediately
```

Existing terminal opened before install? Refresh once:

```powershell
jvman init powershell | Invoke-Expression     # PowerShell
for /f "delims=" %L in ('jvman init cmd') do @%L   # CMD
```

Switching version (`jvman use 17`) does **not** rewrite `PATH` or `JAVA_HOME`.
It rewrites one directory junction (`<data-home>\current`); every already-
initialized shell picks the new JDK on the next process spawn. Java/Maven/
Gradle daemons must restart.

## Commands

```text
jvman install <major> [--name <n>] [--sha256 <hex>] [--source <src>] [--no-persist]
jvman install <name> --archive <file> [--sha256 <hex>] [--no-persist]
jvman add <name> <jdk-home>              register an existing JDK (no copy)
jvman use <name> [--no-persist]          switch active JDK
jvman activate [--replace-java-home]     write HKCU/rc; idempotent
jvman deactivate                         reverse activate
jvman list | ls
jvman current
jvman which [name]
jvman remove <name>                      unregister; external JDKs stay on disk
jvman uninstall [<name>]                 alias of remove; no arg → run installer
jvman exec <name> [--] <cmd> [args...]   run with selected JDK, no global change
jvman init [powershell|cmd|sh]           emit shell-specific env script
jvman discover [--register]              scan system for JDKs
jvman doctor                             validate JAVA_HOME/PATH/tools
jvman update [--check] [--version <v>]   self-update from GitHub releases
jvman source [--list|--reset|<name>|add <n> <tmpl>|remove <n>]
jvman language [--list|en|zh-CN]
jvman home                               print data-home
```

Aliases: `ls`=`list`, `default`=`use`.

## Persistent activation

`install` and `use` automatically persist so new terminals and reopened IDEs
resolve `java` without further setup.

| Platform | What's written |
| --- | --- |
| Windows | `HKCU\Environment`: `JAVA_HOME=<data-home>\current`, `Path` prepended with `<data-home>\current\bin`. `WM_SETTINGCHANGE` broadcast. State recorded under `HKCU\Software\jvman\Installer` (shared with the installer). |
| POSIX   | `~/.bashrc` and `~/.zshrc` gain a marker-fenced block that runs `eval "$(jvman init sh)"`. State recorded in `<data-home>/rc.state`. |

Controls:

- `--no-persist` on `install`/`use` — skip for that invocation.
- `JVMAN_NO_PERSIST=1` — skip globally.
- `jvman activate` / `jvman deactivate` — explicit control; idempotent.
- `--replace-java-home` — required if another program's `JAVA_HOME` is present;
  without it, activate refuses to overwrite.

Because `JAVA_HOME` points at the junction (not a version-specific dir),
`jvman use <ver>` never touches the registry/rc after the first activate.

## Data layout

`%LOCALAPPDATA%\jvman` on Windows, `${XDG_DATA_HOME:-~/.local/share}/jvman` on
Unix. Override with `JVMAN_HOME`.

```text
<data-home>/
  jdks/              JDKs owned by jvman (deleted on remove <name>)
  versions/*.conf    name → JAVA_HOME registrations
  cache/             transient downloads + vendor metadata
  staging/           in-progress installs
  sources/*.conf     custom Adoptium-compatible sources
  source.conf        selected remote source mode (absent = auto)
  current            junction/symlink to the active JDK
  current.version    active registration name
  state.lock         cross-process mutation lock
  rc.state           (POSIX) list of shell rc files touched by activate
```

Managed JDK deletion is confined to `jdks/<name>` and never follows links.
External registrations (`jvman add`, `discover --register`) never delete the
target on `remove`.

## Sources

Built-in: `tsinghua` (TUNA Adoptium mirror), `huawei` (BiSheng), `aliyun`
(Dragonwell), `adoptium`, `foojay`. `auto` (default) benchmarks all reachable
sources — resolves metadata, then samples a bounded 64 KiB range from the
final archive URL — and downloads once from the fastest.

```powershell
jvman source auto                          # default
jvman source tsinghua                      # pin
jvman install 17 --source adoptium         # override once
jvman source add corp 'https://jdk.example.com/v3/{major}?os={os}&arch={arch}'
```

Custom source contract: Adoptium-compatible JSON, HTTPS only, must include
SHA-256. Template placeholders: `{major}` (required), `{os}`, `{arch}`,
`{archive}`. Credentials in URLs are rejected.

All remote archives are SHA-256 verified against source metadata; `--sha256`
adds a second pin. This detects corruption/mismatch but is not release
signature verification.

## Build

MSYS2 UCRT64 (this repo's tested toolchain):

```powershell
$env:Path = 'C:\msys64\ucrt64\bin;' + $env:Path
mingw32-make.exe          # produces jvman.exe and jvman-setup.exe
```

Portable Make / CMake:

```bash
make && make test && make integration-test
make installer-test       # Windows only

cmake -S . -B build && cmake --build build && ctest --test-dir build
```

Cross-compiling to Windows: setup stub and packer still build; the final
`jvman-setup.exe` bundle is skipped because the packer can't run on the host.

## Windows installer

`jvman-setup.exe`. Per-user by default (no UAC); all-users starts an elevated
machine install through UAC. Manifest is `asInvoker`.

|  | Current user | All users |
| --- | --- | --- |
| Program files | `%LOCALAPPDATA%\Programs\jvman` | `%ProgramFiles%\jvman` |
| Runtime data  | `%LOCALAPPDATA%\jvman` (or `JVMAN_HOME`) | Per invoking user |
| Installer state | `HKCU\Software\jvman\Installer` | `HKLM\Software\jvman\Installer` |

Switches:

```text
jvman-setup.exe /S [/LANG=en|zh-CN] [/DIR=<abs>] [/USER_PATH|/SYSTEM_PATH] [/NO_PATH]
jvman-setup.exe /CONFIGURE_JAVA [/REPLACE_JAVA_HOME]
jvman-setup.exe /DISCOVER
jvman-setup.exe /PORTABLE /DIR=<abs>
jvman-setup.exe /UNINSTALL [/S] [/REMOVE_DATA [/REMOVE_JDKS]]
jvman-setup.exe /UNINSTALL /MACHINE [/S]
jvman-setup.exe /HELP
```

- `/USER_PATH` (default): HKCU Path gains program dir + `<data-home>\current\bin`.
- `/SYSTEM_PATH` (also `/MACHINE_PATH`, `/ALL_USERS_PATH`): HKLM Path gains
  program dir only. Never adds a user-writable JDK dir to the machine PATH.
- `/CONFIGURE_JAVA`: sets HKCU `JAVA_HOME=<data-home>\current`. Refuses to
  overwrite an existing different value unless `/REPLACE_JAVA_HOME`.
- `/NO_PATH` / `/NO_CONFIGURE_JAVA`: on repeat install, also restore values
  previously owned by this installer.

Installer state is shared with the CLI: `jvman-setup.exe /UNINSTALL` rolls back
entries the CLI added via `install`/`use`/`activate`. `jvman deactivate` does
the equivalent from the CLI side.

Uninstall scopes (current-user only; `/MACHINE` cannot combine with either):

- default — program files and env entries owned by this installer.
- `/REMOVE_DATA` — plus `JVMAN_HOME` contents except the top-level `jdks/` dir.
- `/REMOVE_DATA /REMOVE_JDKS` — plus `jdks/`. Never traverses junctions or
  symlinks; externally-registered JDKs are never deleted.

Shell caveat: no process can modify its parent shell's environment. Terminals
open at install time keep their old env until fully closed and reopened (this
includes tabs on an existing Windows Terminal process — close the app, not
just the tab). If a machine PATH Java still shadows the user PATH, put
`jvman init <shell>` in the shell startup file: `$PROFILE.CurrentUserAllHosts`
for PowerShell; a per-user CMD `AutoRun` with **doubled** percent signs
(`%%L`). Preserve any existing AutoRun content.

## Update

```text
jvman update --check          # verify, don't download
jvman update                  # latest stable from github.com/standtrain/jvman
jvman update --version 0.3.0  # explicit, no downgrade
```

- HTTPS only; metadata and binary size capped.
- Binary must match the release's `SHA256SUMS` **and** the PE/ELF/Mach-O
  architecture must match the host.
- Linux/macOS require curl ≥ 7.20.2 in a fixed root-owned system path — a
  `PATH`-injected curl is deliberately ignored. macOS 11's system curl qualifies.
- Windows: replacement runs in a restricted helper after the current process
  exits. Must run from a non-elevated terminal (the user-writable temp dir
  can't be a privilege boundary). If admin access is required, use the
  installer or replace the binary manually.
- Only the running CLI is replaced. `JVMAN_HOME`, JDKs, `PATH`, `JAVA_HOME`,
  shell profiles, and installer state are not touched.
- The checksum is published in the same GitHub Release: it detects corruption
  and mismatched assets but is not an independent signature.

## Discovery

`jvman discover` is a read-only scan. It doesn't create `JVMAN_HOME`, write
registrations, change `current`, or execute any discovered `java`. Columns:
`TYPE | VERSION | VENDOR | NAME | STATUS | SOURCES | JAVA_HOME`.

Status values:

- `new` — registerable JDK not yet registered
- `registered:<name>` — this home is already registered under `<name>`
- `jre` — runtime is not a complete JDK
- `invalid` — cannot be registered safely

Scanned locations (bounded — never a full-disk scan):

- `JAVA_HOME`, every `PATH` entry (quotes/env vars/links/Windows Java shims
  resolved to real targets)
- Windows: HKLM+HKCU JavaSoft keys (32-bit and 64-bit views), Program Files,
  Program Files (x86), LocalAppData Programs
- User: `~/.jdks`, SDKMAN, Jabba
- Linux: `/usr/java`, `/usr/lib*/jvm`
- macOS: `JavaVirtualMachines`, fixed Homebrew roots

`jvman discover --register` revalidates and registers only `new` entries. Must
have `java`, `javac`, and a valid `release` file ≤ 64 KiB with Java ≥ 8. Name
conflicts get `-2`, `-3`, … suffixes. Idempotent; never selects a `current`.
All auto-registered entries use external semantics.

## Design references

Not copied — read for prior art:

- [SDKMAN](https://sdkman.io/usage/) — `install/use/current`, stable candidate layout
- [jEnv](https://github.com/jenv/jenv) — registering existing JDKs, `which`, `doctor`
- [Jabba](https://github.com/Jabba-Team/jabba) — small cross-platform manager, shell env output
- [HMCL](https://github.com/HMCL-dev/HMCL) — bounded vendor / JavaSoft discovery
- [nvm-windows](https://github.com/coreybutler/nvm-windows) — stable-path switching over `PATH` rewrites

## Scope and non-goals

`0.2.1` supports:

- Auto/pinned source benchmarking across global, TUNA, Huawei, Aliyun, custom
- Remote install by Java major version
- Local discovery of common JDK vendors

Deferred:

- Multi-vendor selection at install time
- Managed JREs, EA/GraalVM channels
- Semver range install syntax
- `.java-version` project files
- Independent signed release manifests

Non-goals of the CLI:

- Never adds a user-writable JDK dir to the machine PATH.
- Never runs unknown external binaries during discovery.
- Never scans an entire disk.
- Never modifies files outside `JVMAN_HOME` except HKCU env entries (Windows)
  or `~/.bashrc` and `~/.zshrc` marker blocks (POSIX) it explicitly owns.

## Security notes

- All remote fetches are HTTPS. Redirects only across HTTPS.
- Every remote archive must supply and match a SHA-256.
- Junctions/symlinks are never traversed during removal.
- `.env` and `<data-home>/sources/*.conf` are private-mode files.
- `jvman.exe` starts with SafeSearchMode enabled and rewrites Windows command
  resolution to skip the empty PATH element (i.e. the current directory) —
  guards against binary planting during `jvman exec` and extractor invocations.
- Self-update refuses to run elevated on Windows.
