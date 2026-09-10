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

#ifdef __cplusplus
}
#endif

#endif /* CMQ_INFO_H */
