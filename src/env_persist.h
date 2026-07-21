#ifndef JVMAN_ENV_PERSIST_H
#define JVMAN_ENV_PERSIST_H

#include "manager.h"

/*
 * CLI 侧的持久化激活门面。
 *
 * Windows: 调用 installer/environment.c 提供的 API 写入 HKCU\Environment
 * 的 JAVA_HOME 与 Path 首段（均指向 <root>\current），并广播 WM_SETTINGCHANGE。
 * 状态与安装器共享 HKCU\Software\jvman\Installer，便于 jvman-setup 卸载时回滚。
 *
 * POSIX: 追加/更新 $HOME/.bashrc 与 $HOME/.zshrc 的 jvman marker 块，
 * 让新终端自动执行 `eval "$(jvman init sh)"`。
 * 状态记录到 <root>/rc.state，deactivate 时依此清理。
 */

typedef struct JvmanPersistOptions {
    const JvmanContext *ctx;
    int replace_java_home; /* 是否覆盖非本工具管理的旧 JAVA_HOME */
    int quiet;             /* 内嵌调用时抑制正常输出，仅打 warning */
} JvmanPersistOptions;

/*
 * 幂等激活：已 owned 或环境变量 JVMAN_NO_PERSIST=1 时直接返回 0（no-op）。
 * 首次接管前会捕获快照，写入失败时回滚。
 * 返回值：0 成功或跳过；非 0 表示写入失败但已尽量回滚。
 */
int jvman_persist_activate(const JvmanPersistOptions *opts);

/* 复用 metadata / rc.state 回滚。失败时打印错误并返回非 0。 */
int jvman_persist_deactivate(const JvmanContext *ctx);

/* 只读探测：out=1 表示当前 HKCU 或 rc 文件里已经存在 jvman 管理的条目。 */
int jvman_persist_is_owned(const JvmanContext *ctx, int *out);

#endif
