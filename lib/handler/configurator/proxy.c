/*
 * Copyright (c) 2014 DeNA Co., Ltd.
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
#include <inttypes.h>
#include <errno.h>
#include <stdio.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include "h2o.h"
#include "h2o/configurator.h"

struct proxy_configurator_t {
    h2o_configurator_t super;
    h2o_proxy_config_vars_t *vars;
    h2o_proxy_config_vars_t _vars_stack[H2O_CONFIGURATOR_NUM_LEVELS + 1];
};

static int on_config_timeout_io(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    struct proxy_configurator_t *self = (void *)cmd->configurator;
    return h2o_configurator_scanf(cmd, node, "%" PRIu64, &self->vars->io_timeout);
}

static int on_config_timeout_keepalive(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    struct proxy_configurator_t *self = (void *)cmd->configurator;
    return h2o_configurator_scanf(cmd, node, "%" PRIu64, &self->vars->keepalive_timeout);
}

static int on_config_preserve_host(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    struct proxy_configurator_t *self = (void *)cmd->configurator;
    ssize_t ret = h2o_configurator_get_one_of(cmd, node, "OFF,ON");
    if (ret == -1)
        return -1;
    self->vars->preserve_host = (int)ret;
    return 0;
}

static int on_config_websocket_timeout(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    struct proxy_configurator_t *self = (void *)cmd->configurator;
    return h2o_configurator_scanf(cmd, node, "%" PRIu64, &self->vars->websocket.timeout);
}

static int on_config_websocket(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    struct proxy_configurator_t *self = (void *)cmd->configurator;
    ssize_t ret = h2o_configurator_get_one_of(cmd, node, "OFF,ON");
    if (ret == -1)
        return -1;
    self->vars->websocket.enabled = (int)ret;
    return 0;
}

static void clone_ssl_ctx(SSL_CTX **slot)
{
    if (*slot == NULL) {
        /* the defaults */
        *slot = SSL_CTX_new(TLSv1_client_method());
        SSL_CTX_set_verify(*slot, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
        if (X509_STORE_load_locations((*slot)->cert_store, H2O_TO_STR(H2O_ROOT) "/share/h2o/ca-bundle.crt", NULL) != 1)
            fprintf(stderr, "Warning: failed to load the default certificates file at " H2O_TO_STR(
                                H2O_ROOT) "/share/h2o/ca-bundle.crt. Proxying to HTTPS servers may fail.\n");
        return;
    }

    /* create a duplicate */
    SSL_CTX *ctx = SSL_CTX_new((*slot)->method);
    if (ctx->cert_store != NULL)
        X509_STORE_free(ctx->cert_store);
    ctx->cert_store = (*slot)->cert_store;
    CRYPTO_add(&ctx->cert_store->references, 1, CRYPTO_LOCK_X509_STORE);
    SSL_CTX_set_verify(ctx, (*slot)->verify_mode, NULL);

    /* replate the one in the slot */
    SSL_CTX_free(*slot);
    *slot = ctx;
}

static int on_config_ssl_verify_peer(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    struct proxy_configurator_t *self = (void *)cmd->configurator;
    ssize_t ret = h2o_configurator_get_one_of(cmd, node, "OFF,ON");
    if (ret == -1)
        return -1;

    clone_ssl_ctx(&self->vars->ssl_ctx);
    SSL_CTX_set_verify(self->vars->ssl_ctx, ret != 0 ? SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT : SSL_VERIFY_NONE, NULL);

    return 0;
}

static int on_config_ssl_cafile(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    struct proxy_configurator_t *self = (void *)cmd->configurator;

    clone_ssl_ctx(&self->vars->ssl_ctx);
    /* reset the store, and load the certificates file */
    X509_STORE_free(self->vars->ssl_ctx->cert_store);
    self->vars->ssl_ctx->cert_store = X509_STORE_new();
    if (X509_STORE_load_locations(self->vars->ssl_ctx->cert_store, node->data.scalar, NULL) != 1) {
        h2o_configurator_errprintf(cmd, node, "failed to load certificates file:%s", node->data.scalar);
        ERR_print_errors_fp(stderr);
        return -1;
    }

    return 0;
}

static int on_config_reverse_url(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    struct proxy_configurator_t *self = (void *)cmd->configurator;
    h2o_url_t parsed;

    if (h2o_url_parse(node->data.scalar, SIZE_MAX, &parsed) != 0) {
        h2o_configurator_errprintf(cmd, node, "failed to parse URL: %s\n", node->data.scalar);
        return -1;
    }
    /* register */
    h2o_proxy_register_reverse_proxy(ctx->pathconf, &parsed, self->vars);

    return 0;
}

