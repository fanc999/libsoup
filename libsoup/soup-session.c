/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * soup-session.c
 *
 * Copyright (C) 2000-2003, Ximian, Inc.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <unistd.h>
#include <string.h>
#include <stdlib.h>

#include "soup-auth.h"
#include "soup-auth-basic.h"
#include "soup-auth-digest.h"
#include "soup-auth-manager.h"
#include "soup-connection.h"
#include "soup-connection-ntlm.h"
#include "soup-marshal.h"
#include "soup-message-private.h"
#include "soup-message-queue.h"
#include "soup-session.h"
#include "soup-session-private.h"
#include "soup-ssl.h"
#include "soup-uri.h"

typedef struct {
	SoupURI    *root_uri;

	GSList     *connections;      /* CONTAINS: SoupConnection */
	guint       num_conns;

	GHashTable *auth_realms;      /* path -> scheme:realm */
	GHashTable *auths;            /* scheme:realm -> SoupAuth */
} SoupSessionHost;

typedef struct {
	SoupURI *proxy_uri;
	SoupAuth *proxy_auth;

	guint max_conns, max_conns_per_host;
	gboolean use_ntlm;

	char *ssl_ca_file;
	SoupSSLCredentials *ssl_creds;

	SoupMessageQueue *queue;

	SoupAuthManager *auth_manager;

	GHashTable *hosts; /* SoupURI -> SoupSessionHost */
	GHashTable *conns; /* SoupConnection -> SoupSessionHost */
	guint num_conns;

	/* Must hold the host_lock before potentially creating a
	 * new SoupSessionHost, or adding/removing a connection.
	 * Must not emit signals or destroy objects while holding it.
	 */
	GMutex *host_lock;

	GMainContext *async_context;

	/* Holds the timeout value for the connection, when
	   no response is received.
	*/
	guint timeout;
} SoupSessionPrivate;
#define SOUP_SESSION_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), SOUP_TYPE_SESSION, SoupSessionPrivate))

static void     free_host      (SoupSessionHost *host);

static void queue_message   (SoupSession *session, SoupMessage *msg,
			     SoupMessageCallbackFn callback,
			     gpointer user_data);
static void requeue_message (SoupSession *session, SoupMessage *msg);
static void cancel_message  (SoupSession *session, SoupMessage *msg);

/* temporary until we fix this to index hosts by SoupAddress */
extern guint     soup_uri_host_hash  (gconstpointer  key);
extern gboolean  soup_uri_host_equal (gconstpointer  v1,
				      gconstpointer  v2);
extern SoupURI  *soup_uri_copy_root  (const SoupURI *uri);

#define SOUP_SESSION_MAX_CONNS_DEFAULT 10
#define SOUP_SESSION_MAX_CONNS_PER_HOST_DEFAULT 4

G_DEFINE_TYPE (SoupSession, soup_session, G_TYPE_OBJECT)

enum {
	REQUEST_STARTED,
	AUTHENTICATE,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

enum {
	PROP_0,

	PROP_PROXY_URI,
	PROP_MAX_CONNS,
	PROP_MAX_CONNS_PER_HOST,
	PROP_USE_NTLM,
	PROP_SSL_CA_FILE,
	PROP_ASYNC_CONTEXT,
	PROP_TIMEOUT,

