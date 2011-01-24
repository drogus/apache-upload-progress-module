#include <stdint.h>
#include <ap_config.h>
#include <http_core.h>
#include <http_log.h>
#include <apr_pools.h>
#include <apr_strings.h>
#include "unixd.h"

#if APR_HAS_SHARED_MEMORY
#include "apr_rmm.h"
#include "apr_shm.h"
#endif

#if APR_HAVE_UNISTD_H
#include <unistd.h>
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
#define PROGRESS_KEY_TEMPLATE "12345678-0000-0000-0000-123456789012"
#ifndef PROGRESS_KEY_LEN
#  define PROGRESS_KEY_LEN strlen(PROGRESS_KEY_TEMPLATE)
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
    char key[PROGRESS_KEY_LEN];
}upload_progress_node_t;

typedef struct {
    int count;
    int active;
    upload_progress_node_t *nodes; /* all nodes allocated at once */
    int *list; /* static array of node indexes, list begins with indexes of active nodes */
}upload_progress_cache_t;

typedef struct {
    request_rec *r;
    upload_progress_node_t *node;
}upload_progress_context_t;


typedef struct {
    server_rec *server;
    apr_pool_t *pool;
    apr_global_mutex_t *cache_lock;
    char *lock_file;           /* filename for shm lock mutex */
    apr_size_t cache_bytes;

#if APR_HAS_SHARED_MEMORY
    apr_shm_t *cache_shm;
    apr_rmm_t *cache_rmm;
#endif
    char *cache_file;
    upload_progress_cache_t *cache;
} ServerConfig;

static const char* upload_progress_shared_memory_size_cmd(cmd_parms *cmd, void *dummy, const char *arg);
static void upload_progress_child_init(apr_pool_t *p, server_rec *s);
static int reportuploads_handler(request_rec *r);
upload_progress_node_t* insert_node(request_rec *r, const char *key);
upload_progress_node_t *store_node(ServerConfig *config, const char *key);
upload_progress_node_t *find_node(request_rec *r, const char *key);
static void clean_old_connections(request_rec *r);
void fill_new_upload_node_data(upload_progress_node_t *node, request_rec *r);
static apr_status_t upload_progress_cleanup(void *data);
const char *get_progress_id(request_rec *r);
static const char *track_upload_progress_cmd(cmd_parms *cmd, void *dummy, int arg);
static const char *report_upload_progress_cmd(cmd_parms *cmd, void *dummy, int arg);
void *upload_progress_config_create_dir(apr_pool_t *p, char *dirspec);
void *upload_progress_config_create_server(apr_pool_t *p, server_rec *s);
static void upload_progress_register_hooks(apr_pool_t *p);
static int upload_progress_handle_request(request_rec *r);
static int track_upload_progress(ap_filter_t *f, apr_bucket_brigade *bb,
        ap_input_mode_t mode, apr_read_type_e block,
        apr_off_t readbytes);
int upload_progress_init(apr_pool_t *, apr_pool_t *, apr_pool_t *, server_rec *);

//from passenger
typedef const char * (*CmdFunc)();// Workaround for some weird C++-specific compiler error.

/**/server_rec *global_server = NULL;

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

module AP_MODULE_DECLARE_DATA upload_progress_module =
{
    STANDARD20_MODULE_STUFF,
    upload_progress_config_create_dir,
    NULL,
    upload_progress_config_create_server,
    NULL,
    upload_progress_cmds,
    upload_progress_register_hooks,      /* callback for registering hooks */
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

ServerConfig *get_server_config(request_rec *r) {
    return (ServerConfig*)ap_get_module_config(r->server->module_config, &upload_progress_module);
}

static int upload_progress_handle_request(request_rec *r)
{
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, r->server, "upload_progress_handle_request()");

    DirConfig* dir = (DirConfig*)ap_get_module_config(r->per_dir_config, &upload_progress_module);
    ServerConfig *config = get_server_config(r);

