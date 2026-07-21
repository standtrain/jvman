#include "i18n.h"

#include "common.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define JVMAN_LANGUAGE_VALUE_MAX 15u

typedef struct JvmanTranslation {
    const char *english;
    const char *zh_cn;
} JvmanTranslation;

static JvmanLanguage current_language = JVMAN_LANGUAGE_EN;

#if defined(_WIN32)
static UINT original_console_output_cp;
static int console_output_cp_changed;
static int console_output_cp_restore_registered;

static void restore_console_output_cp(void) {
    if (!console_output_cp_changed) return;
    (void)fflush(NULL);
    (void)SetConsoleOutputCP(original_console_output_cp);
    console_output_cp_changed = 0;
}

static void use_utf8_console_output(void) {
    UINT output_cp;
    if (console_output_cp_changed) return;
    output_cp = GetConsoleOutputCP();
    if (output_cp == 0 || output_cp == CP_UTF8) return;
    if (!console_output_cp_restore_registered) {
        original_console_output_cp = output_cp;
        if (atexit(restore_console_output_cp) != 0) {
            original_console_output_cp = 0;
            return;
        }
        console_output_cp_restore_registered = 1;
    }
    if (SetConsoleOutputCP(CP_UTF8)) console_output_cp_changed = 1;
}
#endif

