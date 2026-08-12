#ifndef TB_CURL_TRANSPORT_H
#define TB_CURL_TRANSPORT_H
#include "tb.h"
#include <uv.h>

tb_transport *tb_curl_transport_create(uv_loop_t *loop, const tb_config *cfg);

#endif /* TB_CURL_TRANSPORT_H */
