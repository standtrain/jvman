# jvman

[English](README.md)

`jvman` 是一个用 C11 编写的轻量级 Java 版本管理器。它借鉴 nvm 的使用方式，用一个原生可执行文件完成 JDK 下载、注册、切换、查询和命令隔离执行；核心代码不链接第三方运行库。

本文档对应 jvman `0.2.1`。

首版以 Windows 10+ 为主要目标，同时保留 Linux/macOS 平台层。Windows 切换使用 NTFS directory junction，通常不需要管理员权限，也不需要开启 Developer Mode。

## 当前功能

- 下载前自动测速并选择国际源或适合中国地区的 JDK 源。
- 使用所选下载源元数据中的 SHA-256 校验远程安装包。
- 从本地归档离线安装，可额外固定 SHA-256。
- 注册已有 JDK，不复制原目录。
- 自动发现已安装的 Java，并可批量注册有效 JDK。
- 通过稳定的 `current` 路径切换版本。
- 用指定 JDK 运行单条命令，不修改全局选择。
- 检查 `JAVA_HOME`、`PATH`、下载器、解压器和当前 junction 状态。
- 从本项目 GitHub Releases 中经过校验的产物更新 jvman 命令行。

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

构建后可直接运行 `.\jvman.exe`，也可以把它放入一个用户可写且已加入 `PATH` 的目录。`jvman` 命令行不会自动修改系统 `PATH` 或 PowerShell profile；Windows 上只有 `jvman language` 会在 `HKCU\Software\jvman\Preferences` 写入当前用户语言偏好。Windows 构建还会生成可选的 `jvman-setup.exe`；Make 产物位于 `.\jvman-setup.exe`，CMake 产物位于 `<build>\jvman-setup.exe`。

## Windows 安装程序

`jvman-setup.exe` 是原生 Win32 安装程序。默认按当前用户安装，不请求管理员权限；选择“所有用户”时才通过 Windows UAC 启动机器安装流程。manifest 仍为 `asInvoker`，所以当前用户安装和便携解压不会无条件提权。默认位置如下：

| 项目 | 当前用户 | 所有用户 |
| --- | --- | --- |
| 程序文件 | `%LOCALAPPDATA%\Programs\jvman` | `%ProgramFiles%\jvman` |
| 运行数据 | `%LOCALAPPDATA%\jvman`（或 `JVMAN_HOME`） | 按每个调用用户独立解析 |
| 安装器状态 | `HKCU\Software\jvman\Installer` | `HKLM\Software\jvman\Installer` |

非静默运行会先显示由安装器内置语言表动态生成的语言下拉框。
默认选中“跟随系统”，并根据 Windows 界面语言决定实际界面；也可显式选择
English 或简体中文。取消语言选择会直接退出安装程序。安装成功后，确认的语言会同时写入安装记录和发起安装的 Windows 用户 CLI 偏好。`JVMAN_LANG` 仍可覆盖当前进程，之后也可用 `jvman language` 修改已保存偏好。

不带参数启动时，安装器会依次询问是否启用 `PATH` 集成、PATH 项写入“仅当前用户”还是“所有用户”、是否让 `JAVA_HOME` 指向稳定的 `current` 路径，以及是否在安装后执行 `jvman discover --register`。当前用户 PATH 集成会同时加入程序目录和稳定的 `<数据目录>\current\bin`；即使尚未选择 JDK，也会预先加入该稳定路径，后续 `jvman use` 只需重定向 `current`。所有用户安装仅把受保护的程序目录加入系统 PATH。安装器新拥有的 PATH 项会前置，以便优先于旧的用户级 Java；已有 PATH 项会保留，精确或规范化后的重复项不会再次加入。

常用命令行参数：

```text
jvman-setup.exe /S [/LANG=en|zh-CN] [/DIR=<绝对路径>] [/USER_PATH|/SYSTEM_PATH] [/NO_PATH]
jvman-setup.exe /CONFIGURE_JAVA [/REPLACE_JAVA_HOME]
jvman-setup.exe /DISCOVER
jvman-setup.exe /PORTABLE /DIR=<绝对路径>
jvman-setup.exe /UNINSTALL [/S] [/REMOVE_DATA [/REMOVE_JDKS]]
jvman-setup.exe /UNINSTALL /MACHINE [/S]
jvman-setup.exe /HELP
```