static const JvmanTranslation translations[] = {
    {"jvman " JVMAN_VERSION " - lightweight Java version manager",
     "jvman " JVMAN_VERSION " - 轻量级 Java 版本管理器"},
    {"Usage:", "使用方法："},
    {"  jvman install <major> [--name <name>] [--sha256 <hex>] [--source <name>]",
     "  jvman install <主版本> [--name <名称>] [--sha256 <校验值>] [--source <下载源>]"},
    {"  jvman install <name> --archive <file> [--sha256 <hex>]",
     "  jvman install <名称> --archive <文件> [--sha256 <校验值>]"},
    {"  jvman add <name> <jdk-home>", "  jvman add <名称> <JDK主目录>"},
    {"  jvman use <name>", "  jvman use <名称>"},
    {"  jvman list", "  jvman list"},
    {"  jvman discover [--register]", "  jvman discover [--register]"},
    {"  jvman source [--list|--reset|<name>|add <name> <HTTPS-template>|remove <name>]",
     "  jvman source [--list|--reset|<名称>|add <名称> <HTTPS模板>|remove <名称>]"},
    {"  jvman current", "  jvman current"},
    {"  jvman which [name]", "  jvman which [名称]"},
    {"  jvman remove <name>", "  jvman remove <名称>"},
    {"  jvman uninstall [<name>]", "  jvman uninstall [<名称>]"},
    {"  jvman exec <name> [--] <command> [args...]",
     "  jvman exec <名称> [--] <命令> [参数...]"},
    {"  jvman init [powershell|cmd|sh]", "  jvman init [powershell|cmd|sh]"},
    {"  jvman doctor", "  jvman doctor"},
    {"  jvman update [--check] [--version <version>]",
     "  jvman update [--check] [--version <版本>]"},
    {"  jvman language [--list|en|zh-CN]",
     "  jvman language [--list|en|zh-CN]"},
    {"  jvman home", "  jvman home"},
    {"Languages:", "语言："},
    {"English", "英语"},
    {"Simplified Chinese", "简体中文"},
    {"VERSION", "版本"},
    {"SOURCE", "来源"},
    {"Language set to %s.\n", "语言已设置为 %s。\n"},
    {"Persistent language selection is only supported on Windows; set JVMAN_LANG instead",
     "此平台不支持持久化语言选择，请改用 JVMAN_LANG 环境变量"},
    {"usage: jvman language [--list|en|zh-CN]",
     "用法：jvman language [--list|en|zh-CN]"},
    {"unknown language; use `jvman language --list`",
     "未知语言；请使用 `jvman language --list` 查看可选语言"},
    {"cannot save language preference",
     "无法保存语言偏好"},
    {"jvman: %s\n", "jvman：%s\n"},
    {"jvman: %s: %s\n", "jvman：%s：%s\n"},
    {"jvman: cannot switch current JDK: %s; state rollback also failed\n",
     "jvman：无法切换当前 JDK：%s；状态回滚也失败\n"},
    {"jvman: cannot switch current JDK: %s\n",
     "jvman：无法切换当前 JDK：%s\n"},

    {"invalid version name", "版本名称无效"},
    {"the path is not a JDK home (bin/java and bin/javac are required)",
     "该路径不是 JDK 主目录（必须包含 bin/java 和 bin/javac）"},
    {"a JDK registration cannot point to jvman's current link",
     "JDK 注册项不能指向 jvman 的 current 链接"},
    {"cannot lock state", "无法锁定状态"},
    {"that version name is already registered", "该版本名称已注册"},
    {"cannot save registration", "无法保存注册项"},
    {"registered JDK home no longer exists", "已注册的 JDK 主目录不存在"},
    {"cannot record the selected JDK", "无法记录所选 JDK"},
    {"unknown JDK version", "未知的 JDK 版本"},
    {"registered path is not a valid JDK anymore",
     "已注册路径不再是有效的 JDK"},
    {"registered JDK resolves through jvman's current link",
     "已注册 JDK 通过 jvman 的 current 链接解析"},
    {"cannot prepare data directory", "无法准备数据目录"},
    {"cannot list registrations", "无法列出注册项"},
    {"Java discovery failed", "Java 发现失败"},
    {"out of memory preparing discovery results", "准备发现结果时内存不足"},
    {"cannot compare discovered JDKs with registrations",
     "无法将发现的 JDK 与注册项进行比较"},
    {"cannot load registrations", "无法加载注册项"},
    {"cannot allocate a unique discovered JDK name",
     "无法为发现的 JDK 分配唯一名称"},
    {"a discovered JDK name became occupied while registering",
     "注册期间发现的 JDK 名称已被占用"},
    {"cannot register discovered JDK", "无法注册发现的 JDK"},
    {"no JDK is currently selected", "当前未选择 JDK"},
    {"cannot remove the current JDK; switch to another version first",
     "无法移除当前 JDK；请先切换到其他版本"},
    {"managed registration is corrupt; refusing to delete files",
     "托管注册项已损坏；拒绝删除文件"},
    {"cannot remove a managed JDK used by the current link",
     "无法移除 current 链接正在使用的托管 JDK"},
    {"cannot remove managed JDK files", "无法移除托管 JDK 文件"},
    {"cannot remove registration", "无法移除注册项"},
    {"cannot start jvman uninstaller", "无法启动 jvman 卸载程序"},
    {"exec requires a command", "exec 需要指定命令"},
    {"cannot construct child environment", "无法构造子进程环境"},
    {"cannot execute command", "无法执行命令"},
    {"unsupported shell; use powershell, cmd, or sh",
     "不支持该 shell；请使用 powershell、cmd 或 sh"},

    {"cannot reset download source", "无法重置下载源"},
    {"only an existing custom source can be removed",
     "只能移除已存在的自定义下载源"},
    {"download source configuration is invalid", "下载源配置无效"},
    {"select another source before removing the active custom source",
     "移除当前自定义下载源前，请先选择其他下载源"},
    {"cannot remove custom download source", "无法移除自定义下载源"},
    {"custom download source configuration is invalid", "自定义下载源配置无效"},
    {"custom source name is invalid or already exists",
     "自定义下载源名称无效或已存在"},
    {"custom download source limit reached",
     "自定义下载源数量已达上限"},
    {"custom source URL must be HTTPS and include {major}; supported placeholders are {major}, {os}, {arch}, and {archive}",
     "自定义下载源 URL 必须使用 HTTPS 并包含 {major}；支持的占位符为 {major}、{os}、{arch} 和 {archive}"},
    {"custom source configuration is too large", "自定义下载源配置过大"},
    {"cannot save custom download source", "无法保存自定义下载源"},
    {"unknown download source; use `jvman source --list`",
     "未知下载源；请使用 `jvman source --list` 查看可选下载源"},
    {"cannot save download source", "无法保存下载源"},

    {"invalid version", "版本无效"},
    {"invalid --name value", "--name 的值无效"},
    {"remote install currently accepts a Java major version, for example 17 or 21",
     "远程安装当前只接受 Java 主版本号，例如 17 或 21"},
    {"remote install is not supported on this operating system or CPU architecture",
     "当前操作系统或 CPU 架构不支持远程安装"},
    {"--sha256 must contain 64 hexadecimal characters",
     "--sha256 必须包含 64 个十六进制字符"},
    {"--source is only valid for remote installs", "--source 仅适用于远程安装"},
    {"managed install directory already exists", "托管安装目录已存在"},
    {"installation path is too long", "安装路径过长"},
    {"local archive does not exist", "本地压缩包不存在"},
    {"local archive conflicts with the temporary cache path",
     "本地压缩包与临时缓存路径冲突"},
    {"cannot clear stale staging directory", "无法清理旧的暂存目录"},
    {"cannot create staging directory", "无法创建暂存目录"},
    {"could not resolve a Temurin package from any available source",
     "无法从任何可用下载源解析 Temurin 软件包"},
    {"could not resolve a Temurin package from the selected source",
     "无法从所选下载源解析 Temurin 软件包"},
    {"download failed", "下载失败"},
    {"cannot snapshot local archive", "无法复制本地压缩包快照"},
    {"archive SHA-256 verification failed", "压缩包 SHA-256 校验失败"},
    {"archive does not match the SHA-256 pinned on the command line",
     "压缩包与命令行指定的 SHA-256 不匹配"},
    {"archive extraction failed", "压缩包解压失败"},
    {"archive must contain one top-level JDK directory",
     "压缩包必须只包含一个顶层 JDK 目录"},
    {"archive does not contain a valid JDK", "压缩包不包含有效的 JDK"},
    {"invalid JDK home inside archive", "压缩包内的 JDK 主目录无效"},
    {"cannot commit installed JDK", "无法提交已安装的 JDK"},
    {"cannot canonicalize installed JDK home", "无法规范化已安装的 JDK 主目录"},
    {"cannot save installed JDK registration", "无法保存已安装 JDK 的注册项"},
    {"install requires a version", "install 需要指定版本"},
    {"unknown or incomplete install option", "未知或不完整的安装选项"},

    {"usage: jvman add <name> <jdk-home>", "用法：jvman add <名称> <JDK主目录>"},
    {"usage: jvman use <name>", "用法：jvman use <名称>"},
    {"usage: jvman discover [--register]", "用法：jvman discover [--register]"},
    {"usage: jvman which [name]", "用法：jvman which [名称]"},
    {"usage: jvman remove <name>", "用法：jvman remove <名称>"},
    {"usage: jvman uninstall [<name>]",
     "用法：jvman uninstall [<名称>]"},
    {"usage: jvman source [--list|--reset|<name>|add <name> <HTTPS-template>|remove <name>]",
     "用法：jvman source [--list|--reset|<名称>|add <名称> <HTTPS模板>|remove <名称>]"},
    {"usage: jvman exec <name> [--] <command> [args...]",
     "用法：jvman exec <名称> [--] <命令> [参数...]"},
    {"usage: jvman init [powershell|cmd|sh]",
     "用法：jvman init [powershell|cmd|sh]"},
    {"unknown command", "未知命令"},

    {"Registered %s -> %s\n", "已注册 %s -> %s\n"},
    {"Now using %s (%s)\n", "当前使用 %s（%s）\n"},
    {"The selected Java is not active in this shell yet. Initialize it with one of:\n",
     "当前终端尚未激活所选 Java。请执行以下任一命令进行初始化：\n"},
    {"  CMD: for /f \"delims=\" %%L in ('jvman init cmd') do @call %%L\n",
     "  CMD: for /f \"delims=\" %%L in ('jvman init cmd') do @call %%L\n"},
    {"  PowerShell: jvman init powershell | Invoke-Expression\n",
     "  PowerShell: jvman init powershell | Invoke-Expression\n"},
    {"The selected Java is not active in this shell yet. Initialize it with:\n",
     "当前终端尚未激活所选 Java。请执行以下命令进行初始化：\n"},
    {"  sh: eval \"$(jvman init sh)\"\n",
     "  sh: eval \"$(jvman init sh)\"\n"},
    {"  (no JDKs registered)\n", "  （未注册 JDK）\n"},
    {"TYPE    VERSION          VENDOR         NAME                     STATUS                   SOURCES                  JAVA_HOME",
     "类型    版本             厂商           名称                     状态                     来源                     JAVA_HOME"},
    {"(no Java installations discovered)", "（未发现 Java 安装）"},
    {"Summary: %lu new, %lu registered, %lu JRE, %lu invalid\n",
     "汇总：%lu 个新增，%lu 个已注册，%lu 个 JRE，%lu 个无效\n"},
    {"jvman: discovered JDK disappeared or became invalid: %s\n",
     "jvman：发现的 JDK 已消失或变为无效：%s\n"},
    {"Registered %lu new JDK%s.\n", "已注册 %lu 个新 JDK%s。\n"},
    {"Removed %s%s\n", "已移除 %s%s\n"},
    {"The jvman uninstaller was started.", "jvman 卸载程序已启动。"},
    {" and its managed files", " 及其托管文件"},
    {" registration", " 注册项"},
    {"managed", "托管"},
    {"external", "外部"},
    {" [missing]", " [缺失]"},
    {"new", "新增"},
    {"registered", "已注册"},
    {"jre", "JRE"},
    {"invalid", "无效"},
    {"error", "错误"},
    {"unknown", "未知"},
    {"not set", "未设置"},

    {"[ok]   data home: %s\n", "[正常] 数据目录：%s\n"},
    {"[ok]   current: %s -> %s\n", "[正常] 当前版本：%s -> %s\n"},
    {"[warn] current state and directory link do not match\n",
     "[警告] 当前状态与目录链接不匹配\n"},
    {"[warn] no current JDK selected\n", "[警告] 未选择当前 JDK\n"},
    {"[ok]   JAVA_HOME points to the stable current path\n",
     "[正常] JAVA_HOME 指向稳定的 current 路径\n"},
    {"[warn] JAVA_HOME is %s; expected %s\n",
     "[警告] JAVA_HOME 为 %s；预期为 %s\n"},
    {"[ok]   PATH resolves java through jvman\n",
     "[正常] PATH 通过 jvman 解析 java\n"},
    {"[warn] PATH resolves java to %s\n", "[警告] PATH 将 java 解析为 %s\n"},
    {"[warn] java is not available on PATH\n", "[警告] PATH 中没有可用的 java\n"},
    {"[ok]   downloader: Windows WinHTTP\n", "[正常] 下载器：Windows WinHTTP\n"},
    {"[ok]   extractor: %s\n", "[正常] 解压工具：%s\n"},
    {"[warn] tar.exe is required for installs\n", "[警告] 安装需要 tar.exe\n"},
    {"[ok]   downloader: %s\n", "[正常] 下载器：%s\n"},
    {"[warn] curl is required for remote installs\n", "[警告] 远程安装需要 curl\n"},
    {"[warn] tar is required for installs\n", "[警告] 安装需要 tar\n"},
    {"%d warning%s found. Evaluate `jvman init` in your shell after selecting a JDK.\n",
     "发现 %d 个警告%s。选择 JDK 后，请在 shell 中执行 `jvman init` 的输出。\n"},
    {"No problems found.", "未发现问题。"},

    {"Download source: %s (%s)\n", "下载源：%s（%s）\n"},
    {"Removed custom download source: %s\n", "已移除自定义下载源：%s\n"},
    {"Added custom download source: %s\n", "已添加自定义下载源：%s\n"},
    {"Testing download sources...", "正在测试下载源..."},
    {"  %-10s %llu ms, unavailable (%s)\n", "  %-10s %llu 毫秒，不可用（%s）\n"},
    {"  %-10s %lu ms, unavailable (%s)\n", "  %-10s %lu 毫秒，不可用（%s）\n"},
    {"  %-10s %llu ms\n", "  %-10s %llu 毫秒\n"},
    {"  %-10s %lu ms\n", "  %-10s %lu 毫秒\n"},
    {"Selected download source: %s (%llu ms)\n", "已选择下载源：%s（%llu 毫秒）\n"},
    {"Selected download source: %s (%lu ms)\n", "已选择下载源：%s（%lu 毫秒）\n"},
    {"invalid metadata", "元数据无效"},
    {"Automatic (fastest available)", "自动（选择最快可用源）"},
    {"Custom: %s", "自定义：%s"},
    {"Resolving Temurin %d for %s/%s automatically...\n",
     "正在自动解析 Temurin %d（%s/%s）...\n"},
    {"Resolving Temurin %d for %s/%s via %s...\n",
     "正在解析 Temurin %d（%s/%s），下载源：%s...\n"},
    {"Downloading Temurin %s...\n", "正在下载 Temurin %s...\n"},
    {"Archive checksum verified.", "压缩包校验和验证通过。"},
    {"Extracting JDK...", "正在解压 JDK..."},
    {"Installed %s -> %s\n", "已安装 %s -> %s\n"},
    {"Run `jvman use %s` to activate it.\n", "运行 `jvman use %s` 激活该版本。\n"},

    {"cannot inspect the update helper command line",
     "无法检查更新辅助程序命令行"},
    {"cannot initialize paths", "无法初始化路径"},
    {"self-update is not supported on this platform and architecture",
     "当前平台和架构不支持自更新"},
    {"update version must be MAJOR.MINOR.PATCH", "更新版本必须采用 主版本.次版本.修订号 格式"},
    {"cannot locate the running jvman executable", "无法定位正在运行的 jvman 可执行文件"},
    {"cannot fingerprint the running jvman executable", "无法计算正在运行的 jvman 可执行文件指纹"},
    {"cannot verify the running jvman executable version", "无法验证正在运行的 jvman 可执行文件版本"},
    {"the jvman executable changed after this process started", "jvman 可执行文件在本进程启动后发生了变化"},
    {"cannot read the GitHub release metadata", "无法读取 GitHub 发布元数据"},
    {"the GitHub release metadata is invalid", "GitHub 发布元数据无效"},
    {"cannot compare update versions", "无法比较更新版本"},
    {"jvman: refusing to downgrade from %s to %s\n", "jvman：拒绝从 %s 降级到 %s\n"},
    {"jvman %s is already up to date.\n", "jvman %s 已是最新版本。\n"},
    {"cannot construct the fixed GitHub release URL", "无法构造固定的 GitHub 发布 URL"},
    {"cannot download SHA256SUMS", "无法下载 SHA256SUMS"},
    {"the release checksum manifest is invalid", "发布校验和清单无效"},
    {"Update available: %s -> %s\n", "有可用更新：%s -> %s\n"},
    {"cannot create an update download file", "无法创建更新下载文件"},
    {"Downloading jvman %s...\n", "正在下载 jvman %s...\n"},
    {"cannot download the release binary", "无法下载发布二进制文件"},
    {"downloaded release binary failed SHA-256 verification", "下载的发布二进制文件未通过 SHA-256 校验"},
    {"downloaded release asset is not a valid executable for this platform", "下载的发布资源不是当前平台的有效可执行文件"},
    {"release version marker is too long", "发布版本标记过长"},
    {"cannot verify the downloaded release version", "无法验证下载的发布版本"},
    {"downloaded release binary does not match the requested version", "下载的发布二进制文件与请求版本不匹配"},
    {"cannot stage the update beside the running executable", "无法在当前可执行文件旁暂存更新"},
    {"cannot replace the running jvman executable", "无法替换正在运行的 jvman 可执行文件"},
    {"Update to jvman %s is scheduled and will finish after this process exits.\n",
     "已计划更新到 jvman %s，将在本进程退出后完成。\n"},
    {"Updated jvman %s -> %s.\n", "已更新 jvman %s -> %s。\n"},
    {"usage: jvman update [--check] [--version <version>]",
     "用法：jvman update [--check] [--version <版本>]"},
    {"invalid null update argument", "更新参数不能为 NULL"},
    {"update option --check was specified more than once", "更新选项 --check 被重复指定"},
    {"update option --version requires one version", "更新选项 --version 需要一个版本号"}
};