	LAST_PROP
};

static void set_property (GObject *object, guint prop_id,
			  const GValue *value, GParamSpec *pspec);
static void get_property (GObject *object, guint prop_id,
			  GValue *value, GParamSpec *pspec);

static void
soup_session_init (SoupSession *session)
{
	SoupSessionPrivate *priv = SOUP_SESSION_GET_PRIVATE (session);

	priv->queue = soup_message_queue_new ();

	priv->host_lock = g_mutex_new ();
	priv->hosts = g_hash_table_new (soup_uri_host_hash,
					soup_uri_host_equal);
	priv->conns = g_hash_table_new (NULL, NULL);

	priv->max_conns = SOUP_SESSION_MAX_CONNS_DEFAULT;
	priv->max_conns_per_host = SOUP_SESSION_MAX_CONNS_PER_HOST_DEFAULT;

	priv->timeout = 0;

	priv->auth_manager = soup_auth_manager_new (session);
	soup_auth_manager_add_type (priv->auth_manager, SOUP_TYPE_AUTH_BASIC);
	soup_auth_manager_add_type (priv->auth_manager, SOUP_TYPE_AUTH_DIGEST);
}

static gboolean
foreach_free_host (gpointer key, gpointer host, gpointer data)
{
	free_host (host);
	return TRUE;
}

static void
cleanup_hosts (SoupSessionPrivate *priv)
{
	GHashTable *old_hosts;

	g_mutex_lock (priv->host_lock);
	old_hosts = priv->hosts;
	priv->hosts = g_hash_table_new (soup_uri_host_hash,
					soup_uri_host_equal);
	g_mutex_unlock (priv->host_lock);

	g_hash_table_foreach_remove (old_hosts, foreach_free_host, NULL);
	g_hash_table_destroy (old_hosts);
}

static void
dispose (GObject *object)
{
	SoupSession *session = SOUP_SESSION (object);
	SoupSessionPrivate *priv = SOUP_SESSION_GET_PRIVATE (session);

	soup_session_abort (session);
	cleanup_hosts (priv);

	G_OBJECT_CLASS (soup_session_parent_class)->dispose (object);
}

static void
finalize (GObject *object)
{
	SoupSession *session = SOUP_SESSION (object);
	SoupSessionPrivate *priv = SOUP_SESSION_GET_PRIVATE (session);

	soup_message_queue_destroy (priv->queue);

	g_mutex_free (priv->host_lock);
	g_hash_table_destroy (priv->hosts);
	g_hash_table_destroy (priv->conns);

	soup_auth_manager_free (priv->auth_manager);

	if (priv->proxy_uri)
		soup_uri_free (priv->proxy_uri);

	if (priv->ssl_creds)
		soup_ssl_free_client_credentials (priv->ssl_creds);

	if (priv->async_context)
		g_main_context_unref (priv->async_context);

	G_OBJECT_CLASS (soup_session_parent_class)->finalize (object);
}

static void
soup_session_class_init (SoupSessionClass *session_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (session_class);

	g_type_class_add_private (session_class, sizeof (SoupSessionPrivate));

	/* virtual method definition */
	session_class->queue_message = queue_message;
	session_class->requeue_message = requeue_message;
	session_class->cancel_message = cancel_message;

	/* virtual method override */
	object_class->dispose = dispose;
	object_class->finalize = finalize;
	object_class->set_property = set_property;
	object_class->get_property = get_property;

	/* signals */

	/**
	 * SoupSession::request-started:
	 * @session: the session
	 * @msg: the request being sent
	 *
	 * Emitted just before a request is sent.
	 **/
	signals[REQUEST_STARTED] =
		g_signal_new ("request-started",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (SoupSessionClass, request_started),
			      NULL, NULL,
			      soup_marshal_NONE__OBJECT,
			      G_TYPE_NONE, 1,
			      SOUP_TYPE_MESSAGE);

	/**
	 * SoupSession::authenticate:
	 * @session: the session
	 * @msg: the #SoupMessage being sent
	 * @auth: the #SoupAuth to authenticate
	 * @retrying: %TRUE if this is the second (or later) attempt
	 *
	 * Emitted when the session requires authentication. If
	 * credentials are available call soup_auth_authenticate() on
	 * @auth. If these credentials fail, the signal will be
	 * emitted again, with @retrying set to %TRUE, which will
	 * continue until you return without calling
	 * soup_auth_authenticate() on @auth.
	 **/
	signals[AUTHENTICATE] =
		g_signal_new ("authenticate",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (SoupSessionClass, authenticate),
			      NULL, NULL,
			      soup_marshal_NONE__OBJECT_OBJECT_BOOLEAN,
			      G_TYPE_NONE, 3,
			      SOUP_TYPE_MESSAGE,
			      SOUP_TYPE_AUTH,
			      G_TYPE_BOOLEAN);

	/* properties */
	g_object_class_install_property (
		object_class, PROP_PROXY_URI,
		g_param_spec_boxed (SOUP_SESSION_PROXY_URI,
				    "Proxy URI",
				    "The HTTP Proxy to use for this session",
				    SOUP_TYPE_URI,
				    G_PARAM_READWRITE));
	g_object_class_install_property (
		object_class, PROP_MAX_CONNS,
		g_param_spec_int (SOUP_SESSION_MAX_CONNS,
				  "Max Connection Count",
				  "The maximum number of connections that the session can open at once",
				  1,
				  G_MAXINT,
				  10,
				  G_PARAM_READWRITE));
	g_object_class_install_property (
		object_class, PROP_MAX_CONNS_PER_HOST,
		g_param_spec_int (SOUP_SESSION_MAX_CONNS_PER_HOST,
				  "Max Per-Host Connection Count",
				  "The maximum number of connections that the session can open at once to a given host",
				  1,
				  G_MAXINT,
				  4,
				  G_PARAM_READWRITE));
	g_object_class_install_property (
		object_class, PROP_USE_NTLM,
		g_param_spec_boolean (SOUP_SESSION_USE_NTLM,
				      "Use NTLM",
				      "Whether or not to use NTLM authentication",
				      FALSE,
				      G_PARAM_READWRITE));
	g_object_class_install_property (
		object_class, PROP_SSL_CA_FILE,
		g_param_spec_string (SOUP_SESSION_SSL_CA_FILE,
				     "SSL CA file",
				     "File containing SSL CA certificates",
				     NULL,
				     G_PARAM_READWRITE));
	g_object_class_install_property (
		object_class, PROP_ASYNC_CONTEXT,
		g_param_spec_pointer (SOUP_SESSION_ASYNC_CONTEXT,
				      "Async GMainContext",
				      "The GMainContext to dispatch async I/O in",
				      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
	g_object_class_install_property (
		object_class, PROP_TIMEOUT,
		g_param_spec_uint (SOUP_SESSION_TIMEOUT,
				   "Timeout value",
				   "Value in seconds to timeout a blocking I/O",
				   0, G_MAXUINT, 0,
				   G_PARAM_READWRITE));
}

static gboolean
safe_uri_equal (const SoupURI *a, const SoupURI *b)
{
	if (!a && !b)
		return TRUE;

	if ((a && !b) || (b && !a))
		return FALSE;

	return soup_uri_equal (a, b);
}

static gboolean
safe_str_equal (const char *a, const char *b)
{
	if (!a && !b)
		return TRUE;

	if ((a && !b) || (b && !a))
		return FALSE;

	return strcmp (a, b) == 0;
}

static void
set_property (GObject *object, guint prop_id,
	      const GValue *value, GParamSpec *pspec)
{
	SoupSession *session = SOUP_SESSION (object);
	SoupSessionPrivate *priv = SOUP_SESSION_GET_PRIVATE (session);
	SoupURI *uri;
	gboolean need_abort = FALSE;
	gboolean ca_file_changed = FALSE;
	const char *new_ca_file;

	switch (prop_id) {
	case PROP_PROXY_URI:
		uri = g_value_get_boxed (value);

		if (!safe_uri_equal (priv->proxy_uri, uri))
			need_abort = TRUE;

		if (priv->proxy_uri)
			soup_uri_free (priv->proxy_uri);

		priv->proxy_uri = uri ? soup_uri_copy (uri) : NULL;

		if (need_abort) {
			soup_session_abort (session);
			cleanup_hosts (priv);
		}

		break;
	case PROP_MAX_CONNS:
		priv->max_conns = g_value_get_int (value);
		break;
	case PROP_MAX_CONNS_PER_HOST:
		priv->max_conns_per_host = g_value_get_int (value);
		break;
	case PROP_USE_NTLM:
		priv->use_ntlm = g_value_get_boolean (value);
		break;
	case PROP_SSL_CA_FILE:
		new_ca_file = g_value_get_string (value);

		if (!safe_str_equal (priv->ssl_ca_file, new_ca_file))
			ca_file_changed = TRUE;

		g_free (priv->ssl_ca_file);
		priv->ssl_ca_file = g_strdup (new_ca_file);

		if (ca_file_changed) {
			if (priv->ssl_creds) {
				soup_ssl_free_client_credentials (priv->ssl_creds);
				priv->ssl_creds = NULL;
			}

			cleanup_hosts (priv);
		}

		break;
	case PROP_ASYNC_CONTEXT:
		priv->async_context = g_value_get_pointer (value);
		if (priv->async_context)
			g_main_context_ref (priv->async_context);
		break;
	case PROP_TIMEOUT:
		priv->timeout = g_value_get_uint (value);
		break;
	default:
		break;
	}
}

static void
get_property (GObject *object, guint prop_id,
	      GValue *value, GParamSpec *pspec)
{
	SoupSession *session = SOUP_SESSION (object);
	SoupSessionPrivate *priv = SOUP_SESSION_GET_PRIVATE (session);

	switch (prop_id) {
	case PROP_PROXY_URI:
		g_value_set_boxed (value, priv->proxy_uri);
		break;
	case PROP_MAX_CONNS:
		g_value_set_int (value, priv->max_conns);
		break;
	case PROP_MAX_CONNS_PER_HOST:
		g_value_set_int (value, priv->max_conns_per_host);
		break;
	case PROP_USE_NTLM:
		g_value_set_boolean (value, priv->use_ntlm);
		break;
	case PROP_SSL_CA_FILE:
		g_value_set_string (value, priv->ssl_ca_file);
		break;
	case PROP_ASYNC_CONTEXT:
		g_value_set_pointer (value, priv->async_context ? g_main_context_ref (priv->async_context) : NULL);
		break;
	case PROP_TIMEOUT:
		g_value_set_uint (value, priv->timeout);
		break;
	default:
		break;
	}
}


/**
 * soup_session_get_async_context:
 * @session: a #SoupSession
 *
 * Gets @session's async_context. This does not add a ref to the
 * context, so you will need to ref it yourself if you want it to
 * outlive its session.
 *
 * Return value: @session's #GMainContext, which may be %NULL
 **/
GMainContext *
soup_session_get_async_context (SoupSession *session)
{
	SoupSessionPrivate *priv;

	g_return_val_if_fail (SOUP_IS_SESSION (session), NULL);
	priv = SOUP_SESSION_GET_PRIVATE (session);

	return priv->async_context;
}

/* Hosts */

static SoupSessionHost *
soup_session_host_new (SoupSession *session, const SoupURI *source_uri)
{
	SoupSessionPrivate *priv = SOUP_SESSION_GET_PRIVATE (session);
	SoupSessionHost *host;

	host = g_new0 (SoupSessionHost, 1);
	host->root_uri = soup_uri_copy_root (source_uri);

	if (soup_uri_is_https (host->root_uri) && !priv->ssl_creds) {
		priv->ssl_creds =
			soup_ssl_get_client_credentials (priv->ssl_ca_file);
	}

	return host;
}

/* Note: get_host_for_message doesn't lock the host_lock. The caller
 * must do it itself if there's a chance the host doesn't already
 * exist.
 */
static SoupSessionHost *
get_host_for_message (SoupSession *session, SoupMessage *msg)
{
	SoupSessionPrivate *priv = SOUP_SESSION_GET_PRIVATE (session);
	SoupSessionHost *host;
	const SoupURI *source = soup_message_get_uri (msg);

	host = g_hash_table_lookup (priv->hosts, source);
	if (host)
		return host;

	host = soup_session_host_new (session, source);
	g_hash_table_insert (priv->hosts, host->root_uri, host);

	return host;
}

static void
free_host (SoupSessionHost *host)
{
	while (host->connections) {
		SoupConnection *conn = host->connections->data;

		host->connections = g_slist_remove (host->connections, conn);
		soup_connection_disconnect (conn);
	}

	soup_uri_free (host->root_uri);
	g_free (host);
}	

void
soup_session_emit_authenticate (SoupSession *session, SoupMessage *msg,
				SoupAuth *auth, gboolean retrying)
{
	g_signal_emit (session, signals[AUTHENTICATE], 0, msg, auth, retrying);
}

static void
reemit_authenticate (SoupConnection *conn, SoupMessage *msg,
		     SoupAuth *auth, gboolean retrying, gpointer session)
{
	soup_session_emit_authenticate (session, msg, auth, retrying);
}

static void
redirect_handler (SoupMessage *msg, gpointer user_data)
{
	SoupSession *session = user_data;
	const char *new_loc;
	SoupURI *new_uri;

	if (!SOUP_STATUS_IS_REDIRECTION (msg->status_code))
		return;

	new_loc = soup_message_headers_find (msg->response_headers, "Location");
	if (!new_loc)
		return;

	/* Location is supposed to be an absolute URI, but some sites
	 * are lame, so we use soup_uri_new_with_base().
	 */
	new_uri = soup_uri_new_with_base (soup_message_get_uri (msg), new_loc);
	if (!new_uri) {
		soup_message_set_status_full (msg,
					      SOUP_STATUS_MALFORMED,
					      "Invalid Redirect URL");
		return;
	}

	soup_message_set_uri (msg, new_uri);
	soup_uri_free (new_uri);

	soup_session_requeue_message (session, msg);
}

static void
connection_started_request (SoupConnection *conn, SoupMessage *msg,
			    gpointer data)
{
	SoupSession *session = data;

	g_signal_emit (session, signals[REQUEST_STARTED], 0, msg);
}

static void
find_oldest_connection (gpointer key, gpointer host, gpointer data)
{
	SoupConnection *conn = key, **oldest = data;

	/* Don't prune a connection that is currently in use, or
	 * hasn't been used yet.
	 */
	if (soup_connection_is_in_use (conn) ||
	    soup_connection_last_used (conn) == 0)
		return;

	if (!*oldest || (soup_connection_last_used (conn) <
			 soup_connection_last_used (*oldest)))
		*oldest = conn;
}

/**
 * soup_session_try_prune_connection:
 * @session: a #SoupSession
 *
 * Finds the least-recently-used idle connection in @session and closes
 * it.
 *
 * Return value: %TRUE if a connection was closed, %FALSE if there are
 * no idle connections.
 **/
gboolean
soup_session_try_prune_connection (SoupSession *session)
{
	SoupSessionPrivate *priv = SOUP_SESSION_GET_PRIVATE (session);
	SoupConnection *oldest = NULL;

	g_mutex_lock (priv->host_lock);
	g_hash_table_foreach (priv->conns, find_oldest_connection,
			      &oldest);
	if (oldest) {
		/* Ref the connection before unlocking the mutex in
		 * case someone else tries to prune it too.
		 */
		g_object_ref (oldest);
		g_mutex_unlock (priv->host_lock);
		soup_connection_disconnect (oldest);
		g_object_unref (oldest);
		return TRUE;
	} else {
		g_mutex_unlock (priv->host_lock);
		return FALSE;
	}
}

static void
connection_disconnected (SoupConnection *conn, gpointer user_data)
{
	SoupSession *session = user_data;
	SoupSessionPrivate *priv = SOUP_SESSION_GET_PRIVATE (session);
	SoupSessionHost *host;

	g_mutex_lock (priv->host_lock);

	host = g_hash_table_lookup (priv->conns, conn);
	if (host) {
		g_hash_table_remove (priv->conns, conn);
		host->connections = g_slist_remove (host->connections, conn);
		host->num_conns--;
	}

	g_signal_handlers_disconnect_by_func (conn, connection_disconnected, session);
	priv->num_conns--;

	g_mutex_unlock (priv->host_lock);
	g_object_unref (conn);
}

static void
connect_result (SoupConnection *conn, guint status, gpointer user_data)
{
	SoupSession *session = user_data;
	SoupSessionPrivate *priv = SOUP_SESSION_GET_PRIVATE (session);
	SoupSessionHost *host;
	SoupMessageQueueIter iter;
	SoupMessage *msg;

	g_mutex_lock (priv->host_lock);

	host = g_hash_table_lookup (priv->conns, conn);
	if (!host) {
		g_mutex_unlock (priv->host_lock);
		return;
	}

	if (status == SOUP_STATUS_OK) {
		soup_connection_reserve (conn);
		host->connections = g_slist_prepend (host->connections, conn);
		g_mutex_unlock (priv->host_lock);
		return;
	}

	/* The connection failed. */
	g_mutex_unlock (priv->host_lock);
	connection_disconnected (conn, session);

	if (host->connections) {
		/* Something went wrong this time, but we have at
		 * least one open connection to this host. So just
		 * leave the message in the queue so it can use that
		 * connection once it's free.
		 */
		return;
	}

	/* There are two possibilities: either status is
	 * SOUP_STATUS_TRY_AGAIN, in which case the session implementation
	 * will create a new connection (and all we need to do here
	 * is downgrade the message from CONNECTING to QUEUED); or
	 * status is something else, probably CANT_CONNECT or
	 * CANT_RESOLVE or the like, in which case we need to cancel
	 * any messages waiting for this host, since they're out
	 * of luck.
	 */
	for (msg = soup_message_queue_first (priv->queue, &iter); msg; msg = soup_message_queue_next (priv->queue, &iter)) {
		if (get_host_for_message (session, msg) == host) {
			if (status == SOUP_STATUS_TRY_AGAIN) {
				if (soup_message_get_io_status (msg) == SOUP_MESSAGE_IO_STATUS_CONNECTING)
					soup_message_set_io_status (msg, SOUP_MESSAGE_IO_STATUS_QUEUED);
			} else {
				soup_message_set_status (msg, status);
				soup_session_cancel_message (session, msg);
			}
		}
	}
}

/**
 * soup_session_get_connection:
 * @session: a #SoupSession
 * @msg: a #SoupMessage
 * @try_pruning: on return, whether or not to try pruning a connection
 * @is_new: on return, %TRUE if the returned connection is new and not
 * yet connected
 * 
 * Tries to find or create a connection for @msg; this is an internal
 * method for #SoupSession subclasses.
 *
 * If there is an idle connection to the relevant host available, then
 * that connection will be returned (with *@is_new set to %FALSE). The
 * connection will be marked "reserved", so the caller must call
 * soup_connection_release() if it ends up not using the connection
 * right away.
 *
 * If there is no idle connection available, but it is possible to
 * create a new connection, then one will be created and returned,
 * with *@is_new set to %TRUE. The caller MUST then call
 * soup_connection_connect_sync() or soup_connection_connect_async()
 * to connect it. If the connection attempt succeeds, the connection
 * will be marked "reserved" and added to @session's connection pool
 * once it connects. If the connection attempt fails, the connection
 * will be unreffed.
 *
 * If no connection is available and a new connection cannot be made,
 * soup_session_get_connection() will return %NULL. If @session has
 * the maximum number of open connections open, but does not have the
 * maximum number of per-host connections open to the relevant host,
 * then *@try_pruning will be set to %TRUE. In this case, the caller
 * can call soup_session_try_prune_connection() to close an idle
 * connection, and then try soup_session_get_connection() again. (If
 * calling soup_session_try_prune_connection() wouldn't help, then
 * *@try_pruning is left untouched; it is NOT set to %FALSE.)
 *
 * Return value: a #SoupConnection, or %NULL
 **/
SoupConnection *
soup_session_get_connection (SoupSession *session, SoupMessage *msg,
			     gboolean *try_pruning, gboolean *is_new)
{
	SoupSessionPrivate *priv = SOUP_SESSION_GET_PRIVATE (session);
	SoupConnection *conn;
	SoupSessionHost *host;
	GSList *conns;

	g_mutex_lock (priv->host_lock);

	host = get_host_for_message (session, msg);
	for (conns = host->connections; conns; conns = conns->next) {
		if (!soup_connection_is_in_use (conns->data)) {
			soup_connection_reserve (conns->data);
			g_mutex_unlock (priv->host_lock);
			*is_new = FALSE;
			return conns->data;
		}
	}

	if (soup_message_get_io_status (msg) == SOUP_MESSAGE_IO_STATUS_CONNECTING) {
		/* We already started a connection for this
		 * message, so don't start another one.
		 */
		g_mutex_unlock (priv->host_lock);
		return NULL;
	}

	if (host->num_conns >= priv->max_conns_per_host) {
		g_mutex_unlock (priv->host_lock);
		return NULL;
	}

	if (priv->num_conns >= priv->max_conns) {
		*try_pruning = TRUE;
		g_mutex_unlock (priv->host_lock);
		return NULL;
	}

	conn = g_object_new (
		(priv->use_ntlm ?
		 SOUP_TYPE_CONNECTION_NTLM : SOUP_TYPE_CONNECTION),
		SOUP_CONNECTION_ORIGIN_URI, host->root_uri,
		SOUP_CONNECTION_PROXY_URI, priv->proxy_uri,
		SOUP_CONNECTION_SSL_CREDENTIALS, priv->ssl_creds,
		SOUP_CONNECTION_ASYNC_CONTEXT, priv->async_context,
		SOUP_CONNECTION_TIMEOUT, priv->timeout,
		NULL);
	g_signal_connect (conn, "connect_result",
			  G_CALLBACK (connect_result),
			  session);
	g_signal_connect (conn, "disconnected",
			  G_CALLBACK (connection_disconnected),
			  session);
	g_signal_connect (conn, "request_started",
			  G_CALLBACK (connection_started_request),
			  session);
	g_signal_connect (conn, "authenticate",
			  G_CALLBACK (reemit_authenticate),
			  session);

	g_hash_table_insert (priv->conns, conn, host);

	/* We increment the connection counts so it counts against the
	 * totals, but we don't add it to the host's connection list
	 * yet, since it's not ready for use.
	 */
	priv->num_conns++;
	host->num_conns++;

	/* Mark the request as connecting, so we don't try to open
	 * another new connection for it while waiting for this one.
	 */
	soup_message_set_io_status (msg, SOUP_MESSAGE_IO_STATUS_CONNECTING);

	g_mutex_unlock (priv->host_lock);
	*is_new = TRUE;
	return conn;
}

SoupMessageQueue *
soup_session_get_queue (SoupSession *session)
{
	SoupSessionPrivate *priv = SOUP_SESSION_GET_PRIVATE (session);

	return priv->queue;
}

static void
message_finished (SoupMessage *msg, gpointer user_data)
{
	SoupSession *session = user_data;
	SoupSessionPrivate *priv = SOUP_SESSION_GET_PRIVATE (session);

	if (!SOUP_MESSAGE_IS_STARTING (msg)) {
		soup_message_queue_remove_message (priv->queue, msg);
		g_signal_handlers_disconnect_by_func (msg, message_finished, session);
	}
}

static void
queue_message (SoupSession *session, SoupMessage *msg,
	       SoupMessageCallbackFn callback, gpointer user_data)
{
	SoupSessionPrivate *priv = SOUP_SESSION_GET_PRIVATE (session);

	g_signal_connect_after (msg, "finished",
				G_CALLBACK (message_finished), session);

	if (!(soup_message_get_flags (msg) & SOUP_MESSAGE_NO_REDIRECT)) {
		soup_message_add_header_handler (
			msg, "got_body", "Location",
			G_CALLBACK (redirect_handler), session);
	}

	soup_message_set_io_status (msg, SOUP_MESSAGE_IO_STATUS_QUEUED);
	soup_message_queue_append (priv->queue, msg);
}

/**
 * soup_session_queue_message:
 * @session: a #SoupSession
 * @msg: the message to queue
 * @callback: a #SoupMessageCallbackFn which will be called after the
 * message completes or when an unrecoverable error occurs.
 * @user_data: a pointer passed to @callback.
 * 
 * Queues the message @msg for sending. All messages are processed
 * while the glib main loop runs. If @msg has been processed before,
 * any resources related to the time it was last sent are freed.
 *
 * Upon message completion, the callback specified in @callback will
 * be invoked (in the thread associated with @session's async
 * context). If after returning from this callback the message has not
 * been requeued, @msg will be unreffed.
 */
void
soup_session_queue_message (SoupSession *session, SoupMessage *msg,
			    SoupMessageCallbackFn callback, gpointer user_data)
{
	g_return_if_fail (SOUP_IS_SESSION (session));
	g_return_if_fail (SOUP_IS_MESSAGE (msg));

	SOUP_SESSION_GET_CLASS (session)->queue_message (session, msg,
							 callback, user_data);
}

static void
requeue_message (SoupSession *session, SoupMessage *msg)
{
	soup_message_set_io_status (msg, SOUP_MESSAGE_IO_STATUS_QUEUED);
}

/**
 * soup_session_requeue_message:
 * @session: a #SoupSession
 * @msg: the message to requeue
 *
 * This causes @msg to be placed back on the queue to be attempted
 * again.
 **/
void
soup_session_requeue_message (SoupSession *session, SoupMessage *msg)
{
	g_return_if_fail (SOUP_IS_SESSION (session));
	g_return_if_fail (SOUP_IS_MESSAGE (msg));

	SOUP_SESSION_GET_CLASS (session)->requeue_message (session, msg);
}


/**
 * soup_session_send_message:
 * @session: a #SoupSession
 * @msg: the message to send
 * 
 * Synchronously send @msg. This call will not return until the
 * transfer is finished successfully or there is an unrecoverable
 * error.
 *
 * @msg is not freed upon return.
 *
 * Return value: the HTTP status code of the response
 */
guint
soup_session_send_message (SoupSession *session, SoupMessage *msg)
{
	g_return_val_if_fail (SOUP_IS_SESSION (session), SOUP_STATUS_MALFORMED);
	g_return_val_if_fail (SOUP_IS_MESSAGE (msg), SOUP_STATUS_MALFORMED);

	return SOUP_SESSION_GET_CLASS (session)->send_message (session, msg);
}


/**
 * soup_session_pause_message:
 * @session: a #SoupSession
 * @msg: a #SoupMessage currently running on @session
 *
 * Pauses HTTP I/O on @msg. Call soup_session_unpause_message() to
 * resume I/O.
 **/
void
soup_session_pause_message (SoupSession *session,
			    SoupMessage *msg)
{
	g_return_if_fail (SOUP_IS_SESSION (session));
	g_return_if_fail (SOUP_IS_MESSAGE (msg));

	soup_message_io_pause (msg);
}

/**
 * soup_session_unpause_message:
 * @session: a #SoupSession
 * @msg: a #SoupMessage currently running on @session
 *
 * Resumes HTTP I/O on @msg. Use this to resume after calling
 * soup_sessino_pause_message().
 *
 * If @msg is being sent via blocking I/O, this will resume reading or
 * writing immediately. If @msg is using non-blocking I/O, then
 * reading or writing won't resume until you return to the main loop.
 **/
void
soup_session_unpause_message (SoupSession *session,
			      SoupMessage *msg)
{
	g_return_if_fail (SOUP_IS_SESSION (session));
	g_return_if_fail (SOUP_IS_MESSAGE (msg));

	soup_message_io_unpause (msg);
}


static void
cancel_message (SoupSession *session, SoupMessage *msg)
{
	SoupSessionPrivate *priv = SOUP_SESSION_GET_PRIVATE (session);

	soup_message_queue_remove_message (priv->queue, msg);
	soup_message_finished (msg);
}

/**
 * soup_session_cancel_message:
 * @session: a #SoupSession
 * @msg: the message to cancel
 *
 * Causes @session to immediately finish processing @msg. You should
 * set a status code on @msg with soup_message_set_status() before
 * calling this function.
 **/
void
soup_session_cancel_message (SoupSession *session, SoupMessage *msg)
{
	g_return_if_fail (SOUP_IS_SESSION (session));
	g_return_if_fail (SOUP_IS_MESSAGE (msg));

	SOUP_SESSION_GET_CLASS (session)->cancel_message (session, msg);
}

static void
gather_conns (gpointer key, gpointer host, gpointer data)
{
	SoupConnection *conn = key;
	GSList **conns = data;

	*conns = g_slist_prepend (*conns, conn);
}

/**
 * soup_session_abort:
 * @session: the session
 *
 * Cancels all pending requests in @session.
 **/
void
soup_session_abort (SoupSession *session)
{
	SoupSessionPrivate *priv;
	SoupMessageQueueIter iter;
	SoupMessage *msg;
	GSList *conns, *c;

	g_return_if_fail (SOUP_IS_SESSION (session));
	priv = SOUP_SESSION_GET_PRIVATE (session);

	for (msg = soup_message_queue_first (priv->queue, &iter);
	     msg;
	     msg = soup_message_queue_next (priv->queue, &iter)) {
		soup_message_set_status (msg, SOUP_STATUS_CANCELLED);
		soup_session_cancel_message (session, msg);
	}

	/* Close all connections */
	g_mutex_lock (priv->host_lock);
	conns = NULL;
	g_hash_table_foreach (priv->conns, gather_conns, &conns);

	for (c = conns; c; c = c->next)
		g_object_ref (c->data);
	g_mutex_unlock (priv->host_lock);
	for (c = conns; c; c = c->next) {
		soup_connection_disconnect (c->data);
		g_object_unref (c->data);
	}

	g_slist_free (conns);
}
