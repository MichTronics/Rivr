#include "rivr_log.h"

rivr_log_mode_t g_rivr_log_mode = RIVR_LOG_DEBUG;

void rivr_log_set_mode(rivr_log_mode_t mode)
{
    g_rivr_log_mode = mode;
}

rivr_log_mode_t rivr_log_get_mode(void)
{
    return g_rivr_log_mode;
}
