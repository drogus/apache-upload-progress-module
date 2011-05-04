#include <stdint.h>
#include <ap_config.h>
#include <http_core.h>
#include <http_log.h>
#include <apr_version.h>
#include <apr_pools.h>
#include <apr_strings.h>
#include "unixd.h"

#if APR_HAS_SHARED_MEMORY
#include "apr_rmm.h"
#include "apr_shm.h"
#else
#error "APR_HAS_SHARED_MEMORY required for upload_progress module"
#endif

#if APR_HAVE_UNISTD_H
#include <unistd.h>
#endif

#if (APR_MAJOR_VERSION < 1)
#include "apr_shm_remove.h"
#endif

#ifndef PROGRESS_ID
#  define PROGRESS_ID "X-Progress-ID"
#endif
#ifndef JSON_CB_PARAM
#  define JSON_CB_PARAM "callback"
#endif
#ifndef CACHE_FILENAME
#  define CACHE_FILENAME "/tmp/upload_progress_cache"
#endif
#ifndef UP_DEBUG
#  define UP_DEBUG 0
#endif

#ifndef ARG_ALLOWED_PROGRESSID
#  define ARG_ALLOWED_PROGRESSID "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_:./!{}"
#endif
#ifndef ARG_ALLOWED_JSONPCALLBACK
#  define ARG_ALLOWED_JSONPCALLBACK "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._$"
#endif
#ifndef ARG_MINLEN_PROGRESSID
#  define ARG_MINLEN_PROGRESSID 8
#endif
#ifndef ARG_MAXLEN_PROGRESSID
#  define ARG_MAXLEN_PROGRESSID 128
#endif
#ifndef ARG_MINLEN_JSONPCALLBACK
#  define ARG_MINLEN_JSONPCALLBACK 1
#endif
#ifndef ARG_MAXLEN_JSONPCALLBACK
/* This limit is set by most JS implementations on identifier length */
#  define ARG_MAXLEN_JSONPCALLBACK 64
#endif

#if UP_DEBUG == 1
#  define up_log(...) ap_log_error( __VA_ARGS__ )
#else
#  define up_log(...)
#endif

#define CACHE_LOCK() do {                                  \
    if (config->cache_lock) {                              \
        char errbuf[200];                                  \
        up_log(APLOG_MARK, APLOG_DEBUG, 0, config->server, "CACHE_LOCK()"); \
        apr_status_t status = apr_global_mutex_lock(config->cache_lock);        \
        if (status != APR_SUCCESS) {                          \
            ap_log_error(APLOG_MARK, APLOG_CRIT, status, 0, \
                       "%s", apr_strerror(status, errbuf, sizeof(errbuf))); \
        }                                                  \
    } \
} while (0)

#define CACHE_UNLOCK() do {                                \
    if (config->cache_lock)                               \
    {	\
        up_log(APLOG_MARK, APLOG_DEBUG, 0, config->server, "CACHE_UNLOCK()"); \
        apr_global_mutex_unlock(config->cache_lock);      \
    }	\
} while (0)

typedef struct {
    int track_enabled;
    int report_enabled;
} DirConfig;


typedef struct upload_progress_node_s{
    apr_size_t length;
    apr_size_t received;
    int err_status;
    time_t started_at;
    apr_size_t speed; /* bytes per second */
    time_t expires;
    int done;
    char key[ARG_MAXLEN_PROGRESSID];
} upload_progress_node_t;

typedef struct {
    int count;
    int active;
    upload_progress_node_t *nodes; /* all nodes allocated at once */
    int *list; /* static array of node indexes, list begins with indexes of active nodes */
} upload_progress_cache_t;

typedef struct {
    request_rec *r;
    upload_progress_node_t *node;
} upload_progress_context_t;

typedef struct {
    server_rec *server;
    apr_pool_t *pool;
    apr_global_mutex_t *cache_lock;
    char *lock_file;           /* filename for shm lock mutex */
    apr_size_t cache_bytes;
    apr_shm_t *cache_shm;
    apr_rmm_t *cache_rmm;
    char *cache_file;
    upload_progress_cache_t *cache;
} ServerConfig;