    if (dir->track_enabled) {
        if (r->method_number == M_POST) {
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                         "Upload Progress: Upload in trackable location: %s.", r->uri);

            const char* id = get_progress_id(r);

            if (id != NULL) {
                up_log(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                             "Upload Progress: Progress id found: %s.", id);

                CACHE_LOCK();
                clean_old_connections(r);
                upload_progress_node_t *node = find_node(r, id);
                if (node == NULL) {
                    node = insert_node(r, id);
                    if (node)
                        up_log(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                                     "Upload Progress: Added upload with id=%s to list.", id);
                } else if (node->done) {
                    fill_new_upload_node_data(node, r);
                    up_log(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                                 "Upload Progress: Reused existing node with id '%s'.", id);
                } else {
                    node = NULL;
                }

                if (node) {
                    upload_progress_context_t *ctx = (upload_progress_context_t*)apr_pcalloc(r->pool, sizeof(upload_progress_context_t));
                    ctx->node = node;
                    ctx->r = r;
                    apr_pool_cleanup_register(r->pool, ctx, upload_progress_cleanup, apr_pool_cleanup_null);
                    ap_add_input_filter("UPLOAD_PROGRESS", NULL, r, r->connection);
                }
                CACHE_UNLOCK();
            }
        }
    }

    return DECLINED;
}

static const char *report_upload_progress_cmd(cmd_parms *cmd, void *config, int arg)
{
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, global_server, "report_upload_progress_cmd()");
    DirConfig* dir = (DirConfig*)config ;
    dir->report_enabled = arg;
    return NULL;
}

static const char *track_upload_progress_cmd(cmd_parms *cmd, void *config, int arg)
{
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, global_server, "track_upload_progress_cmd()");
    DirConfig* dir = (DirConfig*)config ;
    dir->track_enabled = arg;
    return NULL;
}

static const char* upload_progress_shared_memory_size_cmd(cmd_parms *cmd, void *dummy,
                                           const char *arg) {
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, global_server, "upload_progress_shared_memory_size_cmd()");
    ServerConfig *config = (ServerConfig*)ap_get_module_config(cmd->server->module_config, &upload_progress_module);

    long long int n = atoi(arg);

    if (n <= 0) {
        return "UploadProgressSharedMemorySize should be positive";
    }

    config->cache_bytes = (apr_size_t)n;

    return NULL;
}

void * upload_progress_config_create_dir(apr_pool_t *p, char *dirspec) {
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, global_server, "upload_progress_config_create_dir()");
    DirConfig* dir = (DirConfig*)apr_pcalloc(p, sizeof(DirConfig));
    dir->report_enabled = 0;
    dir->track_enabled = 0;
    return dir;
}

