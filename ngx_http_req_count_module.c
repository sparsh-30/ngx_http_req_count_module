#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

static ngx_command_t ngx_req_count_commands[] = {
    ngx_null_command
};

static ngx_http_module_t ngx_req_count_module_ctx = {
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

ngx_module_t ngx_req_count_module = {
    NGX_MODULE_V1,
    &ngx_req_count_module_ctx,
    ngx_req_count_commands,
    NGX_HTTP_MODULE,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NGX_MODULE_V1_PADDING
};
