/*
 * Copyright (C) 2012-2014 Paul Ionkin <paul.ionkin@gmail.com>
 * Copyright (C) 2012-2014 Skoobe GmbH. All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */
#include "http_connection.h"
#include "utils.h"
#include "stat_srv.h"
#include "awsv4.h"

/*{{{ struct*/

// HTTP header: key, value
typedef struct {
    gchar *key;
    gchar *value;
} HttpConnectionHeader;

#define CON_LOG "con"
#define CMD_IDLE 9999

static void http_connection_on_close (struct evhttp_connection *evcon, void *ctx);
static gboolean http_connection_init (HttpConnection *con);
static void http_connection_free_headers (GList *l_headers);

/*}}}*/

/*{{{ create / destroy */
// create HttpConnection object
// establish HTTP connections to
gpointer http_connection_create (Application *app)
{
    HttpConnection *con;

    con = g_new0 (HttpConnection, 1);
    if (!con) {
        LOG_err (CON_LOG, "Failed to create HttpConnection !");
        return NULL;
    }

    con->app = app;
    con->l_output_headers = NULL;
    con->cur_cmd_type = CMD_IDLE;
    con->cur_url = NULL;
    con->cur_time_start = 0;
    con->cur_time_stop = 0;
    con->jobs_nr = 0;
    con->connects_nr = 0;
    con->cur_code = 0;
    con->total_bytes_out = con->total_bytes_in = 0;
    con->errors_nr = 0;

    con->is_acquired = FALSE;

    if (!http_connection_init (con)) {
        g_free (con);
        return NULL;
    }

    return (gpointer)con;
}

static gboolean http_connection_init (HttpConnection *con)
{
    LOG_debug (CON_LOG, CON_H"Connecting to %s:%d", (void *)con,
        conf_get_string (application_get_conf (con->app), "s3.host"),
        conf_get_int (application_get_conf (con->app), "s3.port")
    );

    con->connects_nr++;

    if (con->evcon)
        evhttp_connection_free (con->evcon);

#ifdef SSL_ENABLED
    if (conf_get_boolean (application_get_conf (con->app), "s3.ssl")) {
        SSL *ssl;
        struct bufferevent *bev;

        ssl = SSL_new (application_get_ssl_ctx (con->app));
        if (!ssl) {
            LOG_err (CON_LOG, CON_H"Failed to create SSL connection: %s", (void *)con,
                ERR_reason_error_string (ERR_get_error ()));
            return FALSE;
        }

        bev = bufferevent_openssl_socket_new (
            application_get_evbase (con->app),
            -1, ssl,
            BUFFEREVENT_SSL_CONNECTING,
            BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS
        );

        if (!bev) {
            LOG_err (CON_LOG, CON_H"Failed to create SSL connection !", (void *)con);
            return FALSE;
        }

        con->evcon = evhttp_connection_base_bufferevent_new (
            application_get_evbase (con->app),
            application_get_dnsbase (con->app),
            bev,
            conf_get_string (application_get_conf (con->app), "s3.host"),
            conf_get_int (application_get_conf (con->app), "s3.port")
        );
    } else {
#endif
    con->evcon = evhttp_connection_base_new (
        application_get_evbase (con->app),
        application_get_dnsbase (con->app),
        conf_get_string (application_get_conf (con->app), "s3.host"),
        conf_get_int (application_get_conf (con->app), "s3.port")
    );
#ifdef SSL_ENABLED
    }
#endif

    if (!con->evcon) {
        LOG_err (CON_LOG, "Failed to create evhttp_connection !");
        return FALSE;
    }

    evhttp_connection_set_timeout (con->evcon, conf_get_int (application_get_conf (con->app), "connection.timeout"));
    evhttp_connection_set_retries (con->evcon, conf_get_int (application_get_conf (con->app), "connection.retries"));

    evhttp_connection_set_closecb (con->evcon, http_connection_on_close, con);

    return TRUE;
}

// destroy HttpConnection)
void http_connection_destroy (gpointer data)
{
    HttpConnection *con = (HttpConnection *) data;

    if (con->cur_url)
        g_free (con->cur_url);
    if (con->evcon)
        evhttp_connection_free (con->evcon);
    g_free (con);
}
/*}}}*/

void http_connection_set_on_released_cb (gpointer client, ClientPool_on_released_cb client_on_released_cb, gpointer ctx)
{
    HttpConnection *con = (HttpConnection *) client;

    con->client_on_released_cb = client_on_released_cb;
    con->pool_ctx = ctx;
}

gboolean http_connection_check_rediness (gpointer client)
{
    HttpConnection *con = (HttpConnection *) client;

    return !con->is_acquired;
}

gboolean http_connection_acquire (HttpConnection *con)
{
    con->is_acquired = TRUE;

    LOG_debug (CON_LOG, CON_H"Connection object is acquired!", (void *)con);

    return TRUE;
}

