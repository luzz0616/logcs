#include "logcliapi.h"

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
)
{
    return 0;
}