void *upload_progress_config_create_server(apr_pool_t *p, server_rec *s) {
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
    ServerConfig* config = get_server_config(f->r);

    rv = ap_get_brigade(f->next, bb, mode, block, readbytes);

    const char* id = get_progress_id(f->r);
    if (id == NULL)
        return rv;

    CACHE_LOCK();
    node = find_node(f->r, id);
    if (node != NULL) {
        if (rv == APR_SUCCESS) {
            apr_off_t length;
            apr_brigade_length(bb, 1, &length);
            node->received += (apr_size_t)length;
            if (node->received > node->length) /* handle chunked tranfer */
                node->length = node->received;
            time_t upload_time = time(NULL) - node->started_at;
            if (upload_time > 0) {
                node->speed = (apr_size_t)(node->received / upload_time);
            }
        }
        node->err_status = read_request_status(f->r);
    }
    CACHE_UNLOCK();

    return rv;
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

const char *get_progress_id(request_rec *r) {
    char *val;
    int len;

    //try to find progress id in headers
    const char *id  = apr_table_get(r->headers_in, PROGRESS_ID);

    //if not found check args
    if (id == NULL) {
        val = get_param_value(r->args, PROGRESS_ID, &len);
        if (val)
            if (len > PROGRESS_KEY_LEN) len = PROGRESS_KEY_LEN;
            id = apr_pstrndup(r->connection->pool, val, len);
    }

    return id;
}

const char *get_json_callback_param(request_rec *r) {
    char *val;
    int len;

    val = get_param_value(r->args, JSON_CB_PARAM, &len);
    if (val) {
        return apr_pstrndup(r->connection->pool, val, len);
    } else {
        return NULL;
    }
}

int check_node(upload_progress_node_t *node, const char *key) {
    return strncasecmp(node->key, key, PROGRESS_KEY_LEN) == 0 ? 1 : 0;
}

void fill_new_upload_node_data(upload_progress_node_t *node, request_rec *r) {
    const char *content_length;

    node->received = 0;
    node->done = 0;
    node->err_status = 0;
    node->started_at = time(NULL);
    node->speed = 0;
    node->expires = -1;
    content_length = apr_table_get(r->headers_in, "Content-Length");
    node->length = 1;
    /* Content-Length is missing is case of chunked transfer encoding */
    if (content_length)
        sscanf(content_length, "%d", &(node->length));
}

upload_progress_node_t* insert_node(request_rec *r, const char *key) {
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, r->server, "insert_node()");

    ServerConfig *config = (ServerConfig*)ap_get_module_config(r->server->module_config, &upload_progress_module);
    upload_progress_cache_t *cache = config->cache;
    upload_progress_node_t *node;

    if (cache->active == cache->count) {
        ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, r->server, "Cache full");
        return NULL;
    }
    node = &cache->nodes[cache->list[cache->active]];
    cache->active += 1;

    strncpy(node->key, key, PROGRESS_KEY_LEN);
    fill_new_upload_node_data(node, r);

    return node;
}

upload_progress_node_t *find_node(request_rec *r, const char *key) {
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, r->server, "find_node()");

    ServerConfig *config = (ServerConfig*)ap_get_module_config(r->server->module_config, &upload_progress_module);
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

    /* FIXME: this function should use locking because it modifies node data */
    upload_progress_context_t *ctx = (upload_progress_context_t *)data;

    if (ctx->node) {
        ctx->node->err_status = read_request_status(ctx->r);
        ctx->node->expires = time(NULL) + 60; /* expires in 60s */
        ctx->node->done = 1;
    }

    return APR_SUCCESS;
}

static void clean_old_connections(request_rec *r) {
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, r->server, "clean_old_connections()");

    ServerConfig *config = get_server_config(r);
    upload_progress_cache_t *cache = config->cache;
    upload_progress_node_t *node, *nodes = cache->nodes;
    int *list = cache->list;
    int i, tmp;
    time_t t = time(NULL);

    for (i = 0; i < cache->active; i++) {
        node = &nodes[list[i]];
        if (t > node->expires && node->done == 1 && node->expires != -1) {
            cache->active -= 1;
            tmp = list[cache->active];
            list[cache->active] = list[i];
            list[i] = tmp;
            i--;
        }

    }
}

static apr_status_t upload_progress_cache_module_kill(void *data)
{
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, global_server, "upload_progress_cache_module_kill()");

    return APR_SUCCESS;
}

void *rmm_calloc(apr_rmm_t *rmm, apr_size_t reqsize)
{
    apr_rmm_off_t block = apr_rmm_calloc(rmm, reqsize);
    return block ? apr_rmm_addr_get(rmm, block) : NULL;
}

apr_status_t upload_progress_cache_init(apr_pool_t *pool, ServerConfig *config)
{
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, global_server, "upload_progress_cache_init()");

#if APR_HAS_SHARED_MEMORY
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

    apr_pool_cleanup_register(config->pool, config , upload_progress_cache_module_kill, apr_pool_cleanup_null);

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
                 "Upload Progress: monitoring %i simultaneous uploads", nodes_cnt);