upload_progress_node_t* insert_node(request_rec *r, const char *key);
upload_progress_node_t *store_node(ServerConfig *config, const char *key);
upload_progress_node_t *find_node(request_rec *r, const char *key);
static void clean_old_connections(request_rec *r);
void fill_new_upload_node_data(upload_progress_node_t *node, request_rec *r);
static apr_status_t upload_progress_cleanup(void *data);
const char *get_progress_id(request_rec *, int *);

//from passenger
typedef const char * (*CmdFunc)();// Workaround for some weird C++-specific compiler error.

extern module AP_MODULE_DECLARE_DATA upload_progress_module;

/**/server_rec *global_server = NULL;

inline ServerConfig *get_server_config(server_rec *s) {
    return (ServerConfig*)ap_get_module_config(s->module_config, &upload_progress_module);
}

static int upload_progress_handle_request(request_rec *r)
{
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, r->server, "upload_progress_handle_request()");

    DirConfig* dir = (DirConfig*)ap_get_module_config(r->per_dir_config, &upload_progress_module);
    ServerConfig *config = get_server_config(r->server);

    if (dir && dir->track_enabled > 0) {
        if (r->method_number == M_POST) {

            int param_error;
            const char* id = get_progress_id(r, &param_error);

            if (id) {
                ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                             "Upload Progress: Upload id='%s' in trackable location: %s.", id, r->uri);
                CACHE_LOCK();
                clean_old_connections(r);
                upload_progress_node_t *node = find_node(r, id);
                if (node == NULL) {
                    node = insert_node(r, id);
                    if (node)
                        up_log(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                               "Upload Progress: Added upload with id='%s' to list.", id);
                } else if (node->done) {
                    fill_new_upload_node_data(node, r);
                    up_log(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                                 "Upload Progress: Reused existing node with id='%s'.", id);
                } else {
                    node = NULL;
                    ap_log_error(APLOG_MARK, APLOG_INFO, 0, r->server,
                                 "Upload Progress: Upload with id='%s' already exists, ignoring.", id);
                }

                if (node) {
                    upload_progress_context_t *ctx = (upload_progress_context_t*)apr_pcalloc(r->pool, sizeof(upload_progress_context_t));
                    ctx->node = node;
                    ctx->r = r;
                    apr_pool_cleanup_register(r->pool, ctx, upload_progress_cleanup, apr_pool_cleanup_null);
                    ap_add_input_filter("UPLOAD_PROGRESS", NULL, r, r->connection);
                }
                CACHE_UNLOCK();

            } else if (param_error < 0) {
                ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                             "Upload Progress: Upload with invalid ID in trackable location: %s.", r->uri);
                /*
                return HTTP_BAD_REQUEST;
                return HTTP_NOT_FOUND;
                */

            } else {
                ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                             "Upload Progress: Upload without ID in trackable location: %s.", r->uri);
            }
        }
    }

    return DECLINED;
}

static const char *report_upload_progress_cmd(cmd_parms *cmd, void *config, int arg)
{
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, global_server, "report_upload_progress_cmd()");
    DirConfig *dir = (DirConfig *)config;
    dir->report_enabled = arg ? 1 : -1;
    return NULL;
}

static const char *track_upload_progress_cmd(cmd_parms *cmd, void *config, int arg)
{
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, global_server, "track_upload_progress_cmd()");
    DirConfig *dir = (DirConfig *)config;
    dir->track_enabled = arg ? 1 : -1;
    return NULL;
}

static const char* upload_progress_shared_memory_size_cmd(cmd_parms *cmd,
                                                 void *dummy, const char *arg)
{
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, global_server, "upload_progress_shared_memory_size_cmd()");
    ServerConfig *config = get_server_config(cmd->server);

    long long int n = atoi(arg);

    if (n <= 0) {
        return "UploadProgressSharedMemorySize should be positive";
    }

    config->cache_bytes = (apr_size_t)n;

    return NULL;
}

