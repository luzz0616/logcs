#ifndef LOGCLIAPI_H
#define LOGCLIAPI_H
#include <stddef.h>

// 请求接口，阻塞
int logcli_req(
    const char *host,
    int port,
    int cmd,
    const char *body,
    size_t body_sz,
    char *response,
    size_t response_sz,
    size_t response_actual_sz,
    int timeout_ms        
);


#endif