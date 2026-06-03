#include "log.h"

#if WORKER_LOG
int g_worker_log = 0;
FILE *g_log_file = NULL;
uint64_t g_log_line = 0;

void log_init(const char *filename)
{
    g_log_file = fopen(filename, "w");
    if (!g_log_file) {
        perror("Failed to open log file");
    } else {
        g_log_line = 0;
        // Write CSV header
        fprintf(g_log_file, "tag,source,log_line,detail\n");
    }
}

void log_close(void)
{
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
}
#endif
