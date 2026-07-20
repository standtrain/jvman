#ifndef JVMAN_MANAGER_H
#define JVMAN_MANAGER_H

#include "common.h"

typedef struct JvmanContext {
    char root[JVMAN_PATH_MAX];
    char versions[JVMAN_PATH_MAX];
    char jdks[JVMAN_PATH_MAX];
    char cache[JVMAN_PATH_MAX];
    char staging[JVMAN_PATH_MAX];
    char current_link[JVMAN_PATH_MAX];
    char current_state[JVMAN_PATH_MAX];
    char lock_file[JVMAN_PATH_MAX];
} JvmanContext;

int jvman_context_init(JvmanContext *context);
int jvman_run(JvmanContext *context, int argc, char **argv);

#endif
