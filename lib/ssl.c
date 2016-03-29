/*
 * libwebsockets - small server side websockets and web server implementation
 *
 * Copyright (C) 2010-2014 Andy Green <andy@warmcat.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation:
 *  version 2.1 of the License.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA  02110-1301  USA
 */

#include "private-libwebsockets.h"
#ifndef USE_WOLFSSL
 #include <openssl/err.h>
#endif

#ifdef LWS_HAVE_OPENSSL_ECDH_H
#include <openssl/ecdh.h>
#endif

int openssl_websocket_private_data_index,
    openssl_SSL_CTX_private_data_index;

static int
lws_context_init_ssl_pem_passwd_cb(char * buf, int size, int rwflag, void *userdata)
{
	struct lws_context_creation_info * info =
			(struct lws_context_creation_info *)userdata;

	strncpy(buf, info->ssl_private_key_password, size);
	buf[size - 1] = '\0';

	return strlen(buf);
}

void
lws_ssl_bind_passphrase(SSL_CTX *ssl_ctx, struct lws_context_creation_info *info)
{
	if (!info->ssl_private_key_password)
		return;
	/*
	 * password provided, set ssl callback and user data
	 * for checking password which will be trigered during
	 * SSL_CTX_use_PrivateKey_file function
	 */
	SSL_CTX_set_default_passwd_cb_userdata(ssl_ctx, (void *)info);
	SSL_CTX_set_default_passwd_cb(ssl_ctx, lws_context_init_ssl_pem_passwd_cb);
}

int
lws_context_init_ssl_library(struct lws_context_creation_info *info)
{
#ifdef USE_WOLFSSL
#ifdef USE_OLD_CYASSL
	lwsl_notice(" Compiled with CyaSSL support\n");
#else
	lwsl_notice(" Compiled with wolfSSL support\n");
#endif
#else
	lwsl_notice(" Compiled with OpenSSL support\n");
#endif

	if (!lws_check_opt(info->options, LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT)) {
		lwsl_notice(" SSL disabled: no LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT\n");
		return 0;
	}

	/* basic openssl init */

	SSL_library_init();

	OpenSSL_add_all_algorithms();
	SSL_load_error_strings();

	openssl_websocket_private_data_index =
		SSL_get_ex_new_index(0, "lws", NULL, NULL, NULL);

	openssl_SSL_CTX_private_data_index = SSL_CTX_get_ex_new_index(0,
			NULL, NULL, NULL, NULL);

	return 0;
}


LWS_VISIBLE void
lws_ssl_destroy(struct lws_vhost *vhost)
{
	if (!lws_check_opt(vhost->context->options, LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT))
		return;

	if (vhost->ssl_ctx)
		SSL_CTX_free(vhost->ssl_ctx);
	if (!vhost->user_supplied_ssl_ctx && vhost->ssl_client_ctx)
		SSL_CTX_free(vhost->ssl_client_ctx);

#if (OPENSSL_VERSION_NUMBER < 0x01000000) || defined(USE_WOLFSSL)
	ERR_remove_state(0);
#else
	ERR_remove_thread_state(NULL);
#endif
	ERR_free_strings();
	EVP_cleanup();
	CRYPTO_cleanup_all_ex_data();
}

LWS_VISIBLE void
lws_decode_ssl_error(void)
{
	char buf[256];
	u_long err;

	while ((err = ERR_get_error()) != 0) {
		ERR_error_string_n(err, buf, sizeof(buf));
		lwsl_err("*** %lu %s\n", err, buf);
	}
}

LWS_VISIBLE void
lws_ssl_remove_wsi_from_buffered_list(struct lws *wsi)
{
	struct lws_context *context = wsi->context;
	struct lws_context_per_thread *pt = &context->pt[(int)wsi->tsi];

	if (!wsi->pending_read_list_prev &&
	    !wsi->pending_read_list_next &&
	    pt->pending_read_list != wsi)
		/* we are not on the list */
		return;

	/* point previous guy's next to our next */
	if (!wsi->pending_read_list_prev)
		pt->pending_read_list = wsi->pending_read_list_next;
	else
		wsi->pending_read_list_prev->pending_read_list_next =
			wsi->pending_read_list_next;

	/* point next guy's previous to our previous */
	if (wsi->pending_read_list_next)
		wsi->pending_read_list_next->pending_read_list_prev =
			wsi->pending_read_list_prev;

	wsi->pending_read_list_prev = NULL;
	wsi->pending_read_list_next = NULL;
}

