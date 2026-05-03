#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

typedef struct {
    ngx_atomic_uint_t count;
    ngx_atomic_uint_t prev_count;
    ngx_int_t freq_ms;
    ngx_str_t zone_name;
} ngx_http_req_count_shm_ctx;

typedef struct {
    ngx_array_t *count_zones;
    ngx_shm_zone_t  *get_zone;
} ngx_http_req_count_conf_t;

static char *ngx_http_req_count_zone(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_http_req_count_init_zone(ngx_shm_zone_t *shm_zone, void *data);
static char *ngx_http_req_count(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char * ngx_http_req_count_get(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_http_req_count_get_handler(ngx_http_request_t *r);
static void *ngx_http_req_count_create_conf(ngx_conf_t *cf);
static char *ngx_http_req_count_merge_conf(ngx_conf_t *cf, void *parent, void *child);
static ngx_int_t ngx_http_req_count_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_req_count_init(ngx_conf_t *cf);

static ngx_command_t ngx_http_req_count_commands[] = {
    { ngx_string("count_zone"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE2,
      ngx_http_req_count_zone,
      0,
      0,
      NULL },

      { ngx_string("count_req"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_req_count,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

      { ngx_string("count_get"),
      NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
      ngx_http_req_count_get,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },
      
    ngx_null_command
};

static ngx_http_module_t ngx_http_req_count_module_ctx = {
    NULL,
    ngx_http_req_count_init,
    NULL,
    NULL,
    NULL,
    NULL,
    ngx_http_req_count_create_conf,
    ngx_http_req_count_merge_conf
};

ngx_module_t ngx_http_req_count_module = {
    NGX_MODULE_V1,
    &ngx_http_req_count_module_ctx,
    ngx_http_req_count_commands,
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

static char *
ngx_http_req_count_zone(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t                         *value;
    ngx_str_t                          name = ngx_null_string;
    ngx_str_t                          freq = ngx_null_string;

    ngx_shm_zone_t                    *shm_zone;
    ngx_http_req_count_shm_ctx        *ctx;

    ssize_t                            rate = 0;
    ngx_int_t                          freq_ms = 0;

    value = cf->args->elts;

    // Parse arguments: name=abc freq=5/s
    for (ngx_uint_t i = 1; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "name=", 5) == 0) {
            name.len = value[i].len - 5;
            name.data = value[i].data + 5;
            continue;
        }

        if (ngx_strncmp(value[i].data, "freq=", 5) == 0) {
            freq.len = value[i].len - 5;
            freq.data = value[i].data + 5;
            continue;
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid parameter \"%V\"", &value[i]);
        return NGX_CONF_ERROR;
    }

    if (name.len == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "count_zone: \"name\" is required");
        return NGX_CONF_ERROR;
    }

    if (freq.len == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "count_zone: \"freq\" is required");
        return NGX_CONF_ERROR;
    }

    // ---- Parse freq like "5/s", "10/m"
    u_char *p = freq.data;
    u_char *last = freq.data + freq.len;

    u_char *slash = ngx_strlchr(p, last, '/');
    if (slash == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid freq format \"%V\"", &freq);
        return NGX_CONF_ERROR;
    }

    rate = ngx_atoi(p, slash - p);
    if (rate <= 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid rate in \"%V\"", &freq);
        return NGX_CONF_ERROR;
    }

    ngx_str_t unit;
    unit.data = slash + 1;
    unit.len = last - (slash + 1);

    if (unit.len == 1 && unit.data[0] == 's') {
        freq_ms = 1000 / rate;
    } else if (unit.len == 1 && unit.data[0] == 'm') {
        freq_ms = 60000 / rate;
    } else if (unit.len == 1 && unit.data[0] == 'h') {
        freq_ms = 3600000 / rate;
    } else {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid time unit in \"%V\"", &freq);
        return NGX_CONF_ERROR;
    }

    // ---- Create context
    ctx = ngx_pcalloc(cf->pool, sizeof(ngx_http_req_count_shm_ctx));
    if (ctx == NULL) {
        return NGX_CONF_ERROR;
    }

    ctx->freq_ms = freq_ms;
    ctx->zone_name = name;

    // ---- Create shared memory zone
    shm_zone = ngx_shared_memory_add(cf, &name, ngx_pagesize,
                                 &ngx_http_req_count_module);
    if (shm_zone == NULL) {
        return NGX_CONF_ERROR;
    }

    if (shm_zone->data) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "duplicate count_zone \"%V\"", &name);
        return NGX_CONF_ERROR;
    }

    shm_zone->init = ngx_http_req_count_init_zone;
    shm_zone->data = ctx;

    return NGX_CONF_OK;
}

