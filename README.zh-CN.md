# jvman

[English](README.md)

`jvman` 是一个用 C11 编写的轻量级 Java 版本管理器。它借鉴 nvm 的使用方式，用一个原生可执行文件完成 JDK 下载、注册、切换、查询和命令隔离执行；核心代码不链接第三方运行库。

本文档对应 jvman `0.2.0`。

首版以 Windows 10+ 为主要目标，同时保留 Linux/macOS 平台层。Windows 切换使用 NTFS directory junction，通常不需要管理员权限，也不需要开启 Developer Mode。

## 当前功能

- 通过 Adoptium 或 Foojay API 按 Java 主版本下载 Temurin JDK。
- 使用所选下载源元数据中的 SHA-256 校验远程安装包。
- 从本地归档离线安装，可额外固定 SHA-256。
- 注册已有 JDK，不复制原目录。
- 自动发现已安装的 Java，并可批量注册有效 JDK。
- 通过稳定的 `current` 路径切换版本。
- 用指定 JDK 运行单条命令，不修改全局选择。
- 检查 `JAVA_HOME`、`PATH`、下载器、解压器和当前 junction 状态。

## 构建

使用 GCC 或 Clang：

```text
make
make test
make integration-test
make installer-test       # Windows 安装程序端到端测试
```

使用 CMake：

```text
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

当前机器可直接使用 MSYS2 UCRT64：

```powershell
$env:Path = 'C:\msys64\ucrt64\bin;' + $env:Path
mingw32-make.exe
```

构建后可直接运行 `.\jvman.exe`，也可以把它放入一个用户可写且已加入 `PATH` 的目录。`jvman` 命令行本身不会自动修改注册表、系统 `PATH` 或 PowerShell profile。Windows 构建还会生成可选的 `jvman-setup.exe`，在用户明确确认后可配置当前用户或系统 `PATH`；Make 产物位于 `.\jvman-setup.exe`，CMake 产物位于 `<build>\jvman-setup.exe`。

## Windows 安装程序

`jvman-setup.exe` 是原生 Win32、仅面向当前用户的安装程序，不请求管理员权限，也不写入机器级设置。默认位置如下：

| 项目 | 默认位置 |
| --- | --- |
| 程序文件 | `%LOCALAPPDATA%\Programs\jvman` |
| jvman 数据 | `%LOCALAPPDATA%\jvman`（或有效的 `JVMAN_HOME`） |
| 安装器状态 | `HKCU\Software\jvman\Installer` |

不带参数启动时，安装器会依次询问是否把程序目录加入 `PATH`、PATH 项写入“仅当前用户”还是“所有用户”、是否用有效的 `current` JDK 配置 `JAVA_HOME` 与 `current\bin`，以及是否在安装后执行 `jvman discover --register`。已有 `PATH` 项会保留，精确或规范化后的重复项不会再次加入；写入系统 `PATH` 需要以管理员身份运行安装器。

常用命令行参数：

```text
jvman-setup.exe /S [/DIR=<绝对路径>] [/USER_PATH|/SYSTEM_PATH] [/NO_PATH]
jvman-setup.exe /CONFIGURE_JAVA [/REPLACE_JAVA_HOME]
jvman-setup.exe /DISCOVER
jvman-setup.exe /PORTABLE /DIR=<绝对路径>
jvman-setup.exe /UNINSTALL [/S]
jvman-setup.exe /HELP
```

`/S`、`/SILENT` 和 `/QUIET` 表示静默安装。`/ADD_TO_PATH` 显式开启 `PATH` 更新。`/USER_PATH` 写入当前用户环境并且是默认值；`/SYSTEM_PATH`（也可用 `/MACHINE_PATH` 或 `/ALL_USERS_PATH`）写入系统环境，需要管理员权限。`/NO_PATH`（或 `/NO_ADD_TO_PATH`）关闭 PATH 更新；重复安装时还会删除此前由本安装器拥有的 PATH 项。`/CONFIGURE_JAVA` 只在确认后生效，并要求 `<数据目录>\current\bin\java.exe` 与 `javac.exe` 均存在；如果已有不同的 `JAVA_HOME`，没有 `/REPLACE_JAVA_HOME` 时会报告冲突并保持原值。`/DISCOVER` 只在安装完成后执行发现，不会自动选择当前 JDK。环境变量更新会广播给已运行的程序，但建议打开新终端后再使用新的 `PATH`。

`/NO_CONFIGURE_JAVA` 会关闭 Java 环境配置；重复安装时，如果这些变量曾由本安装器管理，会恢复原先的 `JAVA_HOME`。

无人值守部署时，请将需要的参数与 `/S` 组合使用；不带 `/S` 时会显示相同选项的图形确认对话框。

`/PORTABLE /DIR=...` 只把 `jvman.exe` 解压到显式指定的目录，不创建 `JVMAN_HOME`，不写注册表，也不修改 `PATH` 或 `JAVA_HOME`，适合 U 盘或项目目录。普通卸载可从“应用和功能”进入，也可执行 `/UNINSTALL`；它只删除本次安装拥有的程序文件和环境变量项，已注册的 JDK、`JVMAN_HOME`、`jdks`、`cache`、`versions` 与 `current` 数据都会保留。

## 快速开始

```powershell
# 下载当前平台最新的 Temurin 21
jvman install 21

