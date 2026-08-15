#pragma once

#include "m45_config.h"

/* Keep these shared so background network tasks reserve capacity for HTTP. */
#define M45_HTTP_MAX_OPEN_SOCKETS 4
#define M45_HTTP_LISTEN_SOCKET_RESERVE 1
/* Primary plus auxiliary sessions, with one transient primary-pool probe. */
#define M45_STRATUM_SOCKET_RESERVE (2 + M45_AUX_POOL_MAX)
