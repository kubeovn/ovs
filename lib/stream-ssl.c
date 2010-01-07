/*
 * Copyright (c) 2008, 2009, 2010 Nicira Networks.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>
#include "stream-ssl.h"
#include "dhparams.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "dynamic-string.h"
#include "leak-checker.h"
#include "ofpbuf.h"
#include "openflow/openflow.h"
#include "packets.h"
#include "poll-loop.h"
#include "socket-util.h"
#include "socket-util.h"
#include "util.h"
#include "stream-provider.h"
#include "stream.h"

#include "vlog.h"
#define THIS_MODULE VLM_stream_ssl

/* Active SSL. */

enum ssl_state {
    STATE_TCP_CONNECTING,
    STATE_SSL_CONNECTING
};

enum session_type {
    CLIENT,
    SERVER
};

struct ssl_stream
{
    struct stream stream;
    enum ssl_state state;
    int connect_error;
    enum session_type type;
    int fd;
    SSL *ssl;
    struct ofpbuf *txbuf;

    /* rx_want and tx_want record the result of the last call to SSL_read()
     * and SSL_write(), respectively:
     *
     *    - If the call reported that data needed to be read from the file
     *      descriptor, the corresponding member is set to SSL_READING.
     *
     *    - If the call reported that data needed to be written to the file
     *      descriptor, the corresponding member is set to SSL_WRITING.
     *
     *    - Otherwise, the member is set to SSL_NOTHING, indicating that the
     *      call completed successfully (or with an error) and that there is no
     *      need to block.
     *
     * These are needed because there is no way to ask OpenSSL what a data read
     * or write would require without giving it a buffer to receive into or
     * data to send, respectively.  (Note that the SSL_want() status is
     * overwritten by each SSL_read() or SSL_write() call, so we can't rely on
     * its value.)
     *
     * A single call to SSL_read() or SSL_write() can perform both reading
     * and writing and thus invalidate not one of these values but actually
     * both.  Consider this situation, for example:
     *
     *    - SSL_write() blocks on a read, so tx_want gets SSL_READING.
     *
     *    - SSL_read() laters succeeds reading from 'fd' and clears out the
     *      whole receive buffer, so rx_want gets SSL_READING.
     *
     *    - Client calls stream_wait(STREAM_RECV) and stream_wait(STREAM_SEND)
     *      and blocks.
     *
     *    - Now we're stuck blocking until the peer sends us data, even though
     *      SSL_write() could now succeed, which could easily be a deadlock
     *      condition.
     *
     * On the other hand, we can't reset both tx_want and rx_want on every call
     * to SSL_read() or SSL_write(), because that would produce livelock,
     * e.g. in this situation:
     *
     *    - SSL_write() blocks, so tx_want gets SSL_READING or SSL_WRITING.
     *
     *    - SSL_read() blocks, so rx_want gets SSL_READING or SSL_WRITING,
     *      but tx_want gets reset to SSL_NOTHING.
     *
     *    - Client calls stream_wait(STREAM_RECV) and stream_wait(STREAM_SEND)
     *      and blocks.
     *
     *    - Client wakes up immediately since SSL_NOTHING in tx_want indicates
     *      that no blocking is necessary.
     *
     * The solution we adopt here is to set tx_want to SSL_NOTHING after
     * calling SSL_read() only if the SSL state of the connection changed,
     * which indicates that an SSL-level renegotiation made some progress, and
     * similarly for rx_want and SSL_write().  This prevents both the
     * deadlock and livelock situations above.
     */
    int rx_want, tx_want;
};

/* SSL context created by ssl_init(). */
static SSL_CTX *ctx;

/* Required configuration. */
static bool has_private_key, has_certificate, has_ca_cert;

/* Ordinarily, we require a CA certificate for the peer to be locally
 * available.  'has_ca_cert' is true when this is the case, and neither of the
 * following variables matter.
 *
 * We can, however, bootstrap the CA certificate from the peer at the beginning
 * of our first connection then use that certificate on all subsequent
 * connections, saving it to a file for use in future runs also.  In this case,
 * 'has_ca_cert' is false, 'bootstrap_ca_cert' is true, and 'ca_cert_file'
 * names the file to be saved. */
