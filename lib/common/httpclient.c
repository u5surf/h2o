/*
 * Copyright (c) 2018 Ichito Nagata, Fastly, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "h2o/httpclient_internal.h"
#include "h2o/httpclient_internal_h1.h"
#include "h2o/httpclient_internal_h2.h"

const char *const h2o_httpclient_error_is_eos = "end of stream";
const char *const h2o_httpclient_error_refused_stream = "refused stream";

void h2o_httpclient_connection_pool_init(h2o_httpclient_connection_pool_t *connpool, h2o_socketpool_t *sockpool)
{
    connpool->socketpool = sockpool;
    h2o_linklist_init_anchor(&connpool->http2.conns);
}

static void close_client(struct st_h2o_httpclient_private_t *client)
{
    if (client->connect_req != NULL) {
        h2o_socketpool_cancel_connect(client->connect_req);
        client->connect_req = NULL;
    }

    if (h2o_timeout_is_linked(&client->connect_timeout_entry))
        h2o_timeout_unlink(&client->connect_timeout_entry);

    free(client);
}

static void on_connect_error(struct st_h2o_httpclient_private_t *client, const char *errstr)
{
    assert(errstr != NULL);
    client->cb.on_connect(&client->super, errstr, NULL, NULL, NULL, 0, NULL, NULL, NULL, NULL);
    close_client(client);
}

static void on_connect_timeout(h2o_timeout_entry_t *entry)
{
    struct st_h2o_httpclient_private_t *client = H2O_STRUCT_FROM_MEMBER(struct st_h2o_httpclient_private_t, connect_timeout_entry, entry);
    on_connect_error(client, "connection timeout");
}

static void do_cancel(h2o_httpclient_t *_client)
{
    struct st_h2o_httpclient_private_t *client = (void *)_client;
    close_client(client);
}

static struct st_h2o_httpclient_private_t *create_client(h2o_mem_pool_t *pool, void *data, h2o_httpclient_ctx_t *ctx, h2o_httpclient_connect_cb cb)
{
#define SZ_MAX(x, y) ((x) > (y) ? (x) : (y))
    size_t sz = SZ_MAX(sizeof(struct st_h2o_http1client_t), sizeof(struct st_h2o_http2client_stream_t));
#undef SZ_MAX
    struct st_h2o_httpclient_private_t *client = h2o_mem_alloc(sz);
    memset(client, 0, sizeof(*client));
    client->super.pool = pool;
    client->super.ctx = ctx;
    client->super.data = data;
    client->super.cancel = do_cancel;
    client->super.steal_socket = NULL;
    client->super.update_window = NULL;
    client->super.write_req = NULL;
    client->cb.on_connect = cb;
    client->connect_timeout_entry.cb = on_connect_timeout;

    return client;
}

static void on_pool_connect(h2o_socket_t *sock, const char *errstr, void *data, h2o_url_t *origin)
{
    struct st_h2o_httpclient_private_t *client = data;

    h2o_timeout_unlink(&client->connect_timeout_entry);

    client->connect_req = NULL;

    if (sock == NULL) {
        assert(errstr != NULL);
        on_connect_error(client, errstr);
        return;
    }

    h2o_iovec_t alpn_proto;
    if (sock->ssl == NULL || (alpn_proto = h2o_socket_ssl_get_selected_protocol(sock)).len == 0) {
        h2o_http1client_on_connect(client, sock, origin);
    } else {
        if (memcmp(alpn_proto.base, "h2", alpn_proto.len) == 0) {
            /* detach this socket from the socketpool to count the number of h1 connections correctly */
            h2o_socketpool_detach(client->super.connpool->socketpool, sock);
            h2o_http2client_on_connect(client, sock, origin);
        } else if (memcmp(alpn_proto.base, "http/1.1", alpn_proto.len) == 0) {
            h2o_http1client_on_connect(client, sock, origin);
        } else {
            on_connect_error(client, "unknown alpn protocol");
        }
    }
}

static int should_use_h2(int8_t ratio, int8_t *counter)
{
    /* weighted fair queueing */
    if (*counter < 0)
        *counter = ratio == 0 ? 0 : 50 / ratio; /* set initial counter value */
    int use_h2 = (((int)ratio * *counter) % 100) + ratio >= 100;
    if (++*counter == 100)
        *counter = 0;
    return use_h2;
}

void h2o_httpclient_connect(h2o_httpclient_t **_client, h2o_mem_pool_t *pool, void *data, h2o_httpclient_ctx_t *ctx, h2o_httpclient_connection_pool_t *connpool,
                            h2o_url_t *origin, h2o_httpclient_connect_cb cb)
{
    static const h2o_iovec_t both_protos = {H2O_STRLIT(
                                                       "\x02"
                                                       "h2"
                                                       "\x08"
                                                       "http/1.1"
                                                       )};
    assert(connpool != NULL);
    h2o_iovec_t alpn_protos = h2o_iovec_init(NULL, 0);

    struct st_h2o_httpclient_private_t *client = create_client(pool, data, ctx, cb);
    client->super.connpool = connpool;
    if (_client != NULL)
        *_client = &client->super;

    struct st_h2o_http2client_conn_t *http2_conn = NULL;
    if (!h2o_linklist_is_empty(&connpool->http2.conns)) {
        http2_conn = H2O_STRUCT_FROM_MEMBER(struct st_h2o_http2client_conn_t, link, connpool->http2.conns.next);
        if (http2_conn->num_streams >= h2o_http2client_get_max_concurrent_streams(http2_conn))
            http2_conn = NULL;
    }

    if (ctx->http2.ratio < 0) {
        /* mix mode */

        if (http2_conn != NULL && connpool->socketpool->_shared.pooled_count != 0) {
            /* both of h1 and h2 connections exist, compare in-use ratio */
            double http1_ratio = (double)(connpool->socketpool->_shared.count - connpool->socketpool->_shared.pooled_count) / connpool->socketpool->_shared.count;
            double http2_ratio = http2_conn->num_streams / h2o_http2client_get_max_concurrent_streams(http2_conn);
            if (http2_ratio <= http1_ratio) {
                h2o_http2client_on_connect(client, http2_conn->sock, &http2_conn->origin_url);
            } else {
                goto UseSocketPool;
            }
        } else if (http2_conn != NULL) {
            /* h2 connection exists */
            h2o_http2client_on_connect(client, http2_conn->sock, &http2_conn->origin_url);
        } else if (connpool->socketpool->_shared.pooled_count != 0) {
            /* h1 connection exists */
            goto UseSocketPool;
        } else {
            /* no connections, connect using ALPN */
            alpn_protos = both_protos;
            goto UseSocketPool;
        }
    } else {
        /* fixed ratio mode */

        if (should_use_h2(ctx->http2.ratio, &ctx->http2.counter)) {
            if (http2_conn != NULL) {
                h2o_http2client_on_connect(client, http2_conn->sock, &http2_conn->origin_url);
            } else {
                alpn_protos = both_protos;
                goto UseSocketPool;
            }
        } else {
            goto UseSocketPool;
        }
    }

    return;

UseSocketPool:
    h2o_timeout_link(client->super.ctx->loop, client->super.ctx->connect_timeout, &client->connect_timeout_entry);
    h2o_socketpool_connect(&client->connect_req, connpool->socketpool, origin, ctx->loop, ctx->getaddr_receiver, alpn_protos, on_pool_connect, client);

}
