#include "manager.h"
#include "platform.h"

#include <stdio.h>

int main(int argc, char **argv) {
    JvmanContext context;
    if (jvman_context_init(&context) != 0) {
        fprintf(stderr, "jvman: cannot initialize paths: %s\n", platform_last_error());
        return 1;
    }
    return jvman_run(&context, argc, argv);
}
