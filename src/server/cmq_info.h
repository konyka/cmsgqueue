#ifndef CMQ_INFO_H
#define CMQ_INFO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* v0.5.174: quote s as a JSON string for the INFO frame.
 * 0 / omitted / empty writes "". Quote, backslash, controls,
 * NULL out, or overflow fail closed. */
int cmq_info_json_str(const char *s, char *out, size_t cap);

/* v0.5.175: quote the INFO host. 0 / omitted / empty is
 * "0.0.0.0". Non-IPv4 fail closed. */
int cmq_info_host_json(const char *host, char *out, size_t cap);

/* v0.5.176: quote the INFO server_id. 0 / omitted / empty is
 * "cmsgsrv". Longer than CMQ_NODE_ID_SIZE-1 fail closed. */
int cmq_info_server_id_json(const char *id, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* CMQ_INFO_H */