gboolean http_connection_release (HttpConnection *con)
{
    con->is_acquired = FALSE;

    LOG_debug (CON_LOG, CON_H"Connection object is released!", (void *)con);

    if (con->client_on_released_cb)
        con->client_on_released_cb (con, con->pool_ctx);

    return TRUE;
}

// callback connection is closed
static void http_connection_on_close (G_GNUC_UNUSED struct evhttp_connection *evcon, void *ctx)
{
    HttpConnection *con = (HttpConnection *) ctx;

    LOG_debug (CON_LOG, CON_H"Connection closed !", (void *)con);

    //con->cur_cmd_type = CMD_IDLE;

    //XXX: need further investigation !
    //con->evcon = NULL;
}

/*{{{ getters */
Application *http_connection_get_app (HttpConnection *con)
{
    return con->app;
}

struct evhttp_connection *http_connection_get_evcon (HttpConnection *con)
{
    return con->evcon;
}

/*}}}*/

/*{{{ get_auth_string */
// create  auth string
// http://docs.amazonwebservices.com/Amazon/2006-03-01/dev/RESTAuthentication.html
static gchar *http_connection_get_auth_string_v4 (HttpConnection *con,
        const gchar *method, const gchar *resource, const time_t reqtime,
        GList *l_output_headers)
{
    gchar* _result;
    unsigned int md_len, _num_entries;
    GList *l;
    gchar *s_headers;
    gchar *_date_str;
    gchar** _headers;

    struct evhttp_uri* _uri;
    gchar* _canonical_uri;
    gchar* _canonical_query;
    KeyValuePair** _canonical_headers;
    gchar* _canonical_request;
    gchar* _credential_scope;
    gchar* _string_to_sign;
    gchar* _signature;
    gchar* _region;
    gchar* _map_headers_string;
    gchar* _map_signed_headers;
    gchar* _tmp;
    gchar* _payloadsha256;

    _region = conf_get_string (application_get_conf (con->app), "s3.region");

    s_headers = NULL;
    for (l = g_list_first (l_output_headers); l; l = g_list_next (l)) {
        HttpConnectionHeader *header = (HttpConnectionHeader *) l->data;

        if(strcicmp(header->key, "x-amz-content-sha256") == 0)
        {
            _payloadsha256 = header->value;
        }

        if(s_headers == NULL)
        {
            s_headers = g_strdup_printf("%s:%s", header->key, header->value);

        }
        else
        {
            _tmp = s_headers;
            s_headers = g_strdup_printf("%s\n%s:%s", s_headers, header->key, header->value);
            g_free(_tmp);
        }    
    }


    _headers = str_split(s_headers,"\n", &_num_entries,false);
    
    g_free(s_headers);

    //LOG_debug (CON_LOG, "%s %s", string_to_sign, conf_get_string (application_get_conf (con->app), "s3.secret_access_key"));

    _uri =  evhttp_uri_parse_with_flags(con->cur_url, EVHTTP_URI_NONCONFORMANT);

    _date_str = utc_yyyymmdd(&reqtime);
    
    _canonical_uri = canonicalize_uri(_uri);
    _canonical_query = canonicalize_query(_uri);
    _canonical_headers = canonicalize_headers(_num_entries, (const gchar**)_headers);

    _map_headers_string = map_headers_string(_num_entries, (const KeyValuePair**)_canonical_headers);
    _map_signed_headers = map_signed_headers(_num_entries, (const KeyValuePair**)_canonical_headers);

    _canonical_request = canonicalize_request(
                                    method,
                                    _canonical_uri,
                                    _canonical_query,
                                    _map_headers_string,
                                    _map_signed_headers,
                                    _payloadsha256
                                );

    LOG_debug (CON_LOG, "Caninonical Request Send:\n%s\n", _canonical_request);

    _credential_scope = credential_scope(
                                            &reqtime,
                                            _region,
                                            "s3"
                                        );


    _tmp = sha256_base16(_canonical_request, strlen(_canonical_request));
    _string_to_sign = string_to_sign(
                                "AWS4-HMAC-SHA256",
                                &reqtime,
                                _credential_scope,
                                _tmp
                            );
    g_free(_tmp);

    _signature = calculate_signature
                                    (
                                        &reqtime, 
                                        conf_get_string (application_get_conf (con->app), "s3.secret_access_key"),
                                        _region,
                                        "s3",
                                        _string_to_sign
                                    );

    LOG_debug (CON_LOG, "StringToSign Send:\n%s\n", _string_to_sign);

    _result = g_strdup_printf(
                                "AWS4-HMAC-SHA256 Credential=%s/%s/%s/s3/aws4_request,SignedHeaders=%s,Signature=%s", 
                                conf_get_string (application_get_conf (con->app), "s3.access_key_id"), 
                                _date_str,
                                _region,
                                _map_signed_headers,
                                _signature);


    g_free(_date_str);
    g_free(_canonical_uri);
    g_free(_canonical_query);
    g_free(_canonical_request);
    g_free(_credential_scope);
    g_free(_string_to_sign);
    g_free(_signature);
    g_free(_map_headers_string);
    g_free(_map_signed_headers);

    free_kvp_array(_canonical_headers, _num_entries);
    free_str_array(_headers, _num_entries);

    evhttp_uri_free(_uri);

    return _result;
}
/*}}}*/