static bool bootstrap_ca_cert;
static char *ca_cert_file;

/* Who knows what can trigger various SSL errors, so let's throttle them down
 * quite a bit. */
static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(10, 25);

static int ssl_init(void);
static int do_ssl_init(void);
static bool ssl_wants_io(int ssl_error);
static void ssl_close(struct stream *);
static void ssl_clear_txbuf(struct ssl_stream *);
static int interpret_ssl_error(const char *function, int ret, int error,
                               int *want);
static DH *tmp_dh_callback(SSL *ssl, int is_export UNUSED, int keylength);
static void log_ca_cert(const char *file_name, X509 *cert);

static short int
want_to_poll_events(int want)
{
    switch (want) {
    case SSL_NOTHING:
        NOT_REACHED();

    case SSL_READING:
        return POLLIN;

    case SSL_WRITING:
        return POLLOUT;

    default:
        NOT_REACHED();
    }
}

static int
new_ssl_stream(const char *name, int fd, enum session_type type,
              enum ssl_state state, const struct sockaddr_in *remote,
              struct stream **streamp)
{
    struct sockaddr_in local;
    socklen_t local_len = sizeof local;
    struct ssl_stream *sslv;
    SSL *ssl = NULL;
    int on = 1;
    int retval;

    /* Check for all the needful configuration. */
    retval = 0;
    if (!has_private_key) {
        VLOG_ERR("Private key must be configured to use SSL");
        retval = ENOPROTOOPT;
    }
    if (!has_certificate) {
        VLOG_ERR("Certificate must be configured to use SSL");
        retval = ENOPROTOOPT;
    }
    if (!has_ca_cert && !bootstrap_ca_cert) {
        VLOG_ERR("CA certificate must be configured to use SSL");
        retval = ENOPROTOOPT;
    }
    if (!SSL_CTX_check_private_key(ctx)) {
        VLOG_ERR("Private key does not match certificate public key: %s",
                 ERR_error_string(ERR_get_error(), NULL));
        retval = ENOPROTOOPT;
    }
    if (retval) {
        goto error;
    }

    /* Get the local IP and port information */
    retval = getsockname(fd, (struct sockaddr *) &local, &local_len);
    if (retval) {
        memset(&local, 0, sizeof local);
    }

    /* Disable Nagle. */
    retval = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
    if (retval) {
        VLOG_ERR("%s: setsockopt(TCP_NODELAY): %s", name, strerror(errno));
        retval = errno;
        goto error;
    }

    /* Create and configure OpenSSL stream. */
    ssl = SSL_new(ctx);
    if (ssl == NULL) {
        VLOG_ERR("SSL_new: %s", ERR_error_string(ERR_get_error(), NULL));
        retval = ENOPROTOOPT;
        goto error;
    }
    if (SSL_set_fd(ssl, fd) == 0) {
        VLOG_ERR("SSL_set_fd: %s", ERR_error_string(ERR_get_error(), NULL));
        retval = ENOPROTOOPT;
        goto error;
    }
    if (bootstrap_ca_cert && type == CLIENT) {
        SSL_set_verify(ssl, SSL_VERIFY_NONE, NULL);
    }

    /* Create and return the ssl_stream. */
    sslv = xmalloc(sizeof *sslv);
    stream_init(&sslv->stream, &ssl_stream_class, EAGAIN, name);
    stream_set_remote_ip(&sslv->stream, remote->sin_addr.s_addr);
    stream_set_remote_port(&sslv->stream, remote->sin_port);
    stream_set_local_ip(&sslv->stream, local.sin_addr.s_addr);
    stream_set_local_port(&sslv->stream, local.sin_port);
    sslv->state = state;
    sslv->type = type;
    sslv->fd = fd;
    sslv->ssl = ssl;
    sslv->txbuf = NULL;
    sslv->rx_want = sslv->tx_want = SSL_NOTHING;
    *streamp = &sslv->stream;
    return 0;

error:
    if (ssl) {
        SSL_free(ssl);
    }
    close(fd);
    return retval;
}

static struct ssl_stream *
ssl_stream_cast(struct stream *stream)
{
    stream_assert_class(stream, &ssl_stream_class);
    return CONTAINER_OF(stream, struct ssl_stream, stream);
}

