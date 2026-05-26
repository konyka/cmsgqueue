#ifndef CMQ_CONFIG_H
#define CMQ_CONFIG_H

#include "cmq.h"

cmq_status_t cmq_config_load(const char *path, cmq_config_t *config);
cmq_status_t cmq_config_validate(const cmq_config_t *config);

#endif
