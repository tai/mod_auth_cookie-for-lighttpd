//
// Cookie-based authentication for lighttpd
//
// This module protects webpage from clients without valid
// cookie. By redirecting not-yet-valid clients to certain
// "logon page", you can protect any webapp without adding
// any auth code to webapp itself.
//
// Unlike mod_authcookie for Apache, this DOES NOT work
// with mod_auth_* modules due to lighttpd limitation (there's
// no way to turn 401 response into page redirection).
// This module solely relies on external "logon page" for
// authentication, and expect it to provide a valid cookie as a
// ticket for authenticated access.
//

#include <ctype.h>

#include "plugin.h"
#include "log.h"
#include "response.h"
#include "md5.h"

#include "base64.h"

#define LOG(level, ...)                                           \
    if (pc->loglevel >= level) {                                  \
        log_error_write(srv, __FILE__, __LINE__, __VA_ARGS__);    \
    }

#define FATAL(...) LOG(0, __VA_ARGS__)
#define ERROR(...) LOG(1, __VA_ARGS__)
#define WARN(...)  LOG(2, __VA_ARGS__)
#define INFO(...)  LOG(3, __VA_ARGS__)
#define DEBUG(...) LOG(4, __VA_ARGS__)

#define HEADER(con, key)                                                \
    (data_string *)array_get_element((con)->request.headers, (key))

#define MD5_LEN 16

/**********************************************************************
 * data strutures
 **********************************************************************/

// module configuration
typedef struct {
    int loglevel;
    buffer *name;    // cookie name to extract auth info
    int override;    // how to handle incoming Auth header
    buffer *authurl; // page to go when unauthorized
    buffer *key;     // key for cookie verification
    int timeout;     // life duration of last-stage auth token
    buffer *options; // options for last-stage auth token cookie
} plugin_config;

// top-level module structure
typedef struct {
    PLUGIN_DATA;
        
    plugin_config **config;
    plugin_config   conf;

    array *users;
} plugin_data;

/**********************************************************************
 * supporting functions
 **********************************************************************/

//
// helper to generate "configuration in current context".
//
static plugin_config *
merge_config(server *srv, connection *con, plugin_data *pd) {
#define PATCH(x) pd->conf.x = pc->x
#define MATCH(k) if (buffer_is_equal_string(du->key, CONST_STR_LEN(k)))
#define MERGE(k, x) MATCH(k) PATCH(x)

    size_t i, j;
    plugin_config *pc = pd->config[0]; // start from global context

    // load initial config in global context
    PATCH(loglevel);
    PATCH(name);
    PATCH(override);
    PATCH(authurl);
    PATCH(key);
    PATCH(timeout);
    PATCH(options);

    // merge config from sub-contexts
    for (i = 1; i < srv->config_context->used; i++) {
        data_config *dc = (data_config *)srv->config_context->data[i];

        // condition didn't match
        if (! config_check_cond(srv, con, dc)) continue;

        // merge config
        pc = pd->config[i];
        for (j = 0; j < dc->value->used; j++) {
            data_unset *du = dc->value->data[j];

            // describe your merge-policy here...
            MERGE("auth-cookie.loglevel", loglevel);
            MERGE("auth-cookie.name", name);
            MERGE("auth-cookie.override", override);
            MERGE("auth-cookie.authurl", authurl);
            MERGE("auth-cookie.key", key);
            MERGE("auth-cookie.timeout", timeout);
            MERGE("auth-cookie.options", options);
        }
    }
    return &(pd->conf);
#undef PATCH
#undef MATCH
#undef MERGE
}

//
// fills (appends) given buffer with "current" URL.
//
static buffer *
self_url(connection *con, buffer *url, buffer_encoding_t enc) {
    buffer_append_string_encoded(url, CONST_BUF_LEN(con->uri.scheme), enc);
    buffer_append_string_encoded(url, CONST_STR_LEN("://"), enc);
    buffer_append_string_encoded(url, CONST_BUF_LEN(con->uri.authority), enc);
    buffer_append_string_encoded(url, CONST_BUF_LEN(con->request.uri), enc);
    return url;
}

//
// Generates appropriate response depending on policy.
//
static handler_t
endauth(server *srv, connection *con, plugin_config *pc) {
    // pass through if no redirect target is specified
    if (buffer_is_empty(pc->authurl)) {
        DEBUG("s", "endauth - continuing");
        return HANDLER_GO_ON;
    }
    DEBUG("sb", "endauth - redirecting:", pc->authurl);

    // prepare redirection header
    buffer *url = buffer_init_buffer(pc->authurl);
    buffer_append_string(url, strchr(url->ptr, '?') ? "&url=" : "?url=");
    self_url(con, url, ENCODING_REL_URI);
    response_header_insert(srv, con, 
                           CONST_STR_LEN("Location"), CONST_BUF_LEN(url));
    buffer_free(url);

    // prepare response
    con->http_status = 307;
    con->mode = DIRECT;
    con->file_finished = 1;

    return HANDLER_FINISHED;
}