static int on_config_enter(h2o_configurator_t *_self, h2o_configurator_context_t *ctx, yoml_t *node)
{
    struct proxy_configurator_t *self = (void *)_self;

    memcpy(self->vars + 1, self->vars, sizeof(*self->vars));
    ++self->vars;

    if (ctx->pathconf == NULL && ctx->hostconf == NULL) {
        /* is global conf */
        assert(self->vars->ssl_ctx == NULL);
        clone_ssl_ctx(&self->vars->ssl_ctx);
    } else {
        CRYPTO_add(&self->vars->ssl_ctx->references, 1, CRYPTO_LOCK_SSL_CTX);
    }

    return 0;
}

static int on_config_exit(h2o_configurator_t *_self, h2o_configurator_context_t *ctx, yoml_t *node)
{
    struct proxy_configurator_t *self = (void *)_self;

    if (ctx->pathconf == NULL && ctx->hostconf == NULL) {
        /* is global conf */
        ctx->globalconf->proxy.io_timeout = self->vars->io_timeout;
        ctx->globalconf->proxy.ssl_ctx = self->vars->ssl_ctx;
    } else {
        SSL_CTX_free(self->vars->ssl_ctx);
    }

    --self->vars;
    return 0;
}

void h2o_proxy_register_configurator(h2o_globalconf_t *conf)
{
    struct proxy_configurator_t *c = (void *)h2o_configurator_create(conf, sizeof(*c));

    /* set default vars */
    c->vars = c->_vars_stack;
    c->vars->io_timeout = H2O_DEFAULT_PROXY_IO_TIMEOUT;
    c->vars->keepalive_timeout = 2000;
    c->vars->websocket.enabled = 0; /* have websocket proxying disabled by default; until it becomes non-experimental */
    c->vars->websocket.timeout = H2O_DEFAULT_PROXY_WEBSOCKET_TIMEOUT;

    /* setup handlers */
    c->super.enter = on_config_enter;
    c->super.exit = on_config_exit;
    h2o_configurator_define_command(
        &c->super, "proxy.reverse.url",
        H2O_CONFIGURATOR_FLAG_PATH | H2O_CONFIGURATOR_FLAG_EXPECT_SCALAR | H2O_CONFIGURATOR_FLAG_DEFERRED, on_config_reverse_url);
    h2o_configurator_define_command(&c->super, "proxy.preserve-host",
                                    H2O_CONFIGURATOR_FLAG_ALL_LEVELS | H2O_CONFIGURATOR_FLAG_EXPECT_SCALAR,
                                    on_config_preserve_host);
    h2o_configurator_define_command(&c->super, "proxy.timeout.io",
                                    H2O_CONFIGURATOR_FLAG_ALL_LEVELS | H2O_CONFIGURATOR_FLAG_EXPECT_SCALAR, on_config_timeout_io);
    h2o_configurator_define_command(&c->super, "proxy.timeout.keepalive",
                                    H2O_CONFIGURATOR_FLAG_ALL_LEVELS | H2O_CONFIGURATOR_FLAG_EXPECT_SCALAR,
                                    on_config_timeout_keepalive);
    h2o_configurator_define_command(&c->super, "proxy.websocket",
                                    H2O_CONFIGURATOR_FLAG_ALL_LEVELS | H2O_CONFIGURATOR_FLAG_EXPECT_SCALAR, on_config_websocket);
    h2o_configurator_define_command(&c->super, "proxy.websocket.timeout",
                                    H2O_CONFIGURATOR_FLAG_ALL_LEVELS | H2O_CONFIGURATOR_FLAG_EXPECT_SCALAR,
                                    on_config_websocket_timeout);
    h2o_configurator_define_command(&c->super, "proxy.ssl.verify-peer",
                                    H2O_CONFIGURATOR_FLAG_ALL_LEVELS | H2O_CONFIGURATOR_FLAG_EXPECT_SCALAR,
                                    on_config_ssl_verify_peer);
    h2o_configurator_define_command(&c->super, "proxy.ssl.cafile",
                                    H2O_CONFIGURATOR_FLAG_ALL_LEVELS | H2O_CONFIGURATOR_FLAG_EXPECT_SCALAR, on_config_ssl_cafile);
}