static int
ssl_open(const char *name, char *suffix, struct stream **streamp)
{
    struct sockaddr_in sin;
    int error, fd;

    error = ssl_init();
    if (error) {
        return error;
    }

    error = inet_open_active(SOCK_STREAM, suffix, OFP_SSL_PORT, &sin, &fd);
    if (fd >= 0) {
        int state = error ? STATE_TCP_CONNECTING : STATE_SSL_CONNECTING;
        return new_ssl_stream(name, fd, CLIENT, state, &sin, streamp);
    } else {
        VLOG_ERR("%s: connect: %s", name, strerror(error));
        return error;
    }
}

static int
do_ca_cert_bootstrap(struct stream *stream)
{
    struct ssl_stream *sslv = ssl_stream_cast(stream);
    STACK_OF(X509) *chain;
    X509 *ca_cert;
    FILE *file;
    int error;
    int fd;

    chain = SSL_get_peer_cert_chain(sslv->ssl);
    if (!chain || !sk_X509_num(chain)) {
        VLOG_ERR("could not bootstrap CA cert: no certificate presented by "
                 "peer");
        return EPROTO;
    }
    ca_cert = sk_X509_value(chain, sk_X509_num(chain) - 1);

    /* Check that 'ca_cert' is self-signed.  Otherwise it is not a CA
     * certificate and we should not attempt to use it as one. */
    error = X509_check_issued(ca_cert, ca_cert);
    if (error) {
        VLOG_ERR("could not bootstrap CA cert: obtained certificate is "
                 "not self-signed (%s)",
                 X509_verify_cert_error_string(error));
        if (sk_X509_num(chain) < 2) {
            VLOG_ERR("only one certificate was received, so probably the peer "
                     "is not configured to send its CA certificate");
        }
        return EPROTO;
    }

    fd = open(ca_cert_file, O_CREAT | O_EXCL | O_WRONLY, 0444);
    if (fd < 0) {
        VLOG_ERR("could not bootstrap CA cert: creating %s failed: %s",
                 ca_cert_file, strerror(errno));
        return errno;
    }

    file = fdopen(fd, "w");
    if (!file) {
        int error = errno;
        VLOG_ERR("could not bootstrap CA cert: fdopen failed: %s",
                 strerror(error));
        unlink(ca_cert_file);
        return error;
    }

    if (!PEM_write_X509(file, ca_cert)) {
        VLOG_ERR("could not bootstrap CA cert: PEM_write_X509 to %s failed: "
                 "%s", ca_cert_file, ERR_error_string(ERR_get_error(), NULL));
        fclose(file);
        unlink(ca_cert_file);
        return EIO;
    }

    if (fclose(file)) {
        int error = errno;
        VLOG_ERR("could not bootstrap CA cert: writing %s failed: %s",
                 ca_cert_file, strerror(error));
        unlink(ca_cert_file);
        return error;
    }

    VLOG_INFO("successfully bootstrapped CA cert to %s", ca_cert_file);
    log_ca_cert(ca_cert_file, ca_cert);
    bootstrap_ca_cert = false;
    has_ca_cert = true;

    /* SSL_CTX_add_client_CA makes a copy of ca_cert's relevant data. */
    SSL_CTX_add_client_CA(ctx, ca_cert);

    /* SSL_CTX_use_certificate() takes ownership of the certificate passed in.
     * 'ca_cert' is owned by sslv->ssl, so we need to duplicate it. */
    ca_cert = X509_dup(ca_cert);
    if (!ca_cert) {
        out_of_memory();
    }
    if (SSL_CTX_load_verify_locations(ctx, ca_cert_file, NULL) != 1) {
        VLOG_ERR("SSL_CTX_load_verify_locations: %s",
                 ERR_error_string(ERR_get_error(), NULL));
        return EPROTO;
    }
    VLOG_INFO("killing successful connection to retry using CA cert");
    return EPROTO;
}

