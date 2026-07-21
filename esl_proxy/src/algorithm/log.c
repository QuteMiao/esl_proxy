#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "log.h"

// Shared resources needed for WORKER_LOG only
#if WORKER_LOG
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

#if WORKER_LOG
int g_worker_log = 0;
int g_log_output_mode = LOG_OUTPUT_MODE;  // runtime override

// Per-thread log files - each thread gets its own slot
static FILE *g_log_files[LOG_MAX_THREADS] = {NULL};
static uint64_t g_log_lines[LOG_MAX_THREADS] = {0};
static char g_log_filenames[LOG_MAX_THREADS][256] = {0};
static pthread_t g_thread_ids[LOG_MAX_THREADS] = {0};  // Track which pthread_t owns each slot
static char g_base_filename[256] = {0};
static unsigned int g_next_slot = 0;  // Next available slot for new thread

void log_init(const char *base_filename)
{
    pthread_mutex_lock(&g_log_mutex);
    
    // Store base filename
    strncpy(g_base_filename, base_filename, sizeof(g_base_filename) - 1);
    g_base_filename[sizeof(g_base_filename) - 1] = '\0';
    
    // Initialize output mode from environment or default
    const char *output_env = getenv("LOG_OUTPUT_MODE");
    if (output_env != NULL) {
        g_log_output_mode = atoi(output_env);
    } else {
        g_log_output_mode = LOG_OUTPUT_MODE;
    }
    
    // Reset state
    g_next_slot = 0;
    for (int i = 0; i < LOG_MAX_THREADS; i++) {
        g_thread_ids[i] = 0;
        g_log_files[i] = NULL;
        g_log_lines[i] = 0;
    }
    
    pthread_mutex_unlock(&g_log_mutex);
}

static unsigned int find_or_create_thread_slot(pthread_t tid)
{
    // First, check if this thread already has a slot
    for (unsigned int i = 0; i < g_next_slot; i++) {
        if (g_thread_ids[i] == tid) {
            return i;
        }
    }
    
    // Not found, assign a new slot
    if (g_next_slot >= LOG_MAX_THREADS) {
        fprintf(stderr, "ERROR: Too many threads, max %d\n", LOG_MAX_THREADS);
        return 0;
    }
    
    unsigned int idx = g_next_slot++;
    g_thread_ids[idx] = tid;
    
    // Create new log file for this thread
    snprintf(g_log_filenames[idx], sizeof(g_log_filenames[idx]),
             "log/%s_thread_%u.csv", g_base_filename, idx);
    g_log_files[idx] = fopen(g_log_filenames[idx], "w");
    if (!g_log_files[idx]) {
        perror("Failed to open thread log file");
        g_thread_ids[idx] = 0;
        return 0;
    }
    
    // Write CSV header
    fprintf(g_log_files[idx], "file,line,detail\n");
    g_log_lines[idx] = 0;
    
    return idx;
}

static FILE* get_thread_log_file(void)
{
    pthread_t tid = pthread_self();
    
    pthread_mutex_lock(&g_log_mutex);
    
    unsigned int idx = find_or_create_thread_slot(tid);
    
    pthread_mutex_unlock(&g_log_mutex);
    
    return (idx < LOG_MAX_THREADS) ? g_log_files[idx] : NULL;
}

void log_write(const char *file, int line, const char *fmt, ...)
{
    FILE *log_file = NULL;
    if (g_log_output_mode != 1) {
        log_file = get_thread_log_file();
    }
    
    pthread_mutex_lock(&g_log_mutex);
    
    pthread_t tid = pthread_self();
    
    // Extract just the filename from the full path
    const char *filename = strrchr(file, '/');
    filename = filename ? filename + 1 : file;
    
    va_list args;
    va_start(args, fmt);
    
    // Output to file (mode 0 or 2)
    if (g_log_output_mode != 1 && log_file) {
        fprintf(log_file, "%s,%d,", filename, line);
        vfprintf(log_file, fmt, args);
        fprintf(log_file, "\n");
    }
    
    // Output to stdout (mode 1 or 2)
    if (g_log_output_mode != 0) {
        fprintf(stdout, "[%s:%d] ", filename, line);
        vfprintf(stdout, fmt, args);
        fprintf(stdout, "\n");
    }
    
    // Find current slot to increment line count
    for (unsigned int i = 0; i < g_next_slot; i++) {
        if (g_thread_ids[i] == tid) {
            g_log_lines[i]++;
            break;
        }
    }
    va_end(args);
    
    pthread_mutex_unlock(&g_log_mutex);
}

void log_close(void)
{
    pthread_mutex_lock(&g_log_mutex);
    
    for (int i = 0; i < LOG_MAX_THREADS; i++) {
        if (g_log_files[i]) {
            fclose(g_log_files[i]);
            g_log_files[i] = NULL;
            g_log_lines[i] = 0;
            g_thread_ids[i] = 0;
        }
    }
    g_next_slot = 0;
    
    pthread_mutex_unlock(&g_log_mutex);
}
#endif

#if MAIN_LOG
void main_log_write(int line, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    
    // Output to stdout only
    fprintf(stdout, "[scheduler:%d] ", line);
    vfprintf(stdout, fmt, args);
    fprintf(stdout, "\n");
    
    va_end(args);
}
#endif