LWS_VISIBLE int
lws_ssl_capable_read(struct lws *wsi, unsigned char *buf, int len)
{
	struct lws_context *context = wsi->context;
	struct lws_context_per_thread *pt = &context->pt[(int)wsi->tsi];
	int n;

	if (!wsi->ssl)
		return lws_ssl_capable_read_no_ssl(wsi, buf, len);

	n = SSL_read(wsi->ssl, buf, len);
	/* manpage: returning 0 means connection shut down */
	if (!n)
		return LWS_SSL_CAPABLE_ERROR;

	if (n < 0) {
		n = SSL_get_error(wsi->ssl, n);
		if (n ==  SSL_ERROR_WANT_READ || n ==  SSL_ERROR_WANT_WRITE)
			return LWS_SSL_CAPABLE_MORE_SERVICE;

		return LWS_SSL_CAPABLE_ERROR;
	}

	/*
	 * if it was our buffer that limited what we read,
	 * check if SSL has additional data pending inside SSL buffers.
	 *
	 * Because these won't signal at the network layer with POLLIN
	 * and if we don't realize, this data will sit there forever
	 */
	if (n != len)
		goto bail;
	if (!wsi->ssl)
		goto bail;
	if (!SSL_pending(wsi->ssl))
		goto bail;
	if (wsi->pending_read_list_next)
		return n;
	if (wsi->pending_read_list_prev)
		return n;
	if (pt->pending_read_list == wsi)
		return n;

	/* add us to the linked list of guys with pending ssl */
	if (pt->pending_read_list)
		pt->pending_read_list->pending_read_list_prev = wsi;

	wsi->pending_read_list_next = pt->pending_read_list;
	wsi->pending_read_list_prev = NULL;
	pt->pending_read_list = wsi;

	return n;
bail:
	lws_ssl_remove_wsi_from_buffered_list(wsi);

	return n;
}

LWS_VISIBLE int
lws_ssl_pending(struct lws *wsi)
{
	if (!wsi->ssl)
		return 0;

	return SSL_pending(wsi->ssl);
}

LWS_VISIBLE int
lws_ssl_capable_write(struct lws *wsi, unsigned char *buf, int len)
{
	int n;

	if (!wsi->ssl)
		return lws_ssl_capable_write_no_ssl(wsi, buf, len);

	n = SSL_write(wsi->ssl, buf, len);
	if (n > 0)
		return n;

	n = SSL_get_error(wsi->ssl, n);
	if (n == SSL_ERROR_WANT_READ || n == SSL_ERROR_WANT_WRITE) {
		if (n == SSL_ERROR_WANT_WRITE)
			lws_set_blocking_send(wsi);
		return LWS_SSL_CAPABLE_MORE_SERVICE;
	}

	return LWS_SSL_CAPABLE_ERROR;
}

LWS_VISIBLE int
lws_ssl_close(struct lws *wsi)
{
	int n;

	if (!wsi->ssl)
		return 0; /* not handled */

	n = SSL_get_fd(wsi->ssl);
	SSL_shutdown(wsi->ssl);
	compatible_close(n);
	SSL_free(wsi->ssl);
	wsi->ssl = NULL;

	return 1; /* handled */
}

/* leave all wsi close processing to the caller */