static int
ssl_connect(struct stream *stream)
{
    struct ssl_stream *sslv = ssl_stream_cast(stream);
    int retval;

    switch (sslv->state) {
    case STATE_TCP_CONNECTING:
        retval = check_connection_completion(sslv->fd);
        if (retval) {
            return retval;
        }
        sslv->state = STATE_SSL_CONNECTING;
        /* Fall through. */

    case STATE_SSL_CONNECTING:
        retval = (sslv->type == CLIENT
                   ? SSL_connect(sslv->ssl) : SSL_accept(sslv->ssl));
        if (retval != 1) {
            int error = SSL_get_error(sslv->ssl, retval);
            if (retval < 0 && ssl_wants_io(error)) {
                return EAGAIN;
            } else {
                int unused;
                interpret_ssl_error((sslv->type == CLIENT ? "SSL_connect"
                                     : "SSL_accept"), retval, error, &unused);
                shutdown(sslv->fd, SHUT_RDWR);
                return EPROTO;
            }
        } else if (bootstrap_ca_cert) {
            return do_ca_cert_bootstrap(stream);
        } else if ((SSL_get_verify_mode(sslv->ssl)
                    & (SSL_VERIFY_NONE | SSL_VERIFY_PEER))
                   != SSL_VERIFY_PEER) {
            /* Two or more SSL connections completed at the same time while we
             * were in bootstrap mode.  Only one of these can finish the
             * bootstrap successfully.  The other one(s) must be rejected
             * because they were not verified against the bootstrapped CA
             * certificate.  (Alternatively we could verify them against the CA
             * certificate, but that's more trouble than it's worth.  These
             * connections will succeed the next time they retry, assuming that
             * they have a certificate against the correct CA.) */
            VLOG_ERR("rejecting SSL connection during bootstrap race window");
            return EPROTO;
        } else {
            return 0;
        }
    }

    NOT_REACHED();
}

static void
ssl_close(struct stream *stream)
{
    struct ssl_stream *sslv = ssl_stream_cast(stream);
    ssl_clear_txbuf(sslv);

    /* Attempt clean shutdown of the SSL connection.  This will work most of
     * the time, as long as the kernel send buffer has some free space and the
     * SSL connection isn't renegotiating, etc.  That has to be good enough,
     * since we don't have any way to continue the close operation in the
     * background. */
    SSL_shutdown(sslv->ssl);

    SSL_free(sslv->ssl);
    close(sslv->fd);
    free(sslv);
}

static int
interpret_ssl_error(const char *function, int ret, int error,
                    int *want)
{
    *want = SSL_NOTHING;

    switch (error) {
    case SSL_ERROR_NONE:
        VLOG_ERR_RL(&rl, "%s: unexpected SSL_ERROR_NONE", function);
        break;

    case SSL_ERROR_ZERO_RETURN:
        VLOG_ERR_RL(&rl, "%s: unexpected SSL_ERROR_ZERO_RETURN", function);
        break;

    case SSL_ERROR_WANT_READ:
        *want = SSL_READING;
        return EAGAIN;

    case SSL_ERROR_WANT_WRITE:
        *want = SSL_WRITING;
        return EAGAIN;

    case SSL_ERROR_WANT_CONNECT:
        VLOG_ERR_RL(&rl, "%s: unexpected SSL_ERROR_WANT_CONNECT", function);
        break;

    case SSL_ERROR_WANT_ACCEPT:
        VLOG_ERR_RL(&rl, "%s: unexpected SSL_ERROR_WANT_ACCEPT", function);
        break;

    case SSL_ERROR_WANT_X509_LOOKUP:
        VLOG_ERR_RL(&rl, "%s: unexpected SSL_ERROR_WANT_X509_LOOKUP",
                    function);
        break;

    case SSL_ERROR_SYSCALL: {
        int queued_error = ERR_get_error();
        if (queued_error == 0) {
            if (ret < 0) {
                int status = errno;
                VLOG_WARN_RL(&rl, "%s: system error (%s)",
                             function, strerror(status));
                return status;
            } else {
                VLOG_WARN_RL(&rl, "%s: unexpected SSL connection close",
                             function);
                return EPROTO;
            }
        } else {
            VLOG_WARN_RL(&rl, "%s: %s",
                         function, ERR_error_string(queued_error, NULL));
            break;
        }
    }

    case SSL_ERROR_SSL: {
        int queued_error = ERR_get_error();
        if (queued_error != 0) {
            VLOG_WARN_RL(&rl, "%s: %s",
                         function, ERR_error_string(queued_error, NULL));
        } else {
            VLOG_ERR_RL(&rl, "%s: SSL_ERROR_SSL without queued error",
                        function);
        }
        break;
    }

    default:
        VLOG_ERR_RL(&rl, "%s: bad SSL error code %d", function, error);
        break;
    }
    return EIO;
}