/*{{{ get_auth_string */
// create  auth string
// http://docs.amazonwebservices.com/Amazon/2006-03-01/dev/RESTAuthentication.html
static gchar *http_connection_get_auth_string (Application *app,
        const gchar *method, const gchar *resource, const gchar *time_str,
        GList *l_output_headers)
{
    gchar *string_to_sign;
    unsigned int md_len;
    unsigned char md[EVP_MAX_MD_SIZE];
    gchar *tmp;
    GList *l;
    GString *s_headers;
    gchar *content_md5 = NULL;
    gchar *content_type = NULL;

    s_headers = g_string_new ("");
    for (l = g_list_first (l_output_headers); l; l = g_list_next (l)) {
        HttpConnectionHeader *header = (HttpConnectionHeader *) l->data;

        if (!strncmp ("Content-MD5", header->key, strlen ("Content-MD5"))) {
            if (content_md5)
                g_free (content_md5);
            content_md5 = g_strdup (header->value);
        } else if (!strncmp ("Content-Type", header->key, strlen ("Content-Type"))) {
            if (content_type)
                g_free (content_type);
            content_type = g_strdup (header->value);
        // select all HTTP request headers that start with 'x-amz-' (using a case-insensitive comparison)
        } else if (strcasestr (header->key, "x-amz-")) {
            g_string_append_printf (s_headers, "%s:%s\n", header->key, header->value);
        }
    }

    if (!content_md5)
        content_md5 = g_strdup ("");
    if (!content_type)
        content_type = g_strdup ("");

    // The list of sub-resources that must be included when constructing the CanonicalizedResource
    // Element are: acl, lifecycle, location, logging, notification, partNumber, policy,
    // requestPayment, torrent, uploadId, uploads, versionId, versioning, versions and website.
    if (strlen (resource) > 2 && resource[1] == '?') {
        if (strstr (resource, "?acl") || strstr (resource, "?versioning") || strstr (resource, "?versions"))
            tmp = g_strdup_printf ("/%s%s", conf_get_string (application_get_conf (app), "s3.bucket_name"), resource);
        else
            tmp = g_strdup_printf ("/%s/", conf_get_string (application_get_conf (app), "s3.bucket_name"));
    } else
        tmp = g_strdup_printf ("/%s%s", conf_get_string (application_get_conf (app), "s3.bucket_name"), resource);

    string_to_sign = g_strdup_printf (
        "%s\n"  // HTTP-Verb + "\n"
        "%s\n"  // Content-MD5 + "\n"
        "%s\n"  // Content-Type + "\n"
        "%s\n"  // Date + "\n"
        "%s"    // CanonicalizedAmzHeaders
        "%s",    // CanonicalizedResource

        method, content_md5, content_type, time_str, s_headers->str, tmp
    );
    g_string_free (s_headers, TRUE);
    g_free (content_md5);
    g_free (content_type);

    g_free (tmp);

    //LOG_debug (CON_LOG, "%s %s", string_to_sign, conf_get_string (application_get_conf (con->app), "s3.secret_access_key"));

    HMAC (EVP_sha1(),
        conf_get_string (application_get_conf (app), "s3.secret_access_key"),
        strlen (conf_get_string (application_get_conf (app), "s3.secret_access_key")),
        (unsigned char *)string_to_sign, strlen (string_to_sign),
        md, &md_len
    );
    g_free (string_to_sign);

    return get_base64 ((const gchar *)md, md_len);
}
/*}}}*/

static gchar *get_endpoint (const char *xml, size_t xml_len) {
    xmlDocPtr doc;
    xmlXPathContextPtr ctx;
    xmlXPathObjectPtr endpoint_xp;
    xmlNodeSetPtr nodes;
    gchar *endpoint = NULL;

    doc = xmlReadMemory (xml, xml_len, "", NULL, 0);
    ctx = xmlXPathNewContext (doc);
    endpoint_xp = xmlXPathEvalExpression ((xmlChar *) "/Error/Endpoint", ctx);
    nodes = endpoint_xp->nodesetval;

    if (!nodes || nodes->nodeNr < 1) {
        endpoint = NULL;
    } else {
        endpoint = (char *) xmlNodeListGetString (doc, nodes->nodeTab[0]->xmlChildrenNode, 1);
    }

    xmlXPathFreeObject (endpoint_xp);
    xmlXPathFreeContext (ctx);
    xmlFreeDoc (doc);

    return endpoint;
}