static ngx_int_t
ngx_http_req_count_init_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    ngx_http_req_count_shm_ctx *octx = data;
    ngx_http_req_count_shm_ctx *ctx;
    ngx_http_req_count_shm_ctx *conf_ctx;

    // 🔴 actual shared memory
    ctx = (ngx_http_req_count_shm_ctx *) shm_zone->shm.addr;

    if (octx) {
        // reload case
        ctx->count = 0;
        ctx->prev_count = 0;

        // keep config values in sync
        ctx->freq_ms = octx->freq_ms;
        ctx->zone_name = octx->zone_name;

        shm_zone->data = ctx;
        return NGX_OK;
    }

    // first-time init
    conf_ctx = shm_zone->data;

    ctx->count = 0;
    ctx->prev_count = 0;

    ctx->freq_ms = conf_ctx->freq_ms;
    ctx->zone_name = conf_ctx->zone_name;

    // 🔁 now make shm ctx the active one
    shm_zone->data = ctx;

    return NGX_OK;
}

static char *
ngx_http_req_count(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_req_count_conf_t   *rcf = conf;

    ngx_str_t                   *value;
    ngx_str_t                    name = ngx_null_string;

    ngx_shm_zone_t              *shm_zone;

    value = cf->args->elts;

    // Parse: count_req name=abc;
    for (ngx_uint_t i = 1; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "name=", 5) == 0) {
            name.len = value[i].len - 5;
            name.data = value[i].data + 5;
            continue;
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid parameter \"%V\"", &value[i]);
        return NGX_CONF_ERROR;
    }

    if (name.len == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "count_req: \"name\" is required");
        return NGX_CONF_ERROR;
    }

    // Lookup existing shared memory zone
    shm_zone = ngx_shared_memory_add(cf, &name, 0, &ngx_http_req_count_module);
    if (shm_zone == NULL) {
        return NGX_CONF_ERROR;
    }

    if (shm_zone->data == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "unknown count_zone \"%V\"", &name);
        return NGX_CONF_ERROR;
    }

    // Initialize array if needed
    if (rcf->count_zones == NULL) {
        rcf->count_zones = ngx_array_create(cf->pool, 1,
                                            sizeof(ngx_shm_zone_t *));
        if (rcf->count_zones == NULL) {
            return NGX_CONF_ERROR;
        }
    }

    // Push zone into array
    ngx_shm_zone_t **zone;

    zone = ngx_array_push(rcf->count_zones);
    if (zone == NULL) {
        return NGX_CONF_ERROR;
    }

    *zone = shm_zone;

    return NGX_CONF_OK;
}

