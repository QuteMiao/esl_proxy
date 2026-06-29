/*
 * platform_sync_onboard.c — onboard cache flush/invalidate for shared scheduler state.
 */
#include "platform.h"

#include "conf.h"
#include "onboard_config.h"

#include <stdarg.h>

void platform_main_log_vwrite(int line, const char *fmt, va_list args)
{
    (void)line;
    (void)fmt;
    (void)args;
}