static void *create_upload_progress_dir_config(apr_pool_t *p, char *dirspec)
{
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, global_server, "upload_progress_config_create_dir()");
    DirConfig *dir = (DirConfig *)apr_pcalloc(p, sizeof(DirConfig));
    dir->track_enabled = 0;
    dir->report_enabled = 0;
    return dir;
}

static void *merge_upload_progress_dir_config(apr_pool_t *p, void *basev, void *overridev)
{
    DirConfig *new = (DirConfig *)apr_pcalloc(p, sizeof(DirConfig));
    DirConfig *override = (DirConfig *)overridev;
    DirConfig *base = (DirConfig *)basev;
    new->track_enabled = (override->track_enabled == 0) ? base->track_enabled :
                             (override->track_enabled > 0 ? 1 : -1);
    new->report_enabled = (override->report_enabled == 0) ? base->report_enabled :
                              (override->report_enabled > 0 ? 1 : -1);
    return new;
}

static void *upload_progress_config_create_server(apr_pool_t *p, server_rec *s)
{
    up_log(APLOG_MARK, APLOG_DEBUG, 0, s, "upload_progress_config_create_server()");
    ServerConfig *config = (ServerConfig *)apr_pcalloc(p, sizeof(ServerConfig));
    config->cache_file = apr_pstrdup(p, CACHE_FILENAME);
    config->cache_bytes = 51200;
    apr_pool_create(&config->pool, p);
    config->server = s;
    return config;
}

int read_request_status(request_rec *r)
{
    int status;

    if (r) {
        /* error status rendered in status line is preferred because passenger
           clobbers request_rec->status when exception occurs */
        /* FIXME: Shouldn't we read r->status instead as it's already preparsed? */
        status = r->status_line ? atoi(r->status_line) : 0;
        if (!ap_is_HTTP_VALID_RESPONSE(status))
            status = r->status;
        return status;
    } else {
        return 0;
    }
}

static int track_upload_progress(ap_filter_t *f, apr_bucket_brigade *bb,
                           ap_input_mode_t mode, apr_read_type_e block,
                           apr_off_t readbytes)
{
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, global_server, "track_upload_progress()");
    apr_status_t rv;
    upload_progress_node_t *node;
    ServerConfig* config = get_server_config(f->r->server);

    rv = ap_get_brigade(f->next, bb, mode, block, readbytes);

    int param_error;
    const char* id = get_progress_id(f->r, &param_error);

    if (id == NULL) return rv;

    CACHE_LOCK();
    node = find_node(f->r, id);
    if (node) {
        time_t t = time(NULL);
        node->expires = t + 60;
        if (rv == APR_SUCCESS) {
            apr_off_t length;
            apr_brigade_length(bb, 1, &length);
            node->received += (apr_size_t)length;
            if (node->received > node->length) /* handle chunked tranfer */
                node->length = node->received;
            time_t upload_time = t - node->started_at;
            if (upload_time > 0) {
                node->speed = (apr_size_t)(node->received / upload_time);
            }
        } else {
            node->err_status = read_request_status(f->r);
        }
    }
    CACHE_UNLOCK();

    return rv;
}

int check_request_argument(const char *value, int len, char *allowed, int minlen, int maxlen) {
    /* Check the length of the argument */
    if (len > maxlen) return -1;
    if (len < minlen) return -2;
    /* If no whitelist given, assume everything whitelisted */
    if (!allowed) return 0;
    /* Check each char to be in the whitelist */
    if (strspn(value, allowed) < len) return -3;
    return 0;
}

char *get_param_value(char *p, const char *param_name, int *len) {
    char pn1[3] = {toupper(param_name[0]), tolower(param_name[0]), 0};
    int pn_len = strlen(param_name);
    char *val_end;
    static char *param_sep = "&";

    while (p) {
        if ((strncasecmp(p, param_name, pn_len) == 0) && (p[pn_len] == '='))
            break;
        if (*p) p++;
        p = strpbrk(p, pn1);
    }
    if (p) {
        p += (pn_len + 1);
        *len = strcspn(p, param_sep);
    }
    return p;
}

