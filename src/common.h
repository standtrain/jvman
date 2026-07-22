#ifndef JVMAN_COMMON_H
#define JVMAN_COMMON_H

#include <stddef.h>

#define JVMAN_VERSION "0.4.0"
#define JVMAN_VERSION_W L"0.4.0"
#define JVMAN_VERSION_NUMERIC 0,4,0,0
#define JVMAN_PATH_MAX 4096
#define JVMAN_NAME_MAX 80
#define JVMAN_LINE_MAX 8192

#if defined(_WIN32)
#define JVMAN_DIR_SEP '\\'
#define JVMAN_DIR_SEP_STR "\\"
#define JVMAN_JAVA_EXE "java.exe"
#define JVMAN_JAVAC_EXE "javac.exe"
#else
#define JVMAN_DIR_SEP '/'
#define JVMAN_DIR_SEP_STR "/"
#define JVMAN_JAVA_EXE "java"
#define JVMAN_JAVAC_EXE "javac"
#endif

typedef struct JvmanRegistration {
    char name[JVMAN_NAME_MAX + 1];
    char home[JVMAN_PATH_MAX];
    int managed;
} JvmanRegistration;

#endif