static gchar *parse_aws_error (const char *xml, size_t xml_len) {
    xmlDocPtr doc;
    xmlXPathContextPtr ctx;
    xmlXPathObjectPtr msg_xp;
    xmlNodeSetPtr nodes;
    gchar *msg;

    if (!xml_len || !xml)
        return NULL;

    printf("%s\n", xml);

    doc = xmlReadMemory (xml, xml_len, "", NULL, 0);
    if (!doc)
        return NULL;

    ctx = xmlXPathNewContext (doc);
    msg_xp = xmlXPathEvalExpression ((xmlChar *) "/Error/Message", ctx);
    nodes = msg_xp->nodesetval;

    if (!nodes || nodes->nodeNr < 1) {
        msg = NULL;
    } else {
        msg = (char *) xmlNodeListGetString (doc, nodes->nodeTab[0]->xmlChildrenNode, 1);
    }

    xmlXPathFreeObject (msg_xp);
    xmlXPathFreeContext (ctx);
    xmlFreeDoc (doc);

    return msg;
}

typedef struct {
    HttpConnection *con;
    HttpConnection_response_cb response_cb;
    gpointer ctx;

    // number of redirects so far
    gint redirects;

    // original values
    gchar *resource_path;
    gchar *http_cmd;
    struct evbuffer *out_buffer;
    size_t out_size;

    struct timeval start_tv;

    gint retry_id; // the number of retries
    gboolean enable_retry;

    GList *l_output_headers;
} RequestData;

static void request_data_free (RequestData *data)
{
    http_connection_free_headers (data->l_output_headers);
    evbuffer_free (data->out_buffer);
    g_free (data->resource_path);
    g_free (data->http_cmd);
    g_free (data);
}