const char *get_progress_id(request_rec *r, int *param_error) {
    int len;

    /* try to find progress id in http headers */
    const char *id  = apr_table_get(r->headers_in, PROGRESS_ID);
    if (id) {
        *param_error = check_request_argument(id, strlen(id), ARG_ALLOWED_PROGRESSID,
            ARG_MINLEN_PROGRESSID, ARG_MAXLEN_PROGRESSID);
        if (*param_error) return NULL;
        return id;
    }

    /* if progress id not found in headers, check request args (query string) */
    id = get_param_value(r->args, PROGRESS_ID, &len);
    if (id) {
        *param_error = check_request_argument(id, len, ARG_ALLOWED_PROGRESSID,
            ARG_MINLEN_PROGRESSID, ARG_MAXLEN_PROGRESSID);
        if (*param_error) return NULL;
        return apr_pstrndup(r->connection->pool, id, len);
    }

    *param_error = 1; /* not found */
    return NULL;
}

const char *get_json_callback_param(request_rec *r, int *param_error) {
    char *val;
    int len;

    val = get_param_value(r->args, JSON_CB_PARAM, &len);
    if (val) {
        *param_error = check_request_argument(val, len, ARG_ALLOWED_JSONPCALLBACK,
            ARG_MINLEN_JSONPCALLBACK, ARG_MAXLEN_JSONPCALLBACK);
        if (*param_error) return NULL;
        return apr_pstrndup(r->connection->pool, val, len);
    }

    *param_error = 1; /* not found */
    return NULL;
}

inline int check_node(upload_progress_node_t *node, const char *key) {
    return strncasecmp(node->key, key, ARG_MAXLEN_PROGRESSID) == 0 ? 1 : 0;
}

void fill_new_upload_node_data(upload_progress_node_t *node, request_rec *r) {
    const char *content_length;
    time_t t = time(NULL);

    node->received = 0;
    node->done = 0;
    node->err_status = 0;
    node->started_at = t;
    node->speed = 0;
    node->expires = t + 60;
    content_length = apr_table_get(r->headers_in, "Content-Length");
    node->length = 1;
    /* Content-Length is missing is case of chunked transfer encoding */
    if (content_length)
        sscanf(content_length, "%d", &(node->length));
}

upload_progress_node_t* insert_node(request_rec *r, const char *key) {
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, r->server, "insert_node()");

    ServerConfig *config = get_server_config(r->server);
    upload_progress_cache_t *cache = config->cache;
    upload_progress_node_t *node;

    if (cache->active == cache->count) {
        ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, r->server, "Cache full");
        return NULL;
    }
    node = &cache->nodes[cache->list[cache->active]];
    cache->active += 1;

    strncpy(node->key, key, ARG_MAXLEN_PROGRESSID);
    fill_new_upload_node_data(node, r);

    return node;
}

upload_progress_node_t *find_node(request_rec *r, const char *key) {
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, r->server, "find_node()");

    ServerConfig *config = get_server_config(r->server);
    upload_progress_cache_t *cache = config->cache;
    upload_progress_node_t *node, *nodes = cache->nodes;
    int *list = cache->list;
    int active = cache->active;
    int i;

    for (i = 0; i < active; i++) {
        node = &nodes[list[i]];
        if (check_node(node, key))
          return node;
    }
    return NULL;
}

static apr_status_t upload_progress_cleanup(void *data)
{
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, global_server, "upload_progress_cleanup()");
//    ServerConfig *config = get_server_config(global_server);

    /* this function should use locking because it modifies node data */
//    CACHE_LOCK();

    upload_progress_context_t *ctx = (upload_progress_context_t *)data;

    if (ctx && ctx->node) {
        ctx->node->err_status = read_request_status(ctx->r);
        ctx->node->expires = time(NULL) + 60; /* expires in 60s */
        ctx->node->done = 1;
    }

//    CACHE_LOCK();

    return APR_SUCCESS;
}

static void clean_old_connections(request_rec *r) {
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, r->server, "clean_old_connections()");

    ServerConfig *config = get_server_config(r->server);
    upload_progress_cache_t *cache = config->cache;
    upload_progress_node_t *node, *nodes = cache->nodes;
    int *list = cache->list;
    int i, tmp;
    time_t t = time(NULL);

    for (i = 0; i < cache->active; i++) {
        node = &nodes[list[i]];
        if (t > node->expires) {
            cache->active -= 1;
            tmp = list[cache->active];
            list[cache->active] = list[i];
            list[i] = tmp;
            i--;
        }
    }
}

