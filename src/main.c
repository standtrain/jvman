#include "manager.h"
#include "platform.h"
#include "update.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    JvmanContext context;
    int update_helper_handled = 0;
    int update_helper_result = platform_handle_update_helper(
        argc, argv, &update_helper_handled);
    if (update_helper_handled) return update_helper_result;
    if (update_helper_result != 0) {
        fprintf(stderr, "jvman: cannot inspect the update helper command line: %s\n",
                platform_last_error());
        return update_helper_result;
    }
    if (argc >= 2 && argv && argv[1] && strcmp(argv[1], "update") == 0) {
        return jvman_update_run_cli(argc, argv);
    }
    if (jvman_context_init(&context) != 0) {
        fprintf(stderr, "jvman: cannot initialize paths: %s\n", platform_last_error());
        return 1;
    }
    return jvman_run(&context, argc, argv);
}