static ssize_t
ssl_recv(struct stream *stream, void *buffer, size_t n)
{
    struct ssl_stream *sslv = ssl_stream_cast(stream);
    int old_state;
    ssize_t ret;

    /* Behavior of zero-byte SSL_read is poorly defined. */
    assert(n > 0);

    old_state = SSL_get_state(sslv->ssl);
    ret = SSL_read(sslv->ssl, buffer, n);
    if (old_state != SSL_get_state(sslv->ssl)) {
        sslv->tx_want = SSL_NOTHING;
    }
    sslv->rx_want = SSL_NOTHING;

    if (ret > 0) {
        return ret;
    } else {
        int error = SSL_get_error(sslv->ssl, ret);
        if (error == SSL_ERROR_ZERO_RETURN) {
            return 0;
        } else {
            return interpret_ssl_error("SSL_read", ret, error, &sslv->rx_want);
        }
    }
}

static void
ssl_clear_txbuf(struct ssl_stream *sslv)
{
    ofpbuf_delete(sslv->txbuf);
    sslv->txbuf = NULL;
}

static int
ssl_do_tx(struct stream *stream)
{
    struct ssl_stream *sslv = ssl_stream_cast(stream);

    for (;;) {
        int old_state = SSL_get_state(sslv->ssl);
        int ret = SSL_write(sslv->ssl, sslv->txbuf->data, sslv->txbuf->size);
        if (old_state != SSL_get_state(sslv->ssl)) {
            sslv->rx_want = SSL_NOTHING;
        }
        sslv->tx_want = SSL_NOTHING;
        if (ret > 0) {
            ofpbuf_pull(sslv->txbuf, ret);
            if (sslv->txbuf->size == 0) {
                return 0;
            }
        } else {
            int ssl_error = SSL_get_error(sslv->ssl, ret);
            if (ssl_error == SSL_ERROR_ZERO_RETURN) {
                VLOG_WARN_RL(&rl, "SSL_write: connection closed");
                return EPIPE;
            } else {
                return interpret_ssl_error("SSL_write", ret, ssl_error,
                                           &sslv->tx_want);
            }
        }
    }
}

static ssize_t
ssl_send(struct stream *stream, const void *buffer, size_t n)
{
    struct ssl_stream *sslv = ssl_stream_cast(stream);

    if (sslv->txbuf) {
        return EAGAIN;
    } else {
        int error;

        sslv->txbuf = ofpbuf_clone_data(buffer, n);
        error = ssl_do_tx(stream);
        switch (error) {
        case 0:
            ssl_clear_txbuf(sslv);
            return 0;
        case EAGAIN:
            leak_checker_claim(buffer);
            return 0;
        default:
            sslv->txbuf = NULL;
            return error;
        }
    }
}

static void
ssl_run(struct stream *stream)
{
    struct ssl_stream *sslv = ssl_stream_cast(stream);

    if (sslv->txbuf && ssl_do_tx(stream) != EAGAIN) {
        ssl_clear_txbuf(sslv);
    }
}

static void
ssl_run_wait(struct stream *stream)
{
    struct ssl_stream *sslv = ssl_stream_cast(stream);

    if (sslv->tx_want != SSL_NOTHING) {
        poll_fd_wait(sslv->fd, want_to_poll_events(sslv->tx_want));
    }
}

