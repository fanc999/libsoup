/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * soup-auth-digest.c: HTTP Digest Authentication
 *
 * Copyright (C) 2001-2003, Ximian, Inc.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "soup-auth-digest.h"
#include "soup-headers.h"
#include "soup-md5-utils.h"
#include "soup-message.h"
#include "soup-message-private.h"
#include "soup-misc.h"
#include "soup-uri.h"

static gboolean update (SoupAuth *auth, SoupMessage *msg, GHashTable *auth_params);
static GSList *get_protection_space (SoupAuth *auth, const SoupURI *source_uri);
static void authenticate (SoupAuth *auth, const char *username, const char *password);
static gboolean is_authenticated (SoupAuth *auth);
static char *get_authorization (SoupAuth *auth, SoupMessage *msg);

typedef struct {
	char                    *user;
	char                     hex_urp[33];
	char                     hex_a1[33];

	/* These are provided by the server */
	char                    *nonce;
	char                    *opaque;
	SoupAuthDigestQop        qop_options;
	SoupAuthDigestAlgorithm  algorithm;
	char                    *domain;

	/* These are generated by the client */
	char                    *cnonce;
	int                      nc;
	SoupAuthDigestQop        qop;
} SoupAuthDigestPrivate;
#define SOUP_AUTH_DIGEST_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), SOUP_TYPE_AUTH_DIGEST, SoupAuthDigestPrivate))

static void recompute_hex_a1 (SoupAuthDigestPrivate *priv);

G_DEFINE_TYPE (SoupAuthDigest, soup_auth_digest, SOUP_TYPE_AUTH)

static void
soup_auth_digest_init (SoupAuthDigest *digest)
{
}

static void
finalize (GObject *object)
{
	SoupAuthDigestPrivate *priv = SOUP_AUTH_DIGEST_GET_PRIVATE (object);

	if (priv->user)
		g_free (priv->user);
	if (priv->nonce)
		g_free (priv->nonce);
	if (priv->domain)
		g_free (priv->domain);
	if (priv->cnonce)
		g_free (priv->cnonce);

	memset (priv->hex_urp, 0, sizeof (priv->hex_urp));
	memset (priv->hex_a1, 0, sizeof (priv->hex_a1));

	G_OBJECT_CLASS (soup_auth_digest_parent_class)->finalize (object);
}

static void
soup_auth_digest_class_init (SoupAuthDigestClass *auth_digest_class)
{
	SoupAuthClass *auth_class = SOUP_AUTH_CLASS (auth_digest_class);
	GObjectClass *object_class = G_OBJECT_CLASS (auth_digest_class);

	g_type_class_add_private (auth_digest_class, sizeof (SoupAuthDigestPrivate));

	auth_class->scheme_name = "Digest";
	auth_class->strength = 5;

	auth_class->get_protection_space = get_protection_space;
	auth_class->update = update;
	auth_class->authenticate = authenticate;
	auth_class->is_authenticated = is_authenticated;
	auth_class->get_authorization = get_authorization;

	object_class->finalize = finalize;
}

SoupAuthDigestAlgorithm
soup_auth_digest_parse_algorithm (const char *algorithm)
{
	if (!algorithm)
		return SOUP_AUTH_DIGEST_ALGORITHM_NONE;
	else if (!g_ascii_strcasecmp (algorithm, "MD5"))
		return SOUP_AUTH_DIGEST_ALGORITHM_MD5;
	else if (!g_ascii_strcasecmp (algorithm, "MD5-sess"))
		return SOUP_AUTH_DIGEST_ALGORITHM_MD5_SESS;
	else
		return -1;
}

char *
soup_auth_digest_get_algorithm (SoupAuthDigestAlgorithm algorithm)
{
	if (algorithm == SOUP_AUTH_DIGEST_ALGORITHM_MD5)
		return g_strdup ("MD5");
	else if (algorithm == SOUP_AUTH_DIGEST_ALGORITHM_MD5_SESS)
		return g_strdup ("MD5-sess");
	else
		return NULL;
}