# 持久切换下载源，也可仅覆盖单次安装
jvman source foojay
jvman install 17 --source adoptium

# 也可以注册机器上已经存在的 JDK
jvman add oracle-23 'C:\Program Files\Java\jdk-23'

# 先预览已安装的 Java，需要时再注册新 JDK
jvman discover
jvman discover --register

jvman list
jvman use 21

# 让当前 PowerShell 会话使用稳定的 JAVA_HOME/current 路径
jvman init powershell | Invoke-Expression

java -version
jvman current
```

希望每个新 PowerShell 会话自动生效时，可把下面一行加入 PowerShell profile：

```powershell
jvman init powershell | Invoke-Expression
```

初始化后，`JAVA_HOME` 始终指向 `<数据目录>\current`，`PATH` 始终包含 `current\bin`。后续执行 `jvman use <名称>` 只会重定向 junction，因此已经初始化过的终端不需要重复修改环境变量；下一次启动的 Java 进程会直接使用新版本。

## 命令

```text
jvman install <主版本> [--name <名称>] [--sha256 <固定校验值>] [--source <下载源>]
jvman install <名称> --archive <本地文件> [--sha256 <校验值>]
jvman source [--list|--reset|<下载源>]
jvman add <名称> <JDK_HOME>
jvman discover [--register]
jvman use <名称>
jvman list
jvman current
jvman which [名称]
jvman remove <名称>
jvman exec <名称> [--] <命令> [参数...]
jvman init [powershell|cmd|sh]
jvman doctor
jvman home
```

别名：`ls` 等同于 `list`，`default` 等同于 `use`，`uninstall` 等同于 `remove`。

`jvman source` 显示当前下载源，`jvman source --list` 列出内置的
`adoptium` 和 `foojay`，`jvman source <下载源>` 持久保存选择，`--reset`
恢复 Adoptium。安装命令的 `--source` 只覆盖本次下载。下载源名称仅接受内置白名单，
元数据和安装包地址必须使用 HTTPS，且所有远程安装包都必须提供并通过 SHA-256 校验。

常用示例：

```powershell
jvman exec 17 -- java -version
jvman exec 8 -- mvn test
jvman install company-jdk --archive .\jdk.zip --sha256 <64位十六进制值>
```

Windows 下 `exec` 支持从 `PATH/PATHEXT` 解析 `.exe`、`.com`、`.cmd` 和 `.bat`，因此 Maven 的 `mvn.cmd` 可以直接使用。

## 自动发现已安装的 Java

`jvman discover` 只做只读预览：不会创建 `JVMAN_HOME`、写入注册配置、切换 `current`，也不会运行任何发现到的 Java 可执行文件。输出包含以下列：

| 列 | 含义 |
| --- | --- |
| `TYPE` | 检测到的运行时类型：`JDK`、`JRE` 或 `INVALID`。 |
| `VERSION` | 安装目录 `release` 文件中的 `JAVA_VERSION`。 |
| `VENDOR` | 规范化后的厂商标识，例如 `temurin`、`corretto` 或 `oracle`。 |
| `NAME` | 已有注册名，或建议使用的 `<vendor>-<version>` 名称。 |
| `STATUS` | 该安装是新 JDK、已注册、JRE，还是无效候选。 |
| `SOURCES` | 解析到同一个 Java home 的全部发现来源。 |
| `JAVA_HOME` | 规范化后的安装路径。 |

状态值包括：`new` 表示尚未注册且可注册的 JDK；`registered:<名称>` 表示该路径已经用此名称注册；`jre` 表示它不是完整 JDK；`invalid` 表示候选无法安全注册。

发现流程会检查 `JAVA_HOME` 和 `PATH` 中的每一项，并解析引号、环境变量、链接以及 Windows Java shim 的真实目标。还会检查以下有限范围：

- Windows 的 HKLM/HKCU JavaSoft JDK/JRE 注册表项（同时读取 32/64 位视图），以及 Program Files、Program Files (x86) 和 LocalAppData Programs 下的常见厂商目录。
- `~/.jdks`、SDKMAN 和 Jabba 等用户级目录。
- Linux 的 `/usr/java`、`/usr/lib*/jvm` 等固定目录。
- macOS 的 `JavaVirtualMachines` 与固定 Homebrew 目录。

jvman 只枚举这些固定根目录及必要的一至两层，不会扫描整块磁盘。相同规范路径只显示一次，多个来源会合并到 `SOURCES`。

`jvman discover --register` 会重新验证候选，并且只注册状态为 `new` 的 JDK。可注册 JDK 必须同时具有 `java`、`javac` 和不超过 64 KiB 的有效 `release` 文件，且 Java 版本不低于 8。JRE 只保留在预览中，永远不会注册。名称冲突会稳定追加 `-2`、`-3` 等后缀；现有配置不会被覆盖，重复执行保持幂等，也不会自动选择当前版本。`JVMAN_HOME/current` 和 `JVMAN_HOME/jdks` 下的路径只用于匹配已有注册，不会生成新的 external 别名。

自动发现注册的 JDK 都采用 external 语义：`jvman remove` 只删除注册记录，绝不会删除原 JDK 安装目录。

## 数据目录

Windows 默认使用 `%LOCALAPPDATA%\jvman`，Unix 默认使用 `${XDG_DATA_HOME:-~/.local/share}/jvman`。可以用 `JVMAN_HOME` 覆盖。

```text
jvman/
  cache/             临时下载与供应商元数据
  jdks/              由 jvman 管理的 JDK
  staging/           尚未提交的安装内容
  versions/*.conf    名称到 JAVA_HOME 的注册记录
  source.conf        当前远程下载源
  current            指向当前 JDK 的 junction/symlink
  current.version    当前注册名称
  state.lock         跨进程状态锁
```

删除外部注册只删除配置，不会删除原 JDK。删除托管安装时，只允许删除精确的 `jdks/<名称>` 目录；当前 junction 正在使用的 JDK不能删除。

## 设计参考

- [SDKMAN](https://sdkman.io/usage/)：`install/use/current` 命令和候选版本目录。
- [jEnv](https://github.com/jenv/jenv)：注册现有 JDK、`which` 和 `doctor`。
- [Jabba](https://github.com/Jabba-Team/jabba)：小型跨平台 Java 管理器和 shell 环境输出。
- [HMCL](https://github.com/HMCL-dev/HMCL)：有限深度的厂商目录与 JavaSoft 注册表发现思路。
- [nvm-windows](https://github.com/coreybutler/nvm-windows)：使用稳定路径切换版本，避免反复重写 `PATH`。

本项目没有复制这些项目的源代码。

## 当前边界

`0.2.0` 通过 Adoptium 或 Foojay 目录安装 Temurin，远程安装只接受 Java 主版本。本地发现可以识别常见 JDK 厂商，但远程选择其他 JDK 供应商、托管 JRE、EA/GraalVM、复杂版本范围、项目级 `.java-version`、自更新和机器级环境变量修改暂不实现。

远程包必须匹配所选下载源返回的 SHA-256；用户传入 `--sha256` 时还会再校验用户固定值。该校验可以发现下载损坏或包不匹配，但不等价于独立的发布签名验证。

解压依赖操作系统自带的 `tar`。远程归档通过内置 HTTPS 下载源获取并经过校验；本地归档默认视为可信输入，传入 `--sha256` 可固定其内容。安装被强制终止时可能遗留 cache/staging 临时文件，可在确认没有安装进程运行后手动清理。

Windows 构建面向 1903 及以上版本，并嵌入 UTF-8 active-code-page manifest，因此中文、emoji、空格路径可以贯穿命令行、JDK 注册和 junction 操作。