static void http_connection_on_response_cb (struct evhttp_request *req, void *ctx)
{
    RequestData *data = (RequestData *) ctx;
    HttpConnection *con;
    struct evbuffer *inbuf;
    const char *buf = NULL;
    size_t buf_len = 0;
    struct timeval end_tv;
    gchar *s_history;
    char ts[50];
    guint diff_sec = 0;
    const gchar *range_str = NULL;

    con = data->con;

    gettimeofday (&end_tv, NULL);
    con->cur_time_stop = time (NULL);

    if (con->cur_time_start) {
        struct tm *cur_p;
        struct tm cur;

        localtime_r (&con->cur_time_start, &cur);
        cur_p = &cur;
        if (!strftime (ts, sizeof (ts), "%H:%M:%S", cur_p))
            ts[0] = '\0';
    } else {
        strcpy (ts, "");
    }

    if (con->cur_time_start && con->cur_time_start < con->cur_time_stop)
        diff_sec = con->cur_time_stop - con->cur_time_start;

    if (req) {
        struct evkeyvalq *output_headers;
        struct evkeyvalq *input_headers;
        struct evkeyval *header;

        inbuf = evhttp_request_get_input_buffer (req);
        buf_len = evbuffer_get_length (inbuf);

        output_headers = evhttp_request_get_output_headers (req);
        if (output_headers) {
            range_str = evhttp_find_header (output_headers, "Range");

            // get the size of Output headers
            TAILQ_FOREACH(header, output_headers, next) {
                con->total_bytes_out += strlen (header->key) + strlen (header->value);
            }
        }

        con->cur_code = evhttp_request_get_response_code (req);

        // get the size of Input headers
        input_headers = evhttp_request_get_input_headers (req);
        if (input_headers) {
            TAILQ_FOREACH(header, input_headers, next) {
                con->total_bytes_in += strlen (header->key) + strlen (header->value);
            }
        }

        con->total_bytes_in += buf_len;

    } else {
        con->cur_code = 500;
    }

    s_history = g_strdup_printf ("[%p] %s (%u sec) %s %s %s   HTTP Code: %d (Sent: %zu Received: %zu bytes)",
        (void *)con,
        ts, diff_sec, data->http_cmd, data->con->cur_url,
        range_str ? range_str : "",
        con->cur_code,
        data->out_size,
        buf_len
    );

    stats_srv_add_op_history (application_get_stat_srv (data->con->app), s_history);
    g_free (s_history);

    LOG_debug (CON_LOG, CON_H"Got HTTP response from server! (%"G_GUINT64_FORMAT"msec)",
        (void *)con, timeval_diff (&data->start_tv, &end_tv));

    if (!req) {
        LOG_err (CON_LOG, CON_H"Request failed !", (void *)con);
        con->errors_nr++;

#ifdef SSL_ENABLED
    { unsigned long oslerr;
      while ((oslerr = bufferevent_get_openssl_error (evhttp_connection_get_bufferevent (data->con->evcon))))
        { char b[128];
          ERR_error_string_n (oslerr, b, sizeof (b));
          LOG_err (CON_LOG, CON_H"SSL error: %s", (void *)con, b);
        }
    }
#endif

        if (data->enable_retry) {
            data->retry_id++;
            LOG_err (CON_LOG, CON_H"Server returned HTTP error ! Retry ID: %d of %d",
                (void *)con, data->retry_id,
                conf_get_int (application_get_conf (data->con->app), "connection.max_retries"));

            if (data->retry_id >= conf_get_int (application_get_conf (data->con->app), "connection.max_retries")) {
                LOG_err (CON_LOG, CON_H"Reached the maximum number of retries !", (void *)con);
                if (data->response_cb)
                    data->response_cb (data->con, data->ctx, FALSE, NULL, 0, NULL);
            } else {
                if (!http_connection_make_request (data->con, data->resource_path, data->http_cmd, data->out_buffer, data->enable_retry, data,
                    data->response_cb, data->ctx)) {
                    LOG_err (CON_LOG, CON_H"Failed to send request !", (void *)con);
                    if (data->response_cb)
                        data->response_cb (data->con, data->ctx, FALSE, NULL, 0, NULL);
                } else {
                    return;
                }
            }
        } else {
            if (data->response_cb)
                data->response_cb (data->con, data->ctx, FALSE, NULL, 0, NULL);
        }
        goto done;
    }

    // check if we reached maximum redirect count
    if (data->redirects > conf_get_int (application_get_conf (data->con->app), "connection.max_redirects")) {
        LOG_err (CON_LOG, CON_H"Too many redirects !", (void *)con);
        con->errors_nr++;
        if (data->response_cb)
            data->response_cb (data->con, data->ctx, FALSE, NULL, 0, NULL);
        goto done;
    }

    // handle redirect
    if (evhttp_request_get_response_code (req) == 301 ||
        evhttp_request_get_response_code (req) == 307) {
        const gchar *loc;
        struct evkeyvalq *headers;
        gboolean free_loc = FALSE;

        data->redirects++;
        headers = evhttp_request_get_input_headers (req);

        loc = http_find_header (headers, "Location");
        if (!loc) {
            inbuf = evhttp_request_get_input_buffer (req);
            buf_len = evbuffer_get_length (inbuf);
            buf = (const char *) evbuffer_pullup (inbuf, buf_len);

            // let's parse XML
            loc = get_endpoint (buf, buf_len);
            free_loc = TRUE;

            if (!loc) {
                LOG_err (CON_LOG, CON_H"Redirect URL not found !", (void *)con);
                if (data->response_cb)
                    data->response_cb (data->con, data->ctx, FALSE, NULL, 0, NULL);
                goto done;
            }
        }

        LOG_debug (CON_LOG, CON_H"New URL: %s", (void *)con, loc);

        if (!application_set_url (data->con->app, loc)) {
            if (data->response_cb)
                data->response_cb (data->con, data->ctx, FALSE, NULL, 0, NULL);
            goto done;
        }

        // free loc if it's parsed from xml
        if (free_loc)
            xmlFree ((char *)loc);

        if (!http_connection_init (data->con)) {
            if (data->response_cb)
                data->response_cb (data->con, data->ctx, FALSE, NULL, 0, NULL);
            goto done;
        }

        // re-send request
        if (!http_connection_make_request (data->con, data->resource_path, data->http_cmd, data->out_buffer, data->enable_retry, data,
            data->response_cb, data->ctx)) {
            LOG_err (CON_LOG, CON_H"Failed to send request !", (void *)con);
            if (data->response_cb)
                data->response_cb (data->con, data->ctx, FALSE, NULL, 0, NULL);
        } else {
            return;
        }
        goto done;
    }

    inbuf = evhttp_request_get_input_buffer (req);
    buf_len = evbuffer_get_length (inbuf);
    buf = (const char *) evbuffer_pullup (inbuf, buf_len);

    // OK codes are:
    // 200
    // 204 (No Content)
    // 206 (Partial Content)
    if (evhttp_request_get_response_code (req) != 200 &&
        evhttp_request_get_response_code (req) != 204 &&
        evhttp_request_get_response_code (req) != 206) {
        gchar *msg;

        // if it contains any readable information
        msg = parse_aws_error (buf, buf_len);

        LOG_debug (CON_LOG, CON_H"Server returned HTTP error: %d (%s). AWS message: %s",
            (void *)con, evhttp_request_get_response_code (req), req->response_code_line, msg);

        if (msg)
            xmlFree (msg);

        con->errors_nr++;

        if (data->enable_retry) {
            data->retry_id++;
            LOG_err (CON_LOG, CON_H"Server returned HTTP error: %d (%s)! Retry ID: %d of %d",
                (void *)con, evhttp_request_get_response_code (req), req->response_code_line, data->retry_id,
                conf_get_int (application_get_conf (data->con->app), "connection.max_retries"));

            if (data->retry_id >= conf_get_int (application_get_conf (data->con->app), "connection.max_retries")) {
                LOG_err (CON_LOG, CON_H"Reached the maximum number of retries !", (void *)con);
                if (data->response_cb)
                    data->response_cb (data->con, data->ctx, FALSE, NULL, 0, NULL);
            } else {
                if (!http_connection_make_request (data->con, data->resource_path, data->http_cmd, data->out_buffer, data->enable_retry, data,
                    data->response_cb, data->ctx)) {
                    LOG_err (CON_LOG, CON_H"Failed to send request !", (void *)con);
                    if (data->response_cb)
                        data->response_cb (data->con, data->ctx, FALSE, NULL, 0, NULL);
                } else {
                    return;
                }
            }
        } else {
            if (data->response_cb)
                data->response_cb (data->con, data->ctx, FALSE, NULL, 0, NULL);
        }
        goto done;
    }


    if (data->response_cb)
        data->response_cb (data->con, data->ctx, TRUE, buf, buf_len, evhttp_request_get_input_headers (req));
    else
        LOG_debug (CON_LOG, CON_H"NO callback function !", (void *)con);

done:
    request_data_free (data);
}

