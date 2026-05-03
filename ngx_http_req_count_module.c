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
} ngx_http_req_count_conf_t;

static char *ngx_http_req_count_zone(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_http_req_count_init_zone(ngx_shm_zone_t *shm_zone, void *data);
static char *ngx_http_req_count(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static void *ngx_http_req_count_create_conf(ngx_conf_t *cf);
static char *ngx_http_req_count_merge_conf(ngx_conf_t *cf, void *parent, void *child);

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
    ngx_null_command
};

static ngx_http_module_t ngx_http_req_count_module_ctx = {
    NULL,
    NULL,
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

static void *
ngx_http_req_count_create_conf(ngx_conf_t *cf)
{
    ngx_http_req_count_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_req_count_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    // Important: initialize to NULL so merge logic works correctly
    conf->count_zones = NULL;

    return conf;
}

static char *
ngx_http_req_count_merge_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_req_count_conf_t *prev = parent;
    ngx_http_req_count_conf_t *conf = child;

    // If child doesn't define zones → inherit from parent
    if (conf->count_zones == NULL) {
        conf->count_zones = prev->count_zones;
    }

    return NGX_CONF_OK;
}