void *rmm_calloc(apr_rmm_t *rmm, apr_size_t reqsize)
{
    apr_rmm_off_t block = apr_rmm_calloc(rmm, reqsize);
    return block ? apr_rmm_addr_get(rmm, block) : NULL;
}

apr_status_t upload_progress_cache_init(apr_pool_t *pool, ServerConfig *config)
{
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, global_server, "upload_progress_cache_init()");

    apr_status_t result;
    apr_size_t size;
    upload_progress_cache_t *cache;
    int nodes_cnt, i;

    if (config->cache_file) {
        /* Remove any existing shm segment with this name. */
        apr_shm_remove(config->cache_file, config->pool);
    }

    size = APR_ALIGN_DEFAULT(config->cache_bytes);
    result = apr_shm_create(&config->cache_shm, size, config->cache_file, config->pool);
    if (result != APR_SUCCESS) {
        return result;
    }

    /* Determine the usable size of the shm segment. */
    size = apr_shm_size_get(config->cache_shm);

    /* This will create a rmm "handler" to get into the shared memory area */
    result = apr_rmm_init(&config->cache_rmm, NULL,
                          apr_shm_baseaddr_get(config->cache_shm), size,
                          config->pool);
    if (result != APR_SUCCESS) {
        return result;
    }

    /* init cache object */
    cache = (upload_progress_cache_t *)rmm_calloc(config->cache_rmm,
                                        sizeof(upload_progress_cache_t));
    if (!cache) return APR_ENOMEM;

    config->cache = cache;
    nodes_cnt = ((size - sizeof(upload_progress_cache_t)) /
                (sizeof(upload_progress_node_t) + sizeof(int))) - 1;
    cache->count = nodes_cnt;
    cache->active = 0;

    cache->list = (int *)rmm_calloc(config->cache_rmm, nodes_cnt * sizeof(int));
    if (!cache->list) return APR_ENOMEM;
    for (i = 0; i < nodes_cnt; i++) cache->list[i] = i;

    cache->nodes = (upload_progress_node_t *)rmm_calloc(config->cache_rmm,
                       nodes_cnt * sizeof(upload_progress_node_t));
    if (!cache->nodes) return APR_ENOMEM;

    ap_log_error(APLOG_MARK, APLOG_INFO, 0, global_server,
                 "Upload Progress: monitoring max %i simultaneous uploads, id (%s) length %i..%i",
                 nodes_cnt, PROGRESS_ID, ARG_MINLEN_PROGRESSID, ARG_MAXLEN_PROGRESSID);

    return APR_SUCCESS;
}