static gint hdr_compare (const HttpConnectionHeader *a, const HttpConnectionHeader *b)
{
    return strcmp (a->key, b->key);
}
// add an header to the outgoing request
void http_connection_add_output_header (HttpConnection *con, const gchar *key, const gchar *value)
{
    HttpConnectionHeader *header;

    header = g_new0 (HttpConnectionHeader, 1);
    header->key = g_strdup (key);
    header->value = g_strdup (value);

    con->l_output_headers = g_list_insert_sorted (con->l_output_headers, header, (GCompareFunc) hdr_compare);
}

static void http_connection_free_headers (GList *l_headers)
{
    GList *l;
    for (l = g_list_first (l_headers); l; l = g_list_next (l)) {
        HttpConnectionHeader *header = (HttpConnectionHeader *) l->data;
        g_free (header->key);
        g_free (header->value);
        g_free (header);
    }

    g_list_free (l_headers);
}

bool contains_header(const GList* list, const gchar* headerkey)
{
    GList *l;
    for (l = g_list_first (list); l; l = g_list_next (l)) {
        HttpConnectionHeader *header = (HttpConnectionHeader *) l->data;
        if(strcicmp(header->key, headerkey) == 0)
        {
            return true;
        }
    }
    
    return false;
}


gboolean http_connection_make_request (HttpConnection *con,
    const gchar *resource_path,
    const gchar *http_cmd,
    struct evbuffer *out_buffer,
    gboolean enable_retry, gpointer parent_request_data,
    HttpConnection_response_cb response_cb,
    gpointer ctx)
{
    gchar *auth_str;
    struct evhttp_request *req;
    gchar *auth_key;
    time_t t;
    char time_str[50];
    RequestData *data;
    int res;
    enum evhttp_cmd_type cmd_type;
    gchar *request_str = NULL;
    GList *l;
    GList* _request_headers = NULL;
    const gchar *bucket_name;
    const gchar *host;
    HttpConnectionHeader* _header;

    if (!con->evcon)
        if (!http_connection_init (con)) {
            LOG_err (CON_LOG, CON_H"Failed to init HTTP connection !", (void *)con);
            if (response_cb)
                response_cb (con, ctx, FALSE, NULL, 0, NULL);
            return FALSE;
        }

    bool _useawsv4 = conf_get_boolean (application_get_conf (con->app), "s3.use_awsv4");

    // if this is the first request
    if (!parent_request_data) {

        data = g_new0 (RequestData, 1);
        data->redirects = 0;
        data->resource_path = url_escape (resource_path);
        data->http_cmd = g_strdup (http_cmd);
        data->out_buffer = evbuffer_new ();
        if (out_buffer) {
            gchar *tmp;

            data->out_size = evbuffer_get_length (out_buffer);

            tmp = g_new0 (gchar, data->out_size);
            evbuffer_copyout (out_buffer, tmp, data->out_size);
            evbuffer_add (data->out_buffer, tmp, data->out_size);

            g_free (tmp);
        } else
            data->out_size = 0;

        data->retry_id = 0;
        data->enable_retry = enable_retry;

        data->l_output_headers = NULL;

        // save headers
        if (con->l_output_headers) {
            for (l = g_list_first (con->l_output_headers); l; l = g_list_next (l)) {
                HttpConnectionHeader *in_header = (HttpConnectionHeader *) l->data;
                HttpConnectionHeader *out_header = g_new0 (HttpConnectionHeader, 1);

                out_header->key = g_strdup (in_header->key);
                out_header->value = g_strdup (in_header->value);

                data->l_output_headers = g_list_append (data->l_output_headers, out_header);
            }

            http_connection_free_headers (con->l_output_headers);
            con->l_output_headers = NULL;
        }
    } else
        data = (RequestData *) parent_request_data;

    data->response_cb = response_cb;
    data->ctx = ctx;
    data->con = con;

    if (!strcasecmp (http_cmd, "GET")) {
        cmd_type = EVHTTP_REQ_GET;
    } else if (!strcasecmp (http_cmd, "PUT")) {
        cmd_type = EVHTTP_REQ_PUT;
    } else if (!strcasecmp (http_cmd, "POST")) {
        cmd_type = EVHTTP_REQ_POST;
    } else if (!strcasecmp (http_cmd, "DELETE")) {
        cmd_type = EVHTTP_REQ_DELETE;
    } else if (!strcasecmp (http_cmd, "HEAD")) {
        cmd_type = EVHTTP_REQ_HEAD;
    } else {
        LOG_err (CON_LOG, CON_H"Unsupported HTTP method: %s", (void *)con, http_cmd);
        if (data->response_cb)
            data->response_cb (data->con, data->ctx, FALSE, NULL, 0, NULL);
        request_data_free (data);
        return FALSE;
    }

    t = time (NULL);
    if (!strftime (time_str, sizeof (time_str), "%a, %d %b %Y %H:%M:%S GMT", gmtime(&t))) {
        LOG_err (CON_LOG, CON_H"strftime returned error !", (void *)con);
        if (data->response_cb)
            data->response_cb (data->con, data->ctx, FALSE, NULL, 0, NULL);
        request_data_free (data);
        return FALSE;
    }


    req = evhttp_request_new (http_connection_on_response_cb, data);
    if (!req) {
        LOG_err (CON_LOG, CON_H"Failed to create HTTP request object !", (void *)con);
        if (data->response_cb)
            data->response_cb (data->con, data->ctx, FALSE, NULL, 0, NULL);
        request_data_free (data);
        return FALSE;
    }

    // introduced _request_headers as a temporary structure so that AWSV4 can manipulate headers...
    for (l = g_list_first (data->l_output_headers); l; l = g_list_next (l)) {
        _header = g_malloc(sizeof(HttpConnectionHeader));
        _header->key = g_strdup(((HttpConnectionHeader*)l->data)->key);
        _header->value = g_strdup(((HttpConnectionHeader*)l->data)->value);

        _request_headers = g_list_prepend(_request_headers, _header);
    }

    if(contains_header(_request_headers,"Host") == false)
    {
        _header = g_malloc(sizeof(HttpConnectionHeader));
        _header->key = g_strdup("Host");
        _header->value = g_strdup(conf_get_string (application_get_conf (con->app), "s3.host"));
        _request_headers = g_list_prepend(_request_headers, _header);
    }

    // ask to keep connection opened
    if(contains_header(_request_headers,"Connection") == false)
    {
        _header = g_malloc(sizeof(HttpConnectionHeader));
        _header->key = g_strdup("Connection");
        _header->value = g_strdup("keep-alive");
        _request_headers = g_list_prepend(_request_headers, _header);
    }


    if(contains_header(_request_headers,"Accept-Encoding") == false)
    {
        _header = g_malloc(sizeof(HttpConnectionHeader));
        _header->key = g_strdup("Accept-Encoding");
        _header->value = g_strdup("identity");
        _request_headers = g_list_prepend(_request_headers, _header);
    }


    if(_useawsv4 == true)
    {        
        // this header may have been added before, however, if it's not there lets add it with an empty payload...
        if(contains_header(_request_headers,"x-amz-content-sha256") == false)
        {
            _header = g_malloc(sizeof(HttpConnectionHeader));
            _header->key = g_strdup("x-amz-content-sha256");
            _header->value = sha256_base16("", 0);
            _request_headers = g_list_prepend(_request_headers, _header);
        }

        // required header...
        if(contains_header(_request_headers,"x-amz-date") == false)
        {
            _header = g_malloc(sizeof(HttpConnectionHeader));
            _header->key = g_strdup("x-amz-date");
            _header->value = ISO8601_date(&t);

            _request_headers = g_list_prepend(_request_headers, _header);
        }
    }


    if (out_buffer) {
        con->total_bytes_out += evbuffer_get_length (out_buffer);
        // we don't want to destruct the original buffer
        evbuffer_add (req->output_buffer, evbuffer_pullup (out_buffer, -1), evbuffer_get_length (out_buffer));
        // a better approach, but requires "beta" libevent version
        // evbuffer_add_buffer_reference (req->output_buffer, out_buffer);
    }

    bucket_name = conf_get_string (application_get_conf (con->app), "s3.bucket_name");
    host = conf_get_string (application_get_conf (con->app), "s3.host");

    if (!strncasecmp (bucket_name, host, strlen (bucket_name))) {
        request_str = g_strdup_printf ("%s", data->resource_path);
    } else {
        request_str = g_strdup_printf ("/%s%s", bucket_name, data->resource_path);
    }

    LOG_msg (CON_LOG, CON_H"%s %s  bucket: %s, host: %s, out_len: %zd", (void *)con,
        http_cmd, request_str, bucket_name, host,
        out_buffer ? evbuffer_get_length (out_buffer) : 0);

    // update stats info
    con->cur_cmd_type = cmd_type;
    if (con->cur_url)
        g_free (con->cur_url);

        
    con->cur_url = g_strdup_printf ("%s%s%s", "http://", conf_get_string (application_get_conf (con->app), "s3.host"), request_str);

    if(_useawsv4 == true)
    {
        auth_str = http_connection_get_auth_string_v4 (con, http_cmd, data->resource_path, t, _request_headers);
        auth_key = g_strdup(auth_str);
    }
    else
    {
        auth_str = http_connection_get_auth_string (con->app, http_cmd, data->resource_path, time_str, data->l_output_headers);
        auth_key = g_malloc(sizeof(gchar)*256);
        snprintf (auth_key, 255, "AWS %s:%s", conf_get_string (application_get_conf (con->app), "s3.access_key_id"), auth_str);
    }

    evhttp_add_header (req->output_headers, "Authorization", auth_key);

    g_free (auth_str);
    g_free(auth_key);

    for (l = g_list_first (_request_headers); l; l = g_list_next (l)) {
        _header = (HttpConnectionHeader *) l->data;
        evhttp_add_header (req->output_headers, _header->key, _header->value);

        g_free(_header->key);
        g_free(_header->value);
        g_free(_header);
    }
    g_list_free(g_list_first (_request_headers));

    gettimeofday (&data->start_tv, NULL);
    con->cur_time_start = time (NULL);
    con->jobs_nr++;

    res = evhttp_make_request (http_connection_get_evcon (con), req, cmd_type, request_str);
    g_free (request_str);

    if (res < 0) {
        if (data->response_cb)
            data->response_cb (data->con, data->ctx, FALSE, NULL, 0, NULL);
        request_data_free (data);
        return FALSE;
    } else
        return TRUE;
}

