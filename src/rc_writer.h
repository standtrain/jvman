#ifndef JVMAN_RC_WRITER_H
#define JVMAN_RC_WRITER_H

#include <stddef.h>

/*
 * POSIX shell 启动配置文件的幂等块写入。
 * marker 起止之间会被视为 jvman 管理段，二次写入直接替换；不存在时追加。
 * 所有函数返回 0 表示成功，-1 表示失败（错误消息通过 platform_last_error）。
 */

/* 起始/结束标记，写入时会占独立行，检测时按整行完全匹配。 */
#define JVMAN_RC_MARKER_BEGIN "# >>> jvman initialize >>>"
#define JVMAN_RC_MARKER_END "# <<< jvman initialize <<<"

/* 幂等写入指定 rc 文件；已包含相同 payload 时不做磁盘写。
 * rc_path：目标绝对路径。文件不存在时会以 0644 创建。
 * payload：marker 之间要写入的内容，不含 marker，不必以换行结尾。
 */
int jvman_rc_writer_apply(const char *rc_path, const char *payload);

/* 移除 rc 文件里 jvman 的托管块；文件不存在或无该块时视为成功。 */
int jvman_rc_writer_remove(const char *rc_path);

/* 探测：文件里是否已存在 jvman 管理块。out_present 会被置 0/1。 */
int jvman_rc_writer_probe(const char *rc_path, int *out_present);

#endif