int upload_progress_init(apr_pool_t *p, apr_pool_t *plog,
                    apr_pool_t *ptemp,
                    server_rec *s) {
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, s, "upload_progress_init()");
/**/global_server = s;

    apr_status_t result;
    server_rec *s_vhost;
    ServerConfig *st_vhost;

    ServerConfig *config = get_server_config(s);

    void *data;
    const char *userdata_key = "upload_progress_init";

    /* upload_progress_init will be called twice. Don't bother
     * going through all of the initialization on the first call
     * because it will just be thrown away.*/
    apr_pool_userdata_get(&data, userdata_key, s->process->pool);
    if (!data) {
        apr_pool_userdata_set((const void *)1, userdata_key,
                               apr_pool_cleanup_null, s->process->pool);

        /* If the cache file already exists then delete it.  Otherwise we are
         * going to run into problems creating the shared memory. */
        if (config->cache_file) {
            char *lck_file = apr_pstrcat(ptemp, config->cache_file, ".lck",
                                         NULL);
            apr_file_remove(lck_file, ptemp);
        }
        return OK;
    }

    /* initializing cache if shared memory size is not zero and we already
     * don't have shm address
     */
    if (!config->cache_shm && config->cache_bytes > 0) {
        result = upload_progress_cache_init(p, config);
        if (result != APR_SUCCESS) {
            ap_log_error(APLOG_MARK, APLOG_ERR, result, s,
                         "Upload Progress cache: could not create shared memory segment");
            return DONE;
        }

        if (config->cache_file) {
            config->lock_file = apr_pstrcat(config->pool, config->cache_file, ".lck",
                                        NULL);
        }

        result = apr_global_mutex_create(&config->cache_lock,
                                         config->lock_file, APR_LOCK_DEFAULT,
                                         config->pool);
        if (result != APR_SUCCESS) {
            return result;
        }

#ifdef AP_NEED_SET_MUTEX_PERMS
        result = unixd_set_global_mutex_perms(config->cache_lock);
        if (result != APR_SUCCESS) {
            ap_log_error(APLOG_MARK, APLOG_CRIT, result, s,
                         "Upload progress cache: failed to set mutex permissions");
            return result;
        }
#endif

        /* merge config in all vhost */
        s_vhost = s->next;
        while (s_vhost) {
            st_vhost = get_server_config(s_vhost);

            st_vhost->cache_shm = config->cache_shm;
            st_vhost->cache_rmm = config->cache_rmm;
            st_vhost->cache_file = config->cache_file;
            st_vhost->cache = config->cache;
            up_log(APLOG_MARK, APLOG_DEBUG, result, s,
                         "Upload Progress: merging Shared Cache conf: shm=0x%pp rmm=0x%pp "
                         "for VHOST: %s", config->cache_shm, config->cache_rmm,
                         s_vhost->server_hostname);

            st_vhost->cache_lock = config->cache_lock;
            st_vhost->lock_file = config->lock_file;
            s_vhost = s_vhost->next;
        }

    } else {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s,
                     "Upload Progress cache: cache size is zero, disabling "
                     "shared memory cache");
    }

    return(OK);
}

static int reportuploads_handler(request_rec *r)
{ 
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, r->server, "reportuploads_handler()");

    apr_size_t length, received, speed;
    time_t started_at=0;
    int done=0, err_status, found=0, param_error;
    char *response;
    DirConfig* dir = (DirConfig*)ap_get_module_config(r->per_dir_config, &upload_progress_module);

    if (!dir || (dir->report_enabled <= 0)) {
        return DECLINED;
    }
    if (r->method_number != M_GET) {
        return HTTP_METHOD_NOT_ALLOWED;
    }

    /* get the tracking id if any */
    const char *id = get_progress_id(r, &param_error);

    if (id == NULL) {
        if (param_error < 0) {
            ap_log_error(APLOG_MARK, APLOG_INFO, 0, r->server,
                         "Upload Progress: Report requested with invalid id. uri=%s", r->uri);
            return HTTP_BAD_REQUEST;
        } else {
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                         "Upload Progress: Report requested without id. uri=%s", r->uri);
            return HTTP_NOT_FOUND;
        }
    }

    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                 "Upload Progress: Report requested with id='%s'. uri=%s", id, r->uri);

    ServerConfig *config = get_server_config(r->server);

    CACHE_LOCK();
    upload_progress_node_t *node = find_node(r, id);
    if (node != NULL) {
        up_log(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                     "Node with id=%s found for report", id);
        received = node->received;
        length = node->length;
        done = node->done;
        speed = node->speed;
        started_at = node->started_at;
        err_status = node->err_status;
        found = 1;
    } else {
        up_log(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                     "Node with id=%s not found for report", id);
    }
    CACHE_UNLOCK();

    ap_set_content_type(r, "application/json");

    apr_table_set(r->headers_out, "Expires", "Mon, 28 Sep 1970 06:00:00 GMT");
    apr_table_set(r->headers_out, "Cache-Control", "no-cache");

    /* There are 4 possibilities
     * request not yet started: found = false
     * request in error:        err_status >= NGX_HTTP_SPECIAL_RESPONSE
     * request finished:        done = true
     * request not yet started but registered:        length==0 && rest ==0
     * reauest in progress:     rest > 0
     */

    if (!found) {
        response = apr_psprintf(r->pool, "{ \"state\" : \"starting\", \"uuid\" : \"%s\" }", id);
    } else if (err_status >= HTTP_BAD_REQUEST  ) {
        response = apr_psprintf(r->pool, "{ \"state\" : \"error\", \"status\" : %d, \"uuid\" : \"%s\" }", err_status, id);
    } else if (done) {
        response = apr_psprintf(r->pool, "{ \"state\" : \"done\", \"size\" : %d, \"speed\" : %d, \"started_at\": %d, \"uuid\" : \"%s\" }", length, speed, started_at, id);
    } else if ( length == 0 && received == 0 ) {
        response = apr_psprintf(r->pool, "{ \"state\" : \"starting\", \"uuid\" : \"%s\" }", id);
    } else {
        response = apr_psprintf(r->pool, "{ \"state\" : \"uploading\", \"received\" : %d, \"size\" : %d, \"speed\" : %d, \"started_at\": %d, \"uuid\" : \"%s\" }", received, length, speed, started_at, id);
    }

    char *completed_response;

    /* get the jsonp callback if any */
    const char *jsonp = get_json_callback_param(r, &param_error);

    if (param_error < 0) {
        ap_log_error(APLOG_MARK, APLOG_INFO, 0, r->server,
                     "Upload Progress: Report requested with invalid JSON-P callback. uri=%s", r->uri);
        return HTTP_BAD_REQUEST;
    }

    up_log(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                       "Upload Progress: JSON-P callback: %s.", jsonp);

    // fix up response for jsonp request, if needed
    if (jsonp) {
        completed_response = apr_psprintf(r->pool, "%s(%s);\r\n", jsonp, response);
    } else {
        completed_response = apr_psprintf(r->pool, "%s\r\n", response);
    }

    ap_rputs(completed_response, r);

    return OK;
}