`/S`、`/SILENT` 和 `/QUIET` 表示静默安装。`/LANG=en` 与 `/LANG=zh-CN` 可跳过下拉框并指定安装器及安装后 CLI 的语言。`/ADD_TO_PATH` 显式开启 `PATH` 集成。`/USER_PATH` 把程序目录和稳定的 `<数据目录>\current\bin` 写入当前用户环境，并且是默认值；`/SYSTEM_PATH`（也可用 `/MACHINE_PATH` 或 `/ALL_USERS_PATH`）会把程序安装到受保护的 Program Files、把状态写入 HKLM、加入系统 PATH，并按需请求 UAC。机器模式不会把用户可写的 JDK 目录加入系统 PATH。`/NO_PATH`（或 `/NO_ADD_TO_PATH`）关闭 PATH 更新；重复安装时还会删除此前由本安装器拥有的程序和 Java PATH 项。升级旧版“当前用户安装 + 系统 PATH”状态时，只会提权一个窄范围清理进程来移除已认证的旧系统 PATH 项，随后由原用户进程更新 HKCU，不会把整个用户安装流程放到管理员账户下执行。`/CONFIGURE_JAVA` 把当前用户 `JAVA_HOME` 配置为稳定的 `<数据目录>\current`；该路径可以在首次成功执行 `jvman use` 后才存在。如果已有不同的 `JAVA_HOME`，没有 `/REPLACE_JAVA_HOME` 时会报告冲突并保持原值。`/DISCOVER` 只在当前用户安装完成后执行发现，不会自动选择当前 JDK。

`/NO_CONFIGURE_JAVA` 会关闭 `JAVA_HOME` 配置；重复安装时，如果该变量曾由本安装器管理，会恢复原先的值。它不会覆盖单独作出的 PATH 选择。

可执行文件无法修改父 shell 的环境。安装前已经打开的终端会保留旧的进程环境；已经运行的 Windows Terminal 所创建的新标签页也可能继续继承旧环境。完整退出终端程序后重新打开可接收持久环境；如果系统 PATH 中的旧 Java 仍然靠前，或需要初始化现有终端，请执行一次对应命令：

```cmd
for /f "delims=" %L in ('jvman init cmd') do @%L
```

```powershell
jvman init powershell | Invoke-Expression
```

希望后续终端自动生效时，应把对应初始化放入 shell 启动配置。PowerShell 可加入 `$PROFILE.CurrentUserAllHosts`；CMD 的受信任当前用户 AutoRun 脚本应写成 `for /f "delims=" %%L in ('jvman init cmd') do @%%L`，因为批处理文件中的百分号必须成对书写。必须保留已有 AutoRun 内容，不能直接覆盖。经常以管理员身份启动的 shell 不应自动加载用户可写的 Java 路径。用户 PATH 无法在所有 Windows 进程中安全压过系统 PATH 中的 Java，因此存在该冲突时，shell 启动初始化才是可靠方案。

无人值守部署时，请将需要的参数与 `/S` 组合使用；不带 `/S` 时会显示相同选项的图形确认对话框。

`/PORTABLE /DIR=...` 只把 `jvman.exe` 解压到显式指定的目录，不创建 `JVMAN_HOME`，不写注册表，也不修改 `PATH` 或 `JAVA_HOME`，适合 U 盘或项目目录。普通卸载可执行 `jvman uninstall`，也可从“应用和功能”或 `/UNINSTALL` 进入；机器安装使用 `/UNINSTALL /MACHINE` 并对称请求 UAC。交互式卸载会先选择范围，再显示最终高风险确认；`/S` 仅用于无人值守场景。