static void
ssl_wait(struct stream *stream, enum stream_wait_type wait)
{
    struct ssl_stream *sslv = ssl_stream_cast(stream);

    switch (wait) {
    case STREAM_CONNECT:
        if (stream_connect(stream) != EAGAIN) {
            poll_immediate_wake();
        } else {
            switch (sslv->state) {
            case STATE_TCP_CONNECTING:
                poll_fd_wait(sslv->fd, POLLOUT);
                break;

            case STATE_SSL_CONNECTING:
                /* ssl_connect() called SSL_accept() or SSL_connect(), which
                 * set up the status that we test here. */
                poll_fd_wait(sslv->fd,
                             want_to_poll_events(SSL_want(sslv->ssl)));
                break;

            default:
                NOT_REACHED();
            }
        }
        break;

    case STREAM_RECV:
        if (sslv->rx_want != SSL_NOTHING) {
            poll_fd_wait(sslv->fd, want_to_poll_events(sslv->rx_want));
        } else {
            poll_immediate_wake();
        }
        break;

    case STREAM_SEND:
        if (!sslv->txbuf) {
            /* We have room in our tx queue. */
            poll_immediate_wake();
        } else {
            /* stream_run_wait() will do the right thing; don't bother with
             * redundancy. */
        }
        break;

    default:
        NOT_REACHED();
    }
}

struct stream_class ssl_stream_class = {
    "ssl",                      /* name */
    ssl_open,                   /* open */
    ssl_close,                  /* close */
    ssl_connect,                /* connect */
    ssl_recv,                   /* recv */
    ssl_send,                   /* send */
    ssl_run,                    /* run */
    ssl_run_wait,               /* run_wait */
    ssl_wait,                   /* wait */
};

/* Passive SSL. */

struct pssl_pstream
{
    struct pstream pstream;
    int fd;
};

struct pstream_class pssl_pstream_class;

static struct pssl_pstream *
pssl_pstream_cast(struct pstream *pstream)
{
    pstream_assert_class(pstream, &pssl_pstream_class);
    return CONTAINER_OF(pstream, struct pssl_pstream, pstream);
}

static int
pssl_open(const char *name UNUSED, char *suffix, struct pstream **pstreamp)
{
    struct pssl_pstream *pssl;
    struct sockaddr_in sin;
    char bound_name[128];
    int retval;
    int fd;

    retval = ssl_init();
    if (retval) {
        return retval;
    }

    fd = inet_open_passive(SOCK_STREAM, suffix, OFP_SSL_PORT, NULL);
    if (fd < 0) {
        return -fd;
    }
    sprintf(bound_name, "pssl:%"PRIu16":"IP_FMT,
            ntohs(sin.sin_port), IP_ARGS(&sin.sin_addr.s_addr));

    pssl = xmalloc(sizeof *pssl);
    pstream_init(&pssl->pstream, &pssl_pstream_class, bound_name);
    pssl->fd = fd;
    *pstreamp = &pssl->pstream;
    return 0;
}

static void
pssl_close(struct pstream *pstream)
{
    struct pssl_pstream *pssl = pssl_pstream_cast(pstream);
    close(pssl->fd);
    free(pssl);
}

static int
pssl_accept(struct pstream *pstream, struct stream **new_streamp)
{
    struct pssl_pstream *pssl = pssl_pstream_cast(pstream);
    struct sockaddr_in sin;
    socklen_t sin_len = sizeof sin;
    char name[128];
    int new_fd;
    int error;

    new_fd = accept(pssl->fd, &sin, &sin_len);
    if (new_fd < 0) {
        int error = errno;
        if (error != EAGAIN) {
            VLOG_DBG_RL(&rl, "accept: %s", strerror(error));
        }
        return error;
    }

    error = set_nonblocking(new_fd);
    if (error) {
        close(new_fd);
        return error;
    }

    sprintf(name, "ssl:"IP_FMT, IP_ARGS(&sin.sin_addr));
    if (sin.sin_port != htons(OFP_SSL_PORT)) {
        sprintf(strchr(name, '\0'), ":%"PRIu16, ntohs(sin.sin_port));
    }
    return new_ssl_stream(name, new_fd, SERVER, STATE_SSL_CONNECTING, &sin,
                         new_streamp);
}

static void
pssl_wait(struct pstream *pstream)
{
    struct pssl_pstream *pssl = pssl_pstream_cast(pstream);
    poll_fd_wait(pssl->fd, POLLIN);
}

struct pstream_class pssl_pstream_class = {
    "pssl",
    pssl_open,
    pssl_close,
    pssl_accept,
    pssl_wait,
};

