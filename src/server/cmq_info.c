#include "cmq_info.h"
#include "cmq_cluster.h"
#include <arpa/inet.h>
#include <string.h>

int cmq_info_json_str(const char *s, char *out, size_t cap) {
    if (!out || cap < 3)
        return -1;
    if (!s || !s[0]) {
        out[0] = '"';
        out[1] = '"';
        out[2] = '\0';
        return 0;
    }
    size_t n = strlen(s);
    if (n > cap - 3)
        return -1;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20u || c == '"' || c == '\\')
            return -1;
    }
    out[0] = '"';
    memcpy(out + 1, s, n);
    out[1 + n] = '"';
    out[2 + n] = '\0';
    return 0;
}

int cmq_info_host_json(const char *host, char *out, size_t cap) {
    const char *h = (host && host[0]) ? host : "0.0.0.0";
    struct in_addr a;
    if (inet_pton(AF_INET, h, &a) != 1)
        return -1;
    return cmq_info_json_str(h, out, cap);
}

int cmq_info_server_id_json(const char *id, char *out, size_t cap) {
    const char *s = (id && id[0]) ? id : "cmsgsrv";
    if (strnlen(s, CMQ_NODE_ID_SIZE) >= CMQ_NODE_ID_SIZE)
        return -1;
    return cmq_info_json_str(s, out, cap);
}
