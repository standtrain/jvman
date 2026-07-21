#include "manager.h"
#include "i18n.h"
#include "platform.h"
#include "update.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    JvmanContext context;
    int update_helper_handled = 0;
    jvman_i18n_init();
    int update_helper_result = platform_handle_update_helper(
        argc, argv, &update_helper_handled);
    if (update_helper_handled) return update_helper_result;
    if (update_helper_result != 0) {
        jvman_i18n_fprintf(
            stderr, "jvman: %s: %s\n",
            jvman_i18n_text("cannot inspect the update helper command line"),
            platform_last_error());
        return update_helper_result;
    }
    if (argc >= 2 && argv && argv[1] && strcmp(argv[1], "update") == 0) {
        return jvman_update_run_cli(argc, argv);
    }
    if (argc == 2 && argv && argv[1] && strcmp(argv[1], "uninstall") == 0) {
        return jvman_uninstall_run_cli();
    }
    if (jvman_context_init(&context) != 0) {
        jvman_i18n_fprintf(stderr, "jvman: %s: %s\n",
                           jvman_i18n_text("cannot initialize paths"),
                           platform_last_error());
        return 1;
    }
    return jvman_run(&context, argc, argv);
}