SoupAuthDigestQop
soup_auth_digest_parse_qop (const char *qop)
{
	GSList *qop_values, *iter;
	SoupAuthDigestQop out = 0;

	if (qop) {
		qop_values = soup_header_parse_list (qop);
		for (iter = qop_values; iter; iter = iter->next) {
			if (!g_ascii_strcasecmp (iter->data, "auth"))
				out |= SOUP_AUTH_DIGEST_QOP_AUTH;
			else if (!g_ascii_strcasecmp (iter->data, "auth-int"))
				out |= SOUP_AUTH_DIGEST_QOP_AUTH_INT;
			else
				out = -1;
		}
		soup_header_free_list (qop_values);
	}

	return out;
}

char *
soup_auth_digest_get_qop (SoupAuthDigestQop qop)
{
	GString *out;

	out = g_string_new (NULL);
	if (qop & SOUP_AUTH_DIGEST_QOP_AUTH)
		g_string_append (out, "auth");
	if (qop & SOUP_AUTH_DIGEST_QOP_AUTH_INT) {
		if (qop & SOUP_AUTH_DIGEST_QOP_AUTH)
			g_string_append (out, ",");
		g_string_append (out, "auth-int");
	}

	return g_string_free (out, FALSE);
}

static gboolean
update (SoupAuth *auth, SoupMessage *msg, GHashTable *auth_params)
{
	SoupAuthDigestPrivate *priv = SOUP_AUTH_DIGEST_GET_PRIVATE (auth);
	const char *stale;
	guint qop_options;
	gboolean ok = TRUE;

	g_free (priv->domain);
	g_free (priv->nonce);
	g_free (priv->opaque);

	priv->nc = 1;

	priv->domain = g_strdup (g_hash_table_lookup (auth_params, "domain"));
	priv->nonce = g_strdup (g_hash_table_lookup (auth_params, "nonce"));
	priv->opaque = g_strdup (g_hash_table_lookup (auth_params, "opaque"));

	qop_options = soup_auth_digest_parse_qop (g_hash_table_lookup (auth_params, "qop"));
	/* We're just going to do qop=auth for now */
	if (qop_options == -1 || !(qop_options & SOUP_AUTH_DIGEST_QOP_AUTH))
		ok = FALSE;
	priv->qop = SOUP_AUTH_DIGEST_QOP_AUTH;

	priv->algorithm = soup_auth_digest_parse_algorithm (g_hash_table_lookup (auth_params, "algorithm"));
	if (priv->algorithm == -1)
		ok = FALSE;

	stale = g_hash_table_lookup (auth_params, "stale");
	if (stale && !g_ascii_strcasecmp (stale, "TRUE") && *priv->hex_urp)
		recompute_hex_a1 (priv);
	else {
		g_free (priv->user);
		priv->user = NULL;
		g_free (priv->cnonce);
		priv->cnonce = NULL;
		memset (priv->hex_urp, 0, sizeof (priv->hex_urp));
		memset (priv->hex_a1, 0, sizeof (priv->hex_a1));
        }

	return ok;
}

static GSList *
get_protection_space (SoupAuth *auth, const SoupURI *source_uri)
{
	SoupAuthDigestPrivate *priv = SOUP_AUTH_DIGEST_GET_PRIVATE (auth);
	GSList *space = NULL;
	SoupURI *uri;
	char **dvec, *d, *dir, *slash;
	int dix;

	if (!priv->domain || !*priv->domain) {
		/* If no domain directive, the protection space is the
		 * whole server.
		 */
		return g_slist_prepend (NULL, g_strdup (""));
	}

	dvec = g_strsplit (priv->domain, " ", 0);
	for (dix = 0; dvec[dix] != NULL; dix++) {
		d = dvec[dix];
		if (*d == '/')
			dir = g_strdup (d);
		else {
			uri = soup_uri_new (d);
			if (uri && uri->scheme == source_uri->scheme &&
			    uri->port == source_uri->port &&
			    !strcmp (uri->host, source_uri->host))
				dir = g_strdup (uri->path);
			else
				dir = NULL;
			if (uri)
				soup_uri_free (uri);
		}

		if (dir) {
			slash = strrchr (dir, '/');
			if (slash && !slash[1])
				*slash = '\0';

			space = g_slist_prepend (space, dir);
		}
	}
	g_strfreev (dvec);

	return space;
}