static int ascii_equal_ignore_case(const char *left, const char *right) {
    unsigned char a;
    unsigned char b;
    if (!left || !right) return 0;
    do {
        a = (unsigned char)*left++;
        b = (unsigned char)*right++;
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
        if (a != b) return 0;
    } while (a != '\0');
    return 1;
}

int jvman_i18n_parse_language(const char *value, JvmanLanguage *language_out) {
    size_t length = 0;
    if (!value || !language_out) return -1;
    while (length <= JVMAN_LANGUAGE_VALUE_MAX && value[length]) ++length;
    if (length == 0 || length > JVMAN_LANGUAGE_VALUE_MAX) return -1;
    if (ascii_equal_ignore_case(value, "en") ||
        ascii_equal_ignore_case(value, "en-US")) {
        *language_out = JVMAN_LANGUAGE_EN;
        return 0;
    }
    if (ascii_equal_ignore_case(value, "zh") ||
        ascii_equal_ignore_case(value, "zh-CN") ||
        ascii_equal_ignore_case(value, "zh_CN")) {
        *language_out = JVMAN_LANGUAGE_ZH_CN;
        return 0;
    }
    return -1;
}

#if defined(_WIN32)
static int registry_language(HKEY root, const wchar_t *key_name,
                             JvmanLanguage *language_out) {
    static const wchar_t value_name[] = L"Language";
    wchar_t value[JVMAN_LANGUAGE_VALUE_MAX + 1u];
    HKEY key = NULL;
    DWORD type = 0;
    DWORD byte_count = 0;
    DWORD actual_count;
    size_t char_count;
    size_t index;
    LSTATUS status;
    int valid = 0;

    if (!key_name || !language_out) return 0;
    status = RegOpenKeyExW(root, key_name, 0, KEY_QUERY_VALUE, &key);
    if (status != ERROR_SUCCESS) return 0;
    status = RegQueryValueExW(key, value_name, NULL, &type, NULL, &byte_count);
    if (status != ERROR_SUCCESS || type != REG_SZ ||
        byte_count < sizeof(wchar_t) || byte_count > sizeof(value) ||
        byte_count % sizeof(wchar_t) != 0) {
        RegCloseKey(key);
        return 0;
    }
    memset(value, 0, sizeof(value));
    actual_count = byte_count;
    status = RegQueryValueExW(key, value_name, NULL, &type,
                              (BYTE *)value, &actual_count);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_SZ ||
        actual_count != byte_count || actual_count % sizeof(wchar_t) != 0) {
        return 0;
    }
    char_count = actual_count / sizeof(wchar_t);
    if (char_count == 0 || value[char_count - 1u] != L'\0') return 0;
    for (index = 0; index + 1u < char_count; ++index) {
        if (value[index] == L'\0') return 0;
    }
    if (wcscmp(value, L"en") == 0) {
        *language_out = JVMAN_LANGUAGE_EN;
        valid = 1;
    } else if (wcscmp(value, L"zh-CN") == 0) {
        *language_out = JVMAN_LANGUAGE_ZH_CN;
        valid = 1;
    }
    return valid;
}
#endif