#endif

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

    ServerConfig *config = (ServerConfig*)ap_get_module_config(s->module_config, &upload_progress_module);

    void *data;
    const char *userdata_key = "upload_progress_init";

    /* upload_progress_init will be called twice. Don't bother
     * going through all of the initialization on the first call
     * because it will just be thrown away.*/
    apr_pool_userdata_get(&data, userdata_key, s->process->pool);
    if (!data) {
        apr_pool_userdata_set((const void *)1, userdata_key,
                               apr_pool_cleanup_null, s->process->pool);

    #if APR_HAS_SHARED_MEMORY
        /* If the cache file already exists then delete it.  Otherwise we are
         * going to run into problems creating the shared memory. */
        if (config->cache_file) {
            char *lck_file = apr_pstrcat(ptemp, config->cache_file, ".lck",
                                         NULL);
            apr_file_remove(lck_file, ptemp);
        }
    #endif
        return OK;
    }

#if APR_HAS_SHARED_MEMORY
    /* initializing cache if shared memory size is not zero and we already
     * don't have shm address
     */
    if (!config->cache_shm && config->cache_bytes > 0) {
#endif
        result = upload_progress_cache_init(p, config);
        if (result != APR_SUCCESS) {
            ap_log_error(APLOG_MARK, APLOG_ERR, result, s,
                         "Upload Progress cache: could not create shared memory segment");
            return DONE;
        }

#if APR_HAS_SHARED_MEMORY
        if (config->cache_file) {
            config->lock_file = apr_pstrcat(config->pool, config->cache_file, ".lck",
                                        NULL);
        }
#endif

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
            st_vhost = (ServerConfig *)
                       ap_get_module_config(s_vhost->module_config,
                                            &upload_progress_module);

#if APR_HAS_SHARED_MEMORY
            st_vhost->cache_shm = config->cache_shm;
            st_vhost->cache_rmm = config->cache_rmm;
            st_vhost->cache_file = config->cache_file;
            st_vhost->cache = config->cache;
            up_log(APLOG_MARK, APLOG_DEBUG, result, s,
                         "Upload Progress: merging Shared Cache conf: shm=0x%pp rmm=0x%pp "
                         "for VHOST: %s", config->cache_shm, config->cache_rmm,
                         s_vhost->server_hostname);
#endif

            st_vhost->cache_lock = config->cache_lock;
            st_vhost->lock_file = config->lock_file;
            s_vhost = s_vhost->next;
        }

#if APR_HAS_SHARED_MEMORY
    } else {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s,
                     "Upload Progress cache: cache size is zero, disabling "
                     "shared memory cache");
    }
#endif

    return(OK);
}

static int reportuploads_handler(request_rec *r)
{ 
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, r->server, "reportuploads_handler()");

    apr_size_t length, received, speed;
    time_t started_at=0;
    int done=0, err_status, found=0;
    char *response;
    DirConfig* dir = (DirConfig*)ap_get_module_config(r->per_dir_config, &upload_progress_module);

    if(!dir->report_enabled) {
        return DECLINED;
    }
    if (r->method_number != M_GET) {
        return HTTP_METHOD_NOT_ALLOWED;
    }

    /* get the tracking id if any */
    const char *id = get_progress_id(r);

    if (id == NULL) {
        up_log(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                     "Upload Progress: Request without id in location with reports enabled. uri=%s", id, r->uri);
        return HTTP_NOT_FOUND;
    } else {
        up_log(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                     "Upload Progress: Request with id=%s in location with reports enabled. uri=%s", id, r->uri);
    }

    ServerConfig *config = (ServerConfig*)ap_get_module_config(r->server->module_config, &upload_progress_module);

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

    ap_set_content_type(r, "text/javascript");

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
    const char *jsonp = get_json_callback_param(r);

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
    ServerConfig *st = (ServerConfig *)ap_get_module_config(s->module_config, &upload_progress_module);

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