void
soup_auth_digest_compute_hex_urp (const char *username,
				  const char *realm,
				  const char *password,
				  char        hex_urp[33])
{
	SoupMD5Context ctx;

	soup_md5_init (&ctx);
	soup_md5_update (&ctx, username, strlen (username));
	soup_md5_update (&ctx, ":", 1);
	soup_md5_update (&ctx, realm, strlen (realm));
	soup_md5_update (&ctx, ":", 1);
	soup_md5_update (&ctx, password, strlen (password));
	soup_md5_final_hex (&ctx, hex_urp);
}

void
soup_auth_digest_compute_hex_a1 (const char              *hex_urp,
				 SoupAuthDigestAlgorithm  algorithm,
				 const char              *nonce,
				 const char              *cnonce,
				 char                     hex_a1[33])
{
	if (algorithm == SOUP_AUTH_DIGEST_ALGORITHM_MD5) {
		/* In MD5, A1 is just user:realm:password, so hex_A1
		 * is just hex_urp.
		 */
		/* You'd think you could say "sizeof (hex_a1)" here,
		 * but you'd be wrong.
		 */
		memcpy (hex_a1, hex_urp, 33);
	} else {
		SoupMD5Context ctx;

		/* In MD5-sess, A1 is hex_urp:nonce:cnonce */

		soup_md5_init (&ctx);
		soup_md5_update (&ctx, hex_urp, strlen (hex_urp));
		soup_md5_update (&ctx, ":", 1);
		soup_md5_update (&ctx, nonce, strlen (nonce));
		soup_md5_update (&ctx, ":", 1);
		soup_md5_update (&ctx, cnonce, strlen (cnonce));

		soup_md5_final_hex (&ctx, hex_a1);
	}
}

static void
recompute_hex_a1 (SoupAuthDigestPrivate *priv)
{
	soup_auth_digest_compute_hex_a1 (priv->hex_urp,
					 priv->algorithm,
					 priv->nonce,
					 priv->cnonce,
					 priv->hex_a1);
}

static void
authenticate (SoupAuth *auth, const char *username, const char *password)
{
	SoupAuthDigestPrivate *priv = SOUP_AUTH_DIGEST_GET_PRIVATE (auth);
	char *bgen;

	/* Create client nonce */
	bgen = g_strdup_printf ("%p:%lu:%lu",
				auth,
				(unsigned long) getpid (),
				(unsigned long) time (0));
	priv->cnonce = g_base64_encode ((guchar *)bgen, strlen (bgen));
	g_free (bgen);

	priv->user = g_strdup (username);

	/* compute "URP" (user:realm:password) */
	soup_auth_digest_compute_hex_urp (username, auth->realm,
					  password ? password : "",
					  priv->hex_urp);

	/* And compute A1 from that */
	recompute_hex_a1 (priv);
}

static gboolean
is_authenticated (SoupAuth *auth)
{
	return SOUP_AUTH_DIGEST_GET_PRIVATE (auth)->cnonce != NULL;
}