卸载范围分为三级：默认只删除程序本体和本安装器拥有的环境变量；`/REMOVE_DATA` 还会删除 `JVMAN_HOME` 中除顶层 `jdks` 目录外的数据；`/REMOVE_DATA /REMOVE_JDKS` 会删除整个 `JVMAN_HOME` 及其中由 jvman 管理的 JDK。`/REMOVE_JDKS` 不能单独使用。删除过程不会跟随 junction 或符号链接，通过外部路径注册的 JDK 永远不会被删除。静默卸载属于破坏性操作，应仅在调用方已明确确认删除范围后使用。

数据删除范围仅适用于当前用户安装。机器卸载始终只删除共享程序、HKLM 安装记录和系统 PATH 项；`/MACHINE` 不能与 `/REMOVE_DATA` 或 `/REMOVE_JDKS` 组合，因为每个用户的 CLI 数据目录都是独立解析的。

## 快速开始

```powershell
# 下载当前平台最新的 Temurin 21
jvman install 21

# 默认自动测速选源，也可固定下载源或仅覆盖单次安装
jvman source auto
jvman source tsinghua
jvman install 17 --source adoptium

# 添加返回 Adoptium 兼容元数据的自定义源
jvman source add company 'https://jdk.example.com/v3/{major}?os={os}&arch={arch}'

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

希望每个新 PowerShell 会话自动生效时，可把下面一行加入 `$PROFILE.CurrentUserAllHosts`：

```powershell
jvman init powershell | Invoke-Expression
```

初始化后，`JAVA_HOME` 始终指向 `<数据目录>\current`，`PATH` 始终包含 `current\bin`。后续执行 `jvman use <名称>` 只会重定向 junction，因此所有已经初始化过的终端都不需要重复修改环境变量；下一次启动的 Java 进程会直接使用新版本。已经运行的 Java、Maven 或 Gradle 守护进程仍需重启，因为其进程环境不能原地更新。

## 命令

```text
jvman install <主版本> [--name <名称>] [--sha256 <固定校验值>] [--source <下载源>]
jvman install <名称> --archive <本地文件> [--sha256 <校验值>]
jvman source [--list|--reset|<下载源>]
jvman source add <名称> <HTTPS模板>
jvman source remove <名称>
jvman add <名称> <JDK_HOME>
jvman discover [--register]
jvman use <名称>
jvman list
jvman current
jvman which [名称]
jvman remove <名称>
jvman uninstall
jvman exec <名称> [--] <命令> [参数...]
jvman init [powershell|cmd|sh]
jvman doctor
jvman update [--check] [--version <版本>]
jvman language [--list|en|zh-CN]
jvman home
```

别名：`ls` 等同于 `list`，`default` 等同于 `use`。为保持兼容，
`jvman uninstall <名称>` 仍等同于 `jvman remove <名称>`；不带名称时，
`jvman uninstall` 会启动 Windows 已注册的卸载程序。

`jvman language` 显示当前界面语言。Windows 上可用 `jvman language en` 或
`jvman language zh-CN` 保存当前用户偏好；`JVMAN_LANG=en` 和
`JVMAN_LANG=zh-CN` 会仅对当前进程覆盖安装器记录与已保存偏好。其他平台支持该
环境变量和 `--list`，但不会写入持久偏好。

`jvman source` 显示当前模式。内置源包括 `tsinghua`（清华 TUNA Adoptium 镜像）、
`huawei`（毕昇 JDK）、`aliyun`（Dragonwell）、`adoptium` 和 `foojay`；`auto`
会测试全部可用的内置源与自定义源。`jvman source <下载源>` 持久保存选择，
`--reset` 恢复自动选源，安装命令的 `--source` 只覆盖本次下载。

自定义源必须返回与 Adoptium 兼容的 JSON，其中包含 HTTPS 安装包地址与 SHA-256。
使用 `jvman source add <名称> <HTTPS模板>` 添加，模板支持 `{major}`、`{os}`、
`{arch}`、`{archive}`，且必须包含 `{major}`，禁止 URL 内嵌账号或密码。配置保存在
`<数据目录>/sources/<名称>.conf` 并使用私有文件权限。删除正在使用的自定义源前必须
先切换到其他源；源 URL 中禁止放置 API 密钥、Token 等敏感信息。

`auto` 模式会先解析各源的元数据，再从最终安装包地址读取有界的 64 KiB 区间样本；
实际安装包主机不可达的源会被忽略，并只从测速最快的成功源完整下载一次 JDK。
探测过程只允许 HTTPS 重定向，且不会下载完整的样本归档。元数据和安装包地址必须
使用 HTTPS，且所有远程安装包都必须提供并通过 SHA-256 校验。

常用示例：

```powershell
jvman exec 17 -- java -version
jvman exec 8 -- mvn test
jvman install company-jdk --archive .\jdk.zip --sha256 <64位十六进制值>
```

Windows 下 `exec` 支持从 `PATH/PATHEXT` 解析 `.exe`、`.com`、`.cmd` 和 `.bat`，因此 Maven 的 `mvn.cmd` 可以直接使用。

## 更新 jvman

```text
jvman update --check
jvman update
jvman update --version 0.3.0
```

不指定 `--version` 时，命令会检查 `github.com/standtrain/jvman` 的最新稳定
Release。发现较新版本时，`--check` 会验证对应平台的校验项，但不会下载或
替换可执行文件。显式版本会跳过最新 Release 元数据查询，且必须是
`MAJOR.MINOR.PATCH`，可带前导 `v`；不允许降级。

更新器只会选择固定名称的 Windows x86_64、静态 Linux x86_64 或 macOS 11+
x86_64/aarch64 产物。下载仅使用 HTTPS，并限制元数据和二进制大小；二进制
必须匹配 Release 中的 `SHA256SUMS`，且 PE、ELF 或 Mach-O 格式及架构必须
与当前平台一致。Linux 和 macOS 要求 curl 7.20.2 或更高版本位于由 root
拥有的固定系统目录中，仅存在于自定义 `PATH` 的 `curl` 会被忽略；macOS 11
自带的系统 curl 满足要求。临时文件使用受限权限，正常完成的成功或失败路径
会尽力清理；Windows 无法立即自删时，helper 可能保留在用户临时目录中直到
下次重启，如果连延迟删除也不可用则可能需要手动清理。该校验可以发现下载
损坏或产物不匹配，但校验清单与二进制位于同一 GitHub Release，因此不等同
于独立发布签名。

Windows 会在当前进程退出后由受限助手完成替换。为避免把用户可写临时目录
变成权限边界，自更新必须从非管理员终端运行；需要管理员权限时，请使用
安装程序或手动替换二进制，之后可运行 `jvman version` 确认。Linux 和 macOS
会在命令返回前完成原子替换。当前用户必须能写入可执行文件所在目录。更新只
替换正在运行的 jvman 命令行，不会修改
`JVMAN_HOME`、已安装 JDK、`PATH`、`JAVA_HOME`、shell profile 或 Windows
安装器状态。

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
  source.conf        当前远程下载源模式（缺省为 auto）
  sources/*.conf     自定义 Adoptium 兼容下载源
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

`0.2.1` 会在国际源、清华、华为、阿里云及用户自定义目录之间自动测速选源，远程安装只接受 Java 主版本。本地发现可以识别常见 JDK 厂商，但托管 JRE、EA/GraalVM、复杂版本范围、项目级 `.java-version`、独立签名发布清单，以及 CLI 自动修改持久环境设置暂不实现。

远程包必须匹配所选下载源返回的 SHA-256；用户传入 `--sha256` 时还会再校验用户固定值。该校验可以发现下载损坏或包不匹配，但不等价于独立的发布签名验证。

解压依赖操作系统自带的 `tar`。远程归档通过内置 HTTPS 下载源获取并经过校验；本地归档默认视为可信输入，传入 `--sha256` 可固定其内容。安装被强制终止时可能遗留 cache/staging 临时文件，可在确认没有安装进程运行后手动清理。

Windows 构建面向 1903 及以上版本，并嵌入 UTF-8 active-code-page manifest，因此中文、emoji、空格路径可以贯穿命令行、JDK 注册和 junction 操作。