void jvman_i18n_init(void) {
    const char *override = getenv("JVMAN_LANG");
    JvmanLanguage selected;
    current_language = JVMAN_LANGUAGE_EN;
    if (override && jvman_i18n_parse_language(override, &selected) == 0) {
        current_language = selected;
    }
#if defined(_WIN32)
    else if (registry_language(HKEY_CURRENT_USER,
                               L"Software\\jvman\\Preferences", &selected) ||
             registry_language(HKEY_CURRENT_USER,
                               L"Software\\jvman\\Installer", &selected) ||
             registry_language(HKEY_LOCAL_MACHINE,
                               L"Software\\jvman\\Installer", &selected)) {
        current_language = selected;
    }
    if (current_language == JVMAN_LANGUAGE_ZH_CN) {
        use_utf8_console_output();
    }
#endif
}

JvmanLanguage jvman_i18n_language(void) {
    return current_language;
}

const char *jvman_i18n_language_tag(void) {
    return current_language == JVMAN_LANGUAGE_ZH_CN ? "zh-CN" : "en";
}

int jvman_i18n_set_persistent(JvmanLanguage language) {
    const char *tag;
    if (language != JVMAN_LANGUAGE_EN && language != JVMAN_LANGUAGE_ZH_CN) {
        return -1;
    }
    tag = language == JVMAN_LANGUAGE_ZH_CN ? "zh-CN" : "en";
#if defined(_WIN32)
    {
        static const wchar_t key_name[] = L"Software\\jvman\\Preferences";
        static const wchar_t value_name[] = L"Language";
        const wchar_t *wide_tag = language == JVMAN_LANGUAGE_ZH_CN
                                      ? L"zh-CN" : L"en";
        HKEY key = NULL;
        LSTATUS status = RegCreateKeyExW(
            HKEY_CURRENT_USER, key_name, 0, NULL, REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE, NULL, &key, NULL);
        (void)tag;
        if (status != ERROR_SUCCESS) return -1;
        status = RegSetValueExW(
            key, value_name, 0, REG_SZ, (const BYTE *)wide_tag,
            (DWORD)((wcslen(wide_tag) + 1u) * sizeof(*wide_tag)));
        RegCloseKey(key);
        if (status != ERROR_SUCCESS) return -1;
        current_language = language;
        if (language == JVMAN_LANGUAGE_ZH_CN) {
            use_utf8_console_output();
        }
        return 0;
    }
#else
    (void)tag;
    return -2;
#endif
}