inline int
min(int a, int b) {
    return a > b ? b : a;
}

// generate hex-encoded random string
int
gen_random(buffer *b, int len) {
    buffer_prepare_append(b, len);
    while (len--) {
        char c = int2hex(rand() >> 24);
        buffer_append_memory(b, &c, 1);
    }
    return 0;
}

// encode bytes into hexstring
int
hex_encode(buffer *b, const uint8_t *s, int len) {
    return buffer_copy_string_hex(b, (const char *)s, len);
}

// decode hexstring into bytes
int
hex_decode(buffer *b, const char *s) {
    char c0, c1;
        
    buffer_prepare_append(b, strlen(s) >> 1);
    while ((c0 = *s++) && (c1 = *s++)) {
        char v = (hex2int(c0) << 4) | hex2int(c1);
        buffer_append_memory(b, &v, 1);
    }
    return 0;
}

// XOR-based decryption
int
encrypt(buffer *buf, uint8_t *key, int keylen) {
    unsigned int i;

    for (i = 0; i < buf->used; i++) {
        buf->ptr[i] ^= (i > 0 ? buf->ptr[i - 1] : 0) ^ key[i % keylen];
    }
    return 0;
}

// XOR-based encryption
int
decrypt(buffer *buf, uint8_t *key, int keylen) {
    int i;

    for (i = buf->used - 1; i >= 0; i--) {
        buf->ptr[i] ^= (i > 0 ? buf->ptr[i - 1] : 0) ^ key[i % keylen];

        // sanity check - result should be base64-encoded authinfo
        if (! isprint(buf->ptr[i])) {
            return -1;
        }
    }
    return 0;
}

//
// update header using (verified) authentication info.
//
void
update_header(server *srv, connection *con,
              plugin_data *pd, plugin_config *pc, buffer *authinfo) {
    buffer *field, *token;

    //DEBUG("sb", "decrypted authinfo:", authinfo);

    // insert auth header
    field = buffer_init_string("Basic ");
    buffer_append_string_buffer(field, authinfo);
    array_set_key_value(con->request.headers,
                        CONST_STR_LEN("Authorization"), CONST_BUF_LEN(field));

    // generate random token and relate it with authinfo
    gen_random(token = buffer_init(), MD5_LEN * 2); // length in hex string
    DEBUG("sb", "pairing authinfo with token:", token);
    buffer_copy_long(field, time(NULL));
    buffer_append_string(field, ":");
    buffer_append_string_buffer(field, authinfo);
    array_set_key_value(pd->users, CONST_BUF_LEN(token), CONST_BUF_LEN(field));

    // insert opaque auth token
    buffer_copy_string_buffer(field, pc->name);
    buffer_append_string(field, "=token:");
    buffer_append_string_buffer(field, token);
    buffer_append_string(field, "; ");
    buffer_append_string_buffer(field, pc->options);
    DEBUG("sb", "generating token cookie:", field);
    response_header_append(srv, con,
                           CONST_STR_LEN("Set-Cookie"), CONST_BUF_LEN(field));

    // update REMOTE_USER field
    base64_decode(field, authinfo->ptr);
    char *pw = strchr(field->ptr, ':'); *pw = '\0';
    DEBUG("ss", "identified username:", field->ptr);
    buffer_copy_string_len(con->authed_user, field->ptr, strlen(field->ptr));
    
    buffer_free(field);
    buffer_free(token);
}

//
// Handle token given in cookie.
//
// Expected Cookie Format:
//   <name>=token:<random-token-to-be-verified>
// 
static handler_t
handle_token(server *srv, connection *con,
             plugin_data *pd, plugin_config *pc, char *token) {
    data_string *entry =
        (data_string *)array_get_element(pd->users, token);

    // Check for existence
    if (! entry) return endauth(srv, con, pc);

    DEBUG("sb", "found token entry:", entry->value);

    // Check for timeout
    time_t t0 = time(NULL);
    time_t t1 = strtol(entry->value->ptr, NULL, 10);
    DEBUG("sdsdsd", "t0:", t0, ", t1:", t1, ", timeout:", pc->timeout);
    if (t0 - t1 > pc->timeout) return endauth(srv, con, pc);

    // Check for existence of actual authinfo
    char *authinfo = strchr(entry->value->ptr, ':');
    if (! authinfo) return endauth(srv, con, pc);

    // All passed. Inject as BasicAuth header
    buffer *field = buffer_init_string("Basic ");
    buffer_append_string(field, authinfo + 1);
    array_set_key_value(con->request.headers,
                        CONST_STR_LEN("Authorization"), CONST_BUF_LEN(field));

    // update REMOTE_USER field
    base64_decode(field, authinfo + 1);
    char *pw = strchr(field->ptr, ':'); *pw = '\0';
    DEBUG("ss", "identified user:", field->ptr);
    buffer_copy_string_len(con->authed_user, field->ptr, strlen(field->ptr));
    buffer_free(field);

    DEBUG("s", "all check passed");
    return HANDLER_GO_ON;
}

