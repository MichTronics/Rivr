#include "rml.h"

#if RIVR_ENABLE_RML

#include <string.h>

rml_stats_t g_rml_stats;

void rml_stats_reset(void)
{
    memset(&g_rml_stats, 0, sizeof(g_rml_stats));
}

#endif /* RIVR_ENABLE_RML */
