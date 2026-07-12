#ifndef CMQ_CONFIG_H
#define CMQ_CONFIG_H

#include "cmq.h"

cmq_status_t cmq_config_load(const char *path, cmq_config_t *config);
cmq_status_t cmq_config_validate(const cmq_config_t *config);
/* Free heap fields allocated by cmq_config_load (and reset pointers).
   Safe on a zeroed config; load() calls this before parsing. */
void cmq_config_free(cmq_config_t *config);

#endif