//
// Check for redirected auth request in cookie.
//
// Expected Cookie Format:
//   <name>=crypt:<hash>:<data>
//
//   hash    = hex(MD5(key + timesegment + data))
//   data    = hex(encrypt(MD5(timesegment + key), payload))
//   payload = base64(username + ":" + password)
//
static handler_t
handle_crypt(server *srv, connection *con,
             plugin_data *pd, plugin_config *pc, char *line) {
    MD5_CTX ctx;
    uint8_t hash[MD5_LEN];
    char    tmp[256];

    // Check for existence of data part
    char *data = strchr(line, ':');
    if (! data) return endauth(srv, con, pc);

    DEBUG("s", "verifying crypt cookie...");
    
    // Verify signature.
    // Also, find time segment when this auth request was encrypted.
    time_t t1, t0 = time(NULL);
    buffer *buf = buffer_init();
    for (t1 = t0 - (t0 % 5); t0 - t1 < 10; t1 -= 5) {
        DEBUG("sdsd", "t0:", t0, ", t1:", t1);
        
        // compute hash for this time segment
        sprintf(tmp, "%lu", t1);
        MD5_Init(&ctx);
        MD5_Update(&ctx, CONST_BUF_LEN(pc->key));
        MD5_Update(&ctx, tmp, strlen(tmp));
        MD5_Update(&ctx, data + 1, strlen(data + 1));
        MD5_Final(hash, &ctx);
        hex_encode(buf, hash, sizeof(hash));

        DEBUG("sb", "computed hash:", buf);

        // verify by comparing hash
        if (strncasecmp(buf->ptr, line, data - line) == 0) {
            break; // hash verified and time segment found
        }
    }
    buffer_free(buf);

    // Has this found time segment expired?
    if (! (t0 - t1 < 10)) {
        DEBUG("s", "timeout detected");
        return endauth(srv, con, pc);
    }
    DEBUG("s", "timeout check passed");
    
    // compute temporal encryption key (= MD5(t1, key))
    sprintf(tmp, "%lu", t1);
    MD5_Init(&ctx);
    MD5_Update(&ctx, tmp, strlen(tmp));
    MD5_Update(&ctx, CONST_BUF_LEN(pc->key));
    MD5_Final(hash, &ctx);

    // decrypt
    hex_decode(buf = buffer_init(), data + 1);
    if (decrypt(buf, hash, sizeof(hash)) != 0) {
        WARN("s", "decryption error");

        buffer_free(buf);
        return endauth(srv, con, pc);
    }

    // update header using decrypted authinfo
    update_header(srv, con, pd, pc, buf);
    
    buffer_free(buf);
    return HANDLER_GO_ON;
}

/**********************************************************************
 * module interface
 **********************************************************************/

INIT_FUNC(module_init) {
    plugin_data *pd;

    pd = calloc(1, sizeof(*pd));
    pd->users = array_init();
    return pd;
}

FREE_FUNC(module_free) {
    plugin_data *pd = p_d;

    if (! pd) return HANDLER_GO_ON;

    // Free plugin data
    array_free(pd->users);
    
    // Free configuration data.
    // This must be done for each context.
    if (pd->config) {
        size_t i;
        for (i = 0; i < srv->config_context->used; i++) {
            plugin_config *pc = pd->config[i];
            if (! pc) continue;

            // free configuration
            buffer_free(pc->name);
            buffer_free(pc->authurl);
            buffer_free(pc->key);

            free(pc);
        }
        free(pd->config);
    }
    free(pd);

    return HANDLER_GO_ON;
}