// return string with various statistics information
void http_connection_get_stats_info_caption (G_GNUC_UNUSED gpointer client, GString *str, struct PrintFormat *print_format)
{
    g_string_append_printf (str,
        "%s ID %s Current state %s Last CMD %s Last URL %s Last Code %s Time started (Response sec) %s Jobs (Errors) %s Connects Nr %s Total Out %s Total In %s"
        ,
        print_format->caption_start,
        print_format->caption_col_div, print_format->caption_col_div, print_format->caption_col_div, print_format->caption_col_div,
        print_format->caption_col_div, print_format->caption_col_div, print_format->caption_col_div, print_format->caption_col_div,
        print_format->caption_col_div,
        print_format->caption_end
    );
}

void http_connection_get_stats_info_data (gpointer client, GString *str, struct PrintFormat *print_format)
{
    HttpConnection *con = (HttpConnection *) client;
    gchar cmd[10];
    char ts[50];
    guint diff_sec = 0;

    switch (con->cur_cmd_type) {
        case EVHTTP_REQ_GET:
            strcpy (cmd, "GET");
            break;
        case EVHTTP_REQ_PUT:
            strcpy (cmd, "PUT");
            break;
        case EVHTTP_REQ_POST:
            strcpy (cmd, "POST");
            break;
        case EVHTTP_REQ_DELETE:
            strcpy (cmd, "DELETE");
            break;
        case EVHTTP_REQ_HEAD:
            strcpy (cmd, "HEAD");
            break;
        default:
            strcpy (cmd, "-");
    }

    if (con->cur_time_start) {
        struct tm *cur_p;
        struct tm cur;

        localtime_r (&con->cur_time_start, &cur);
        cur_p = &cur;
        if (!strftime (ts, sizeof (ts), "%H:%M:%S", cur_p))
            ts[0] = '\0';
    } else {
        strcpy (ts, "");
    }

    if (con->cur_time_start && con->cur_time_start < con->cur_time_stop)
        diff_sec = con->cur_time_stop - con->cur_time_start;

    g_string_append_printf (str,
        "%s" // header
        "%p %s" // ID
        "%s %s" // State
        "%s %s" // CMD
        "%s %s" // URL
        "%d %s" // Code
        "%s (%u sec) %s" // Time
        "%"G_GUINT64_FORMAT" (%"G_GUINT64_FORMAT") %s" // Jobs nr
        "%"G_GUINT64_FORMAT" %s" // Connects nr
        "~ %"G_GUINT64_FORMAT" b %s" // Total Out
        "~ %"G_GUINT64_FORMAT" b" // Total In
        "%s" // footer
        ,
        print_format->row_start,
        (void *)con, print_format->col_div,
        con->is_acquired ? "Busy" : "Idle", print_format->col_div,
        cmd, print_format->col_div,
        con->cur_url ? con->cur_url : "-",  print_format->col_div,
        con->cur_code, print_format->col_div,
        ts, diff_sec, print_format->col_div,
        con->jobs_nr, con->errors_nr, print_format->col_div,
        con->connects_nr, print_format->col_div,
        con->total_bytes_out, print_format->col_div,
        con->total_bytes_in,
        print_format->row_end
    );
}
