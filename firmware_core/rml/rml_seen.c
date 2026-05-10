#include "rml.h"

#if RIVR_ENABLE_RML

#include <string.h>

static uint32_t s_seen[RML_SEEN_CACHE_SIZE];
static uint16_t s_seen_next;

void rml_seen_reset(void)
{
    memset(s_seen, 0, sizeof(s_seen));
    s_seen_next = 0u;
}

bool rml_seen(uint32_t msg_id)
{
    if (msg_id == 0u) {
        return false;
    }
    for (uint16_t i = 0u; i < (uint16_t)RML_SEEN_CACHE_SIZE; i++) {
        if (s_seen[i] == msg_id) {
            return true;
        }
    }
    return false;
}

void rml_mark_seen(uint32_t msg_id)
{
    if (msg_id == 0u || rml_seen(msg_id)) {
        return;
    }
    s_seen[s_seen_next] = msg_id;
    s_seen_next = (uint16_t)((s_seen_next + 1u) % (uint16_t)RML_SEEN_CACHE_SIZE);
}

#endif /* RIVR_ENABLE_RML */