/*
 * Returns true if OpenSSL error is WANT_READ or WANT_WRITE, indicating that
 * OpenSSL is requesting that we call it back when the socket is ready for read
 * or writing, respectively.
 */
static bool
ssl_wants_io(int ssl_error)
{
    return (ssl_error == SSL_ERROR_WANT_WRITE
            || ssl_error == SSL_ERROR_WANT_READ);
}

static int
ssl_init(void)
{
    static int init_status = -1;
    if (init_status < 0) {
        init_status = do_ssl_init();
        assert(init_status >= 0);
    }
    return init_status;
}

static int
do_ssl_init(void)
{
    SSL_METHOD *method;

    SSL_library_init();
    SSL_load_error_strings();

    method = TLSv1_method();
    if (method == NULL) {
        VLOG_ERR("TLSv1_method: %s", ERR_error_string(ERR_get_error(), NULL));
        return ENOPROTOOPT;
    }

    ctx = SSL_CTX_new(method);
    if (ctx == NULL) {
        VLOG_ERR("SSL_CTX_new: %s", ERR_error_string(ERR_get_error(), NULL));
        return ENOPROTOOPT;
    }
    SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
    SSL_CTX_set_tmp_dh_callback(ctx, tmp_dh_callback);
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE);
    SSL_CTX_set_mode(ctx, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                       NULL);

    return 0;
}

static DH *
tmp_dh_callback(SSL *ssl UNUSED, int is_export UNUSED, int keylength)
{
    struct dh {
        int keylength;
        DH *dh;
        DH *(*constructor)(void);
    };

    static struct dh dh_table[] = {
        {1024, NULL, get_dh1024},
        {2048, NULL, get_dh2048},
        {4096, NULL, get_dh4096},
    };

    struct dh *dh;

    for (dh = dh_table; dh < &dh_table[ARRAY_SIZE(dh_table)]; dh++) {
        if (dh->keylength == keylength) {
            if (!dh->dh) {
                dh->dh = dh->constructor();
                if (!dh->dh) {
                    ovs_fatal(ENOMEM, "out of memory constructing "
                              "Diffie-Hellman parameters");
                }
            }
            return dh->dh;
        }
    }
    VLOG_ERR_RL(&rl, "no Diffie-Hellman parameters for key length %d",
                keylength);
    return NULL;
}

/* Returns true if SSL is at least partially configured. */
bool
stream_ssl_is_configured(void) 
{
    return has_private_key || has_certificate || has_ca_cert;
}

void
stream_ssl_set_private_key_file(const char *file_name)
{
    if (ssl_init()) {
        return;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, file_name, SSL_FILETYPE_PEM) != 1) {
        VLOG_ERR("SSL_use_PrivateKey_file: %s",
                 ERR_error_string(ERR_get_error(), NULL));
        return;
    }
    has_private_key = true;
}

void
stream_ssl_set_certificate_file(const char *file_name)
{
    if (ssl_init()) {
        return;
    }
    if (SSL_CTX_use_certificate_chain_file(ctx, file_name) != 1) {
        VLOG_ERR("SSL_use_certificate_file: %s",
                 ERR_error_string(ERR_get_error(), NULL));
        return;
    }
    has_certificate = true;
}

/* Reads the X509 certificate or certificates in file 'file_name'.  On success,
 * stores the address of the first element in an array of pointers to
 * certificates in '*certs' and the number of certificates in the array in
 * '*n_certs', and returns 0.  On failure, stores a null pointer in '*certs', 0
 * in '*n_certs', and returns a positive errno value.
 *
 * The caller is responsible for freeing '*certs'. */