void
soup_auth_digest_compute_response (const char        *method,
				   const char        *uri,
				   const char        *hex_a1,
				   SoupAuthDigestQop  qop,
				   const char        *nonce,
				   const char        *cnonce,
				   int                nc,
				   char               response[33])
{
	char hex_a2[33];
	SoupMD5Context md5;

	/* compute A2 */
	soup_md5_init (&md5);
	soup_md5_update (&md5, method, strlen (method));
	soup_md5_update (&md5, ":", 1);
	soup_md5_update (&md5, uri, strlen (uri));
	soup_md5_final_hex (&md5, hex_a2);

	/* compute KD */
	soup_md5_init (&md5);
	soup_md5_update (&md5, hex_a1, strlen (hex_a1));
	soup_md5_update (&md5, ":", 1);
	soup_md5_update (&md5, nonce, strlen (nonce));
	soup_md5_update (&md5, ":", 1);

	if (qop) {
		char tmp[9];

		snprintf (tmp, 9, "%.8x", nc);
		soup_md5_update (&md5, tmp, strlen (tmp));
		soup_md5_update (&md5, ":", 1);
		soup_md5_update (&md5, cnonce, strlen (cnonce));
		soup_md5_update (&md5, ":", 1);

		if (qop != SOUP_AUTH_DIGEST_QOP_AUTH)
			g_assert_not_reached ();
		soup_md5_update (&md5, "auth", strlen ("auth"));
		soup_md5_update (&md5, ":", 1);
	}

	soup_md5_update (&md5, hex_a2, 32);
	soup_md5_final_hex (&md5, response);
}

static void
authentication_info_cb (SoupMessage *msg, gpointer data)
{
	SoupAuth *auth = data;
	SoupAuthDigestPrivate *priv = SOUP_AUTH_DIGEST_GET_PRIVATE (auth);
	const char *header;
	GHashTable *auth_params;
	char *nextnonce;

	if (auth != soup_message_get_auth (msg))
		return;

	header = soup_message_headers_get (msg->response_headers,
					    soup_auth_is_for_proxy (auth) ?
					    "Proxy-Authentication-Info" :
					    "Authentication-Info");
	g_return_if_fail (header != NULL);

	auth_params = soup_header_parse_param_list (header);
	if (!auth_params)
		return;

	nextnonce = g_strdup (g_hash_table_lookup (auth_params, "nextnonce"));
	if (nextnonce) {
		g_free (priv->nonce);
		priv->nonce = nextnonce;
	}

	soup_header_free_param_list (auth_params);
}

static char *
get_authorization (SoupAuth *auth, SoupMessage *msg)
{
	SoupAuthDigestPrivate *priv = SOUP_AUTH_DIGEST_GET_PRIVATE (auth);
	char response[33], *token;
	char *url;
	GString *out;
	const SoupURI *uri;

	uri = soup_message_get_uri (msg);
	g_return_val_if_fail (uri != NULL, NULL);
	url = soup_uri_to_string (uri, TRUE);

	soup_auth_digest_compute_response (msg->method, url, priv->hex_a1,
					   priv->qop, priv->nonce,
					   priv->cnonce, priv->nc,
					   response);

	out = g_string_new ("Digest ");

	/* FIXME: doesn't deal with quotes in the %s strings */
	g_string_append_printf (out, "username=\"%s\", realm=\"%s\", "
				"nonce=\"%s\", uri=\"%s\", response=\"%s\"",
				priv->user, auth->realm, priv->nonce,
				url, response);

	if (priv->opaque)
		g_string_append_printf (out, ", opaque=\"%s\"", priv->opaque);

	if (priv->qop) {
		char *qop = soup_auth_digest_get_qop (priv->qop);

		g_string_append_printf (out, ", cnonce=\"%s\", nc=\"%.8x\", qop=\"%s\"",
					priv->cnonce, priv->nc, qop);
		g_free (qop);
	}

	g_free (url);

	priv->nc++;

	token = g_string_free (out, FALSE);

	soup_message_add_header_handler (msg,
					 "got_headers",
					 soup_auth_is_for_proxy (auth) ?
					 "Proxy-Authentication-Info" :
					 "Authentication-Info",
					 G_CALLBACK (authentication_info_cb),
					 auth);
	return token;
}