static char *
ngx_http_req_count_get(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_req_count_conf_t *rcf = conf;

    ngx_str_t      *value;
    ngx_str_t       name = ngx_null_string;
    ngx_shm_zone_t *shm_zone;

    // ❗ Reject duplicate usage in same block
    if (rcf->get_zone != NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "duplicate \"count_get\" directive");
        return NGX_CONF_ERROR;
    }

    value = cf->args->elts;

    // Parse: count_get name=abc;
    for (ngx_uint_t i = 1; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "name=", 5) == 0) {
            name.len = value[i].len - 5;
            name.data = value[i].data + 5;
            continue;
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid parameter \"%V\"", &value[i]);
        return NGX_CONF_ERROR;
    }

    if (name.len == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "count_get: \"name\" is required");
        return NGX_CONF_ERROR;
    }

    // Lookup existing shared memory zone
    shm_zone = ngx_shared_memory_add(cf, &name, 0,
                                     &ngx_http_req_count_module);
    if (shm_zone == NULL) {
        return NGX_CONF_ERROR;
    }

    if (shm_zone->data == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "unknown count_zone \"%V\"", &name);
        return NGX_CONF_ERROR;
    }

    // Store zone in loc_conf
    rcf->get_zone = shm_zone;

    // Register content handler
    ngx_http_core_loc_conf_t *clcf;
    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);

    clcf->handler = ngx_http_req_count_get_handler;

    return NGX_CONF_OK;
}

static ngx_int_t
ngx_http_req_count_get_handler(ngx_http_request_t *r)
{
    ngx_http_req_count_conf_t  *rcf;
    ngx_http_req_count_shm_ctx *ctx;

    ngx_buf_t    *b;
    ngx_chain_t   out;

    u_char       *p;
    u_char        buffer[NGX_INT64_LEN];

    rcf = ngx_http_get_module_loc_conf(r, ngx_http_req_count_module);

    if (rcf->get_zone == NULL) {
        return NGX_DECLINED;
    }

    ctx = rcf->get_zone->data;

    if (ctx == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    // Convert count to string
    p = ngx_sprintf(buffer, "%uA\n", ctx->count);

    ngx_str_t response;
    response.data = buffer;
    response.len = p - buffer;

    // Set headers
    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = response.len;
    ngx_str_set(&r->headers_out.content_type, "text/plain");

    if (ngx_http_send_header(r) != NGX_OK) {
        return NGX_ERROR;
    }

    // Allocate buffer
    b = ngx_pcalloc(r->pool, sizeof(ngx_buf_t));
    if (b == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    b->pos = response.data;
    b->last = response.data + response.len;
    b->memory = 1;
    b->last_buf = 1;

    out.buf = b;
    out.next = NULL;

    return ngx_http_output_filter(r, &out);
}

static void *
ngx_http_req_count_create_conf(ngx_conf_t *cf)
{
    ngx_http_req_count_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_req_count_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    conf->count_zones = NULL;
    conf->get_zone = NULL;

    return conf;
}

static char *
ngx_http_req_count_merge_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_req_count_conf_t *prev = parent;
    ngx_http_req_count_conf_t *conf = child;

    // Inherit count_req zones
    if (conf->count_zones == NULL) {
        conf->count_zones = prev->count_zones;
    }

    // Inherit count_get zone
    if (conf->get_zone == NULL) {
        conf->get_zone = prev->get_zone;
    }

    return NGX_CONF_OK;
}

static ngx_int_t
ngx_http_req_count_handler(ngx_http_request_t *r)
{
    ngx_http_req_count_conf_t   *rcf;
    ngx_shm_zone_t             **zones;
    ngx_uint_t                   i;

    rcf = ngx_http_get_module_loc_conf(r, ngx_http_req_count_module);

    if (rcf->count_zones == NULL) {
        return NGX_DECLINED;
    }

    zones = rcf->count_zones->elts;

    for (i = 0; i < rcf->count_zones->nelts; i++) {

        ngx_http_req_count_shm_ctx *ctx;

        ctx = zones[i]->data;

        if (ctx == NULL) {
            continue;
        }

        // 🔥 Atomic increment (shared across workers)
        ngx_atomic_fetch_add(&ctx->count, 1);
    }

    return NGX_DECLINED;
}

static ngx_int_t
ngx_http_req_count_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_PREACCESS_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_req_count_handler;

    return NGX_OK;
}