const char *jvman_i18n_text(const char *english) {
    size_t index;
    if (!english || current_language != JVMAN_LANGUAGE_ZH_CN) return english;
    for (index = 0; index < sizeof(translations) / sizeof(translations[0]); ++index) {
        if (strcmp(english, translations[index].english) == 0) {
            return translations[index].zh_cn;
        }
    }
    return english;
}

const char *jvman_i18n_plural_suffix(unsigned long count) {
    if (current_language == JVMAN_LANGUAGE_ZH_CN) return "";
    return count == 1u ? "" : "s";
}

int jvman_i18n_printf(const char *trusted_format, ...) {
    int result;
    va_list arguments;
    if (!trusted_format) return -1;
    va_start(arguments, trusted_format);
    result = vprintf(jvman_i18n_text(trusted_format), arguments);
    va_end(arguments);
    return result;
}

int jvman_i18n_fprintf(FILE *stream, const char *trusted_format, ...) {
    int result;
    va_list arguments;
    if (!stream || !trusted_format) return -1;
    va_start(arguments, trusted_format);
    result = vfprintf(stream, jvman_i18n_text(trusted_format), arguments);
    va_end(arguments);
    return result;
}

int jvman_i18n_puts(const char *text) {
    return puts(jvman_i18n_text(text));
}