static void upload_progress_child_init(apr_pool_t *p, server_rec *s)
{
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, s, "upload_progress_child_init()");

    apr_status_t sts;
    ServerConfig *st = get_server_config(s);

    if (!st->cache_lock) {
        ap_log_error(APLOG_MARK, APLOG_CRIT, 0, s, "Global mutex not set.");
        return;
    }

    sts = apr_global_mutex_child_init(&st->cache_lock, st->lock_file, p);
    if (sts != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_CRIT, sts, s,
                     "Failed to initialise global mutex %s in child process %"
                     APR_PID_T_FMT ".",
                     st->lock_file, getpid());
    }
}

static const command_rec upload_progress_cmds[] =
{
    AP_INIT_FLAG("TrackUploads", (CmdFunc) track_upload_progress_cmd, NULL, OR_AUTHCFG,
                 "Track upload progress in this location"),
    AP_INIT_FLAG("ReportUploads", (CmdFunc) report_upload_progress_cmd, NULL, OR_AUTHCFG,
                 "Report upload progress in this location"),
    AP_INIT_TAKE1("UploadProgressSharedMemorySize", (CmdFunc) upload_progress_shared_memory_size_cmd, NULL, RSRC_CONF,
                 "Size of shared memory used to keep uploads data, default 100KB"),
    { NULL }
};

static void upload_progress_register_hooks (apr_pool_t *p)
{
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, global_server, "upload_progress_register_hooks()");

    ap_hook_fixups(upload_progress_handle_request, NULL, NULL, APR_HOOK_FIRST);
    ap_hook_handler(reportuploads_handler, NULL, NULL, APR_HOOK_FIRST);
    ap_hook_post_config(upload_progress_init, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_child_init(upload_progress_child_init, NULL, NULL, APR_HOOK_MIDDLE);
    ap_register_input_filter("UPLOAD_PROGRESS", track_upload_progress, NULL, AP_FTYPE_RESOURCE);
}

module AP_MODULE_DECLARE_DATA upload_progress_module =
{
    STANDARD20_MODULE_STUFF,
    create_upload_progress_dir_config,
    merge_upload_progress_dir_config,
    upload_progress_config_create_server,
    NULL,
    upload_progress_cmds,
    upload_progress_register_hooks,      /* callback for registering hooks */
};
