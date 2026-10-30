/* Shared by the config formats (config.c, json.c); not part of minit's API. */
#ifndef MINIT_CONFIG_INTERNAL_H
#define MINIT_CONFIG_INTERNAL_H

#include "minit.h"

void config_error(const char *path, int lineno, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
/* Each returns the number of errors it reported (0 or 1) */
int config_system_key(const char *path, int lineno, config_t *cfg,
		      const char *key, const char *val);
int config_service_key(const char *path, int lineno, service_t *svc,
		       const char *key, const char *val);
/* NULL, after reporting why, if a service with this name cannot be added */
service_t *config_add_service(const char *path, int lineno, config_t *cfg, const char *name);
int config_check_services(const char *path, const config_t *cfg);  /* number of errors */
int config_parse_json(const char *path, config_t *cfg);  /* cfg is already zeroed */

#endif