//
// authorization handler
//
URIHANDLER_FUNC(module_uri_handler) {
    plugin_data   *pd = p_d;
    plugin_config *pc = merge_config(srv, con, pd);
    data_string *ds;
    char buf[1024]; // cookie content
    char key[32];   // <AuthName> key
    char *cs;       // pointer to (some part of) <AuthName> key

    // skip if not enabled
    if (buffer_is_empty(pc->name)) return HANDLER_GO_ON;

    // decide how to handle incoming Auth header
    if ((ds = HEADER(con, "Authorization")) != NULL) {
        switch (pc->override) {
        case 0: return HANDLER_GO_ON;   // just use it if supplied
        case 1: break;                  // use CookieAuth if exists
        case 2:
        default: buffer_reset(ds->key); // use CookieAuth only
        }
    }

    // check for cookie
    if ((ds = HEADER(con, "Cookie")) == NULL) return endauth(srv, con, pc);
    DEBUG("sb", "parsing cookie:", ds->value);

    // prepare cstring for processing
    memset(key, 0, sizeof(key));
    memset(buf, 0, sizeof(buf));
    strncpy(key, pc->name->ptr,  min(sizeof(key) - 1, pc->name->used));
    strncpy(buf, ds->value->ptr, min(sizeof(buf) - 1, ds->value->used));
    DEBUG("ss", "parsing for key:", key);
    
    // check for "<AuthName>=" entry in a cookie
    for (cs = buf; (cs = strstr(cs, key)) != NULL; ) {
        DEBUG("ss", "checking cookie entry:", cs);

        // check if found entry matches exactly for "KEY=" part.
        cs += pc->name->used - 1;  // jump to the end of "KEY" part
        while (isspace(*cs)) cs++; // whitespace can be skipped

        // break forward if this was an exact match
        if (*cs++ == '=') {
            char *eot = strchr(cs, ';');
            if (eot) *eot = '\0';
            break;
        }
    }
    if (! cs) return endauth(srv, con, pc); // not found - rejecting

    // unescape payload
    buffer *tmp = buffer_init_string(cs);
    buffer_urldecode_path(tmp);
    memset(buf, 0, sizeof(buf));
    strncpy(buf, tmp->ptr, min(sizeof(buf) - 1, tmp->used));
    buffer_free(tmp);
    cs = buf;

    // Allow access if client already has an "authorized" token.
    if (strncmp(cs, "token:", 6) == 0) {
        return handle_token(srv, con, pd, pc, cs + 6);
    }

    // Verify "non-authorized" CookieAuth request in encrypted format.
    // Once verified, give out authorized token ("token:..." cookie).
    if (strncmp(cs, "crypt:", 6) == 0) {
        return handle_crypt(srv, con, pd, pc, cs + 6);
    }

    DEBUG("ss", "unrecognied cookie auth format:", cs);
    return endauth(srv, con, pc);
}

SETDEFAULTS_FUNC(module_set_defaults) {
    plugin_data *pd = p_d;
    size_t i;

    config_values_t cv[] = {
        { "auth-cookie.loglevel",
          NULL, T_CONFIG_INT,    T_CONFIG_SCOPE_CONNECTION },
        { "auth-cookie.name",
          NULL, T_CONFIG_STRING, T_CONFIG_SCOPE_CONNECTION },
        { "auth-cookie.override",
          NULL, T_CONFIG_INT,    T_CONFIG_SCOPE_CONNECTION },
        { "auth-cookie.authurl",
          NULL, T_CONFIG_STRING, T_CONFIG_SCOPE_CONNECTION },
        { "auth-cookie.key",
          NULL, T_CONFIG_STRING, T_CONFIG_SCOPE_CONNECTION },
        { "auth-cookie.timeout",
          NULL, T_CONFIG_INT,    T_CONFIG_SCOPE_CONNECTION },
        { "auth-cookie.options",
          NULL, T_CONFIG_STRING, T_CONFIG_SCOPE_CONNECTION },
        { NULL, NULL, T_CONFIG_UNSET, T_CONFIG_SCOPE_UNSET }
    };

    pd->config = calloc(1,
                        srv->config_context->used * sizeof(specific_config *));

    for (i = 0; i < srv->config_context->used; i++) {
        plugin_config *pc;

        pc = pd->config[i] = calloc(1, sizeof(plugin_config));
        pc->loglevel = 1;
        pc->name     = buffer_init();
        pc->override = 2;
        pc->authurl  = buffer_init();
        pc->key      = buffer_init();
        pc->timeout  = 86400;
        pc->options  = buffer_init();

        cv[0].destination = &(pc->loglevel);
        cv[1].destination = pc->name;
        cv[2].destination = &(pc->override);
        cv[3].destination = pc->authurl;
        cv[4].destination = pc->key;
        cv[5].destination = &(pc->timeout);
        cv[6].destination = pc->options;

        array *ca = ((data_config *)srv->config_context->data[i])->value;
        if (config_insert_values_global(srv, ca, cv) != 0) {
            return HANDLER_ERROR;
        }
    }
    return HANDLER_GO_ON;
}

int
mod_auth_cookie_plugin_init(plugin *p) {
    p->version          = LIGHTTPD_VERSION_ID;
    p->name             = buffer_init_string("auth_cookie");
    p->init             = module_init;
    p->set_defaults     = module_set_defaults;
    p->cleanup          = module_free;
    p->handle_uri_clean = module_uri_handler;
    p->data             = NULL;

    return 0;
}