LWS_VISIBLE int
lws_server_socket_service_ssl(struct lws *wsi, lws_sockfd_type accept_fd)
{
	struct lws_context *context = wsi->context;
	struct lws_context_per_thread *pt = &context->pt[(int)wsi->tsi];
	int n, m;
#ifndef USE_WOLFSSL
	BIO *bio;
#endif

	if (!LWS_SSL_ENABLED(wsi->vhost))
		return 0;

	switch (wsi->mode) {
	case LWSCM_SSL_INIT:

		wsi->ssl = SSL_new(wsi->vhost->ssl_ctx);
		if (wsi->ssl == NULL) {
			lwsl_err("SSL_new failed: %s\n",
				 ERR_error_string(SSL_get_error(wsi->ssl, 0), NULL));
			lws_decode_ssl_error();
			if (accept_fd != LWS_SOCK_INVALID)
				compatible_close(accept_fd);
			goto fail;
		}

		SSL_set_ex_data(wsi->ssl,
			openssl_websocket_private_data_index, context);

		SSL_set_fd(wsi->ssl, accept_fd);

#ifdef USE_WOLFSSL
#ifdef USE_OLD_CYASSL
		CyaSSL_set_using_nonblock(wsi->ssl, 1);
#else
		wolfSSL_set_using_nonblock(wsi->ssl, 1);
#endif
#else
		SSL_set_mode(wsi->ssl, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
		bio = SSL_get_rbio(wsi->ssl);
		if (bio)
			BIO_set_nbio(bio, 1); /* nonblocking */
		else
			lwsl_notice("NULL rbio\n");
		bio = SSL_get_wbio(wsi->ssl);
		if (bio)
			BIO_set_nbio(bio, 1); /* nonblocking */
		else
			lwsl_notice("NULL rbio\n");
#endif

		/*
		 * we are not accepted yet, but we need to enter ourselves
		 * as a live connection.  That way we can retry when more
		 * pieces come if we're not sorted yet
		 */

		wsi->mode = LWSCM_SSL_ACK_PENDING;
		if (insert_wsi_socket_into_fds(context, wsi))
			goto fail;

		lws_set_timeout(wsi, PENDING_TIMEOUT_SSL_ACCEPT,
				context->timeout_secs);

		lwsl_info("inserted SSL accept into fds, trying SSL_accept\n");

		/* fallthru */

	case LWSCM_SSL_ACK_PENDING:

		if (lws_change_pollfd(wsi, LWS_POLLOUT, 0))
			goto fail;

		lws_latency_pre(context, wsi);

		n = recv(wsi->sock, (char *)pt->serv_buf, LWS_MAX_SOCKET_IO_BUF,
			 MSG_PEEK);

		/*
		 * optionally allow non-SSL connect on SSL listening socket
		 * This is disabled by default, if enabled it goes around any
		 * SSL-level access control (eg, client-side certs) so leave
		 * it disabled unless you know it's not a problem for you
		 */

		if (wsi->vhost->allow_non_ssl_on_ssl_port) {
			if (n >= 1 && pt->serv_buf[0] >= ' ') {
				/*
				* TLS content-type for Handshake is 0x16, and
				* for ChangeCipherSpec Record, it's 0x14
				*
				* A non-ssl session will start with the HTTP
				* method in ASCII.  If we see it's not a legit
				* SSL handshake kill the SSL for this
				* connection and try to handle as a HTTP
				* connection upgrade directly.
				*/
				wsi->use_ssl = 0;
				SSL_shutdown(wsi->ssl);
				SSL_free(wsi->ssl);
				wsi->ssl = NULL;
				if (lws_check_opt(context->options,
				    LWS_SERVER_OPTION_REDIRECT_HTTP_TO_HTTPS))
					wsi->redirect_to_https = 1;
				goto accepted;
			}
			if (!n) /*
				 * connection is gone, or nothing to read
				 * if it's gone, we will timeout on
				 * PENDING_TIMEOUT_SSL_ACCEPT
				 */
				break;
			if (n < 0 && (LWS_ERRNO == LWS_EAGAIN ||
				      LWS_ERRNO == LWS_EWOULDBLOCK)) {
				/*
				 * well, we get no way to know ssl or not
				 * so go around again waiting for something
				 * to come and give us a hint, or timeout the
				 * connection.
				 */
				m = SSL_ERROR_WANT_READ;
				goto go_again;
			}
		}

		/* normal SSL connection processing path */

		n = SSL_accept(wsi->ssl);
		lws_latency(context, wsi,
			"SSL_accept LWSCM_SSL_ACK_PENDING\n", n, n == 1);

		if (n == 1)
			goto accepted;

		m = SSL_get_error(wsi->ssl, n);
		lwsl_debug("SSL_accept failed %d / %s\n",
			   m, ERR_error_string(m, NULL));
go_again:
		if (m == SSL_ERROR_WANT_READ) {
			if (lws_change_pollfd(wsi, 0, LWS_POLLIN))
				goto fail;

			lwsl_info("SSL_ERROR_WANT_READ\n");
			break;
		}
		if (m == SSL_ERROR_WANT_WRITE) {
			if (lws_change_pollfd(wsi, 0, LWS_POLLOUT))
				goto fail;

			break;
		}
		lwsl_debug("SSL_accept failed skt %u: %s\n",
			   wsi->sock, ERR_error_string(m, NULL));
		goto fail;

accepted:
		/* OK, we are accepted... give him some time to negotiate */
		lws_set_timeout(wsi, PENDING_TIMEOUT_ESTABLISH_WITH_SERVER,
				context->timeout_secs);

		wsi->mode = LWSCM_HTTP_SERVING;

		lws_http2_configure_if_upgraded(wsi);

		lwsl_debug("accepted new SSL conn\n");
		break;
	}

	return 0;

fail:
	return 1;
}

void
lws_ssl_SSL_CTX_destroy(struct lws_vhost *vhost)
{
	if (vhost->ssl_ctx)
		SSL_CTX_free(vhost->ssl_ctx);
	if (!vhost->user_supplied_ssl_ctx && vhost->ssl_client_ctx)
		SSL_CTX_free(vhost->ssl_client_ctx);
}

void
lws_ssl_context_destroy(struct lws_context *context)
{
#if (OPENSSL_VERSION_NUMBER < 0x01000000) || defined(USE_WOLFSSL)
	ERR_remove_state(0);
#else
	ERR_remove_thread_state(NULL);
#endif
	ERR_free_strings();
	EVP_cleanup();
	CRYPTO_cleanup_all_ex_data();
}