static int
read_cert_file(const char *file_name, X509 ***certs, size_t *n_certs)
{
    FILE *file;
    size_t allocated_certs = 0;

    *certs = NULL;
    *n_certs = 0;

    file = fopen(file_name, "r");
    if (!file) {
        VLOG_ERR("failed to open %s for reading: %s",
                 file_name, strerror(errno));
        return errno;
    }

    for (;;) {
        X509 *certificate;
        int c;

        /* Read certificate from file. */
        certificate = PEM_read_X509(file, NULL, NULL, NULL);
        if (!certificate) {
            size_t i;

            VLOG_ERR("PEM_read_X509 failed reading %s: %s",
                     file_name, ERR_error_string(ERR_get_error(), NULL));
            for (i = 0; i < *n_certs; i++) {
                X509_free((*certs)[i]);
            }
            free(*certs);
            *certs = NULL;
            *n_certs = 0;
            return EIO;
        }

        /* Add certificate to array. */
        if (*n_certs >= allocated_certs) {
            *certs = x2nrealloc(*certs, &allocated_certs, sizeof **certs);
        }
        (*certs)[(*n_certs)++] = certificate;

        /* Are there additional certificates in the file? */
        do {
            c = getc(file);
        } while (isspace(c));
        if (c == EOF) {
            break;
        }
        ungetc(c, file);
    }
    fclose(file);
    return 0;
}


/* Sets 'file_name' as the name of a file containing one or more X509
 * certificates to send to the peer.  Typical use in OpenFlow is to send the CA
 * certificate to the peer, which enables a switch to pick up the controller's
 * CA certificate on its first connection. */
void
stream_ssl_set_peer_ca_cert_file(const char *file_name)
{
    X509 **certs;
    size_t n_certs;
    size_t i;

    if (ssl_init()) {
        return;
    }

    if (!read_cert_file(file_name, &certs, &n_certs)) {
        for (i = 0; i < n_certs; i++) {
            if (SSL_CTX_add_extra_chain_cert(ctx, certs[i]) != 1) {
                VLOG_ERR("SSL_CTX_add_extra_chain_cert: %s",
                         ERR_error_string(ERR_get_error(), NULL));
            }
        }
        free(certs);
    }
}

/* Logs fingerprint of CA certificate 'cert' obtained from 'file_name'. */
static void
log_ca_cert(const char *file_name, X509 *cert)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int n_bytes;
    struct ds fp;
    char *subject;

    ds_init(&fp);
    if (!X509_digest(cert, EVP_sha1(), digest, &n_bytes)) {
        ds_put_cstr(&fp, "<out of memory>");
    } else {
        unsigned int i;
        for (i = 0; i < n_bytes; i++) {
            if (i) {
                ds_put_char(&fp, ':');
            }
            ds_put_format(&fp, "%02hhx", digest[i]);
        }
    }
    subject = X509_NAME_oneline(X509_get_subject_name(cert), NULL, 0);
    VLOG_INFO("Trusting CA cert from %s (%s) (fingerprint %s)", file_name,
              subject ? subject : "<out of memory>", ds_cstr(&fp));
    free(subject);
    ds_destroy(&fp);
}

/* Sets 'file_name' as the name of the file from which to read the CA
 * certificate used to verify the peer within SSL connections.  If 'bootstrap'
 * is false, the file must exist.  If 'bootstrap' is false, then the file is
 * read if it is exists; if it does not, then it will be created from the CA
 * certificate received from the peer on the first SSL connection. */
void
stream_ssl_set_ca_cert_file(const char *file_name, bool bootstrap)
{
    X509 **certs;
    size_t n_certs;
    struct stat s;

    if (ssl_init()) {
        return;
    }

    if (bootstrap && stat(file_name, &s) && errno == ENOENT) {
        bootstrap_ca_cert = true;
        ca_cert_file = xstrdup(file_name);
    } else if (!read_cert_file(file_name, &certs, &n_certs)) {
        size_t i;

        /* Set up list of CAs that the server will accept from the client. */
        for (i = 0; i < n_certs; i++) {
            /* SSL_CTX_add_client_CA makes a copy of the relevant data. */
            if (SSL_CTX_add_client_CA(ctx, certs[i]) != 1) {
                VLOG_ERR("failed to add client certificate %d from %s: %s",
                         i, file_name,
                         ERR_error_string(ERR_get_error(), NULL));
            } else {
                log_ca_cert(file_name, certs[i]);
            }
            X509_free(certs[i]);
        }

        /* Set up CAs for OpenSSL to trust in verifying the peer's
         * certificate. */
        if (SSL_CTX_load_verify_locations(ctx, file_name, NULL) != 1) {
            VLOG_ERR("SSL_CTX_load_verify_locations: %s",
                     ERR_error_string(ERR_get_error(), NULL));
            return;
        }

        has_ca_cert = true;
    }
}
