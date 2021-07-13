/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 *
 * @file tls/log.c
 * @brief Retrieve errors and log messages from OpenSSL's overly complex logging system.
 *
 * @copyright 2016,2021 Arran Cudbard-Bell (a.cudbardb@freeradius.org)
 * @copyright 2016 The FreeRADIUS server project
 */
RCSID("$Id$")
USES_APPLE_DEPRECATED_API	/* OpenSSL API has been deprecated by Apple */

#ifdef WITH_TLS
#define LOG_PREFIX "tls - "

#include <freeradius-devel/server/base.h>
#include <freeradius-devel/util/debug.h>

#include "log.h"

/** Holds the state of a log BIO
 *
 * Most of these fields are expected to change between uses of the BIO.
 *
 * BIOs do not have indexed extension structures like other structures in OpenSSL,
 * so we're forced to place all information in a structure, and populate it just
 * prior to a BIO being used.
 *
 * These BIOs are thread local to avoid conflicts or locking issues.
 */
typedef struct {
	BIO			*bio;		//!< Logging bio to write to.
	fr_sbuff_t		sbuff;		//!< Used to aggregate line data.
	fr_sbuff_uctx_talloc_t	tctx;		//!< extra talloc information for the sbuff.
	fr_sbuff_marker_t	logged_m;	//!< How much data has been written.

	request_t		*request;	//!< the current request.  Only used for the
						///< request log BIOs.
	fr_log_type_t		type;		//!< The type of log messages the bio will produce.
	fr_log_lvl_t		lvl;		//!< Level to log message at.
	char const		*file;		//!< File this log bio was bound on.
	int			line;		//!< Line this log bio was bound on.
} fr_tls_log_bio_t;

/** Template for the thread local request log BIOs
 */
static BIO_METHOD	*tls_request_log_meth;

/** Template for the global log BIOs
 */
static BIO_METHOD	*tls_global_log_meth;

/** Thread local request log BIO
 */
static _Thread_local	fr_tls_log_bio_t	*request_log_bio;

/** Thread local global log BIO
 */
static _Thread_local	fr_tls_log_bio_t	*global_log_bio;

static void _tls_ctx_print_cert_line(char const *file, int line,
				     request_t *request, int index, X509 *cert)
{
	char		subject[1024];

	X509_NAME_oneline(X509_get_subject_name(cert), subject, sizeof(subject));
	subject[sizeof(subject) - 1] = '\0';

	if (request) {
		log_request(L_DBG, fr_debug_lvl, request, file, line,
			    "[%i] %s %s", index, fr_tls_utils_x509_pkey_type(cert), subject);
	} else {
		fr_log(LOG_DST, fr_debug_lvl, file, line,
		       "[%i] %s %s", index, fr_tls_utils_x509_pkey_type(cert), subject);
	}
}

DIAG_OFF(DIAG_UNKNOWN_PRAGMAS)
DIAG_OFF(used-but-marked-unused)	/* fix spurious warnings for sk macros */
/** Print out the current stack of certs
 *
 * @param[in] file	File where this function is being called.
 * @param[in] line	Line where this function is being called.
 * @param[in] request	Current request, may be NULL.
 * @param[in] chain	The certificate chain.
 * @param[in] cert	The leaf certificate.
 */
void _fr_tls_log_certificate_chain(char const *file, int line,
				   request_t *request, STACK_OF(X509) *chain, X509 *cert)
{
	int i;

	for (i = sk_X509_num(chain); i > 0 ; i--) {
		_tls_ctx_print_cert_line(file, line, request, i, sk_X509_value(chain, i - 1));
	}
	_tls_ctx_print_cert_line(file, line, request, i, cert);
}
DIAG_ON(used-but-marked-unused)
DIAG_ON(DIAG_UNKNOWN_PRAGMAS)

DIAG_OFF(format-nonliteral)
/** Print errors in the TLS thread local error stack
 *
 * Drains the thread local OpenSSL error queue, and prints out errors.
 *
 * @param[in] request	The current request (may be NULL).
 * @param[in] msg	Error message describing the operation being attempted.
 * @param[in] ap	Arguments for msg.
 * @return the number of errors drained from the stack.
 */
static int fr_tls_log_error_va(request_t *request, char const *msg, va_list ap)
{
	unsigned long	error;
	char		*p;
	int		in_stack = 0;
	char		buffer[256];

	int		line;
	char const	*file;
	char const	*data;
	int		flags = 0;

	/*
	 *	Pop the first error, so ERR_peek_error()
	 *	can be used to determine if there are
	 *	multiple errors.
	 */
	error = ERR_get_error_line_data(&file, &line, &data, &flags);
	if (!(flags & ERR_TXT_STRING)) data = NULL;

	if (msg) {
		p = talloc_vasprintf(request, msg, ap);

		/*
		 *	Single line mode (there's only one error)
		 */
		if (error && !ERR_peek_error()) {
			ERR_error_string_n(error, buffer, sizeof(buffer));

			/* Extra verbose */
			if ((request && RDEBUG_ENABLED3) || DEBUG_ENABLED3) {
				ROPTIONAL(REDEBUG, ERROR, "%s: %s[%i]:%s%c%s", p, file, line, buffer,
					  data ? ':' : '\0', data ? data : "");
			} else {
				ROPTIONAL(REDEBUG, ERROR, "%s: %s%c%s", p, buffer,
					  data ? ':' : '\0', data ? data : "");
			}

			talloc_free(p);

			return 1;
		}

		/*
		 *	Print the error we were given, irrespective
		 *	of whether there were any OpenSSL errors.
		 */
		ROPTIONAL(RERROR, ERROR, "%s", p);
		talloc_free(p);
	}

	/*
	 *	Stack mode (there are multiple errors)
	 */
	if (!error) return 0;
	do {
		if (!(flags & ERR_TXT_STRING)) data = NULL;

		ERR_error_string_n(error, buffer, sizeof(buffer));
		/* Extra verbose */
		if ((request && RDEBUG_ENABLED3) || DEBUG_ENABLED3) {
			ROPTIONAL(REDEBUG, ERROR, "%s[%i]:%s%c%s", file, line, buffer,
				  data ? ':' : '\0', data ? data : "");
		} else {
			ROPTIONAL(REDEBUG, ERROR, "%s%c%s", buffer,
				  data ? ':' : '\0', data ? data : "");
		}
		in_stack++;
	} while ((error = ERR_get_error_line_data(&file, &line, &data, &flags)));

	return in_stack;
}
DIAG_ON(format-nonliteral)

/** Print errors raised by OpenSSL I/O functions
 *
 * Drains the thread local OpenSSL error queue, and prints out errors
 * based on the SSL handle and the return code of the I/O  function.
 *
 * OpenSSL lists I/O functions to be:
 *   - SSL_connect
 *   - SSL_accept
 *   - SSL_do_handshake
 *   - SSL_read
 *   - SSL_peek
 *   - SSL_write
 *
 * @param request	The current request (may be NULL).
 * @param session	The current tls_session.
 * @param ret		from the I/O operation.
 * @param msg		Error message describing the operation being attempted.
 * @param ...		Arguments for msg.
 * @return
 *	- 0 TLS session may still be viable.
 *	- -1 TLS session cannot continue.
 */
int fr_tls_log_io_error(request_t *request, fr_tls_session_t *session, int ret, char const *msg, ...)
{
	int	error;
	va_list	ap;

	if (ERR_peek_error()) {
		va_start(ap, msg);
		fr_tls_log_error_va(request, msg, ap);
		va_end(ap);
	}

	error = SSL_get_error(session->ssl, ret);
	switch (error) {
	/*
	 *	These seem to be harmless and already "dealt
	 *	with" by our non-blocking environment. NB:
	 *	"ZERO_RETURN" is the clean "error"
	 *	indicating a successfully closed SSL
	 *	tunnel. We let this happen because our IO
	 *	loop should not appear to have broken on
	 *	this condition - and outside the IO loop, the
	 *	"shutdown" state is checked.
	 *
	 *	Don't print anything if we ignore the error.
	 */
	case SSL_ERROR_NONE:
	case SSL_ERROR_WANT_READ:
	case SSL_ERROR_WANT_WRITE:
	case SSL_ERROR_WANT_X509_LOOKUP:
	case SSL_ERROR_ZERO_RETURN:
		break;

	/*
	 *	These seem to be indications of a genuine
	 *	error that should result in the SSL tunnel
	 *	being regarded as "dead".
	 */
	case SSL_ERROR_SYSCALL:
		ROPTIONAL(REDEBUG, ERROR, "System call (I/O) error (%i)", ret);
		return -1;

	case SSL_ERROR_SSL:
		ROPTIONAL(REDEBUG, ERROR, "TLS protocol error (%i)", ret);
		return -1;

	/*
	 *	For any other errors that (a) exist, and (b)
	 *	crop up - we need to interpret what to do with
	 *	them - so "politely inform" the caller that
	 *	the code needs updating here.
	 */
	default:
		ROPTIONAL(REDEBUG, ERROR, "TLS session error %i (%i)", error, ret);
		return -1;
	}

	return 0;
}

/** Print errors in the TLS thread local error stack
 *
 * Drains the thread local OpenSSL error queue, and prints out errors.
 *
 * @param[in] request	The current request (may be NULL).
 * @param[in] msg	Error message describing the operation being attempted.
 * @param[in] ...	Arguments for msg.
 * @return the number of errors drained from the stack.
 */
int fr_tls_log_error(request_t *request, char const *msg, ...)
{
	va_list ap;
	int ret;

	va_start(ap, msg);
	ret = fr_tls_log_error_va(request, msg, ap);
	va_end(ap);

	return ret;
}

DIAG_OFF(format-nonliteral)
/** Print errors in the TLS thread local error stack
 *
 * Drains the thread local OpenSSL error queue, and prints out the first error
 * storing it in libfreeradius's error buffer.
 *
 * @param[in] msg	Error message describing the operation being attempted.
 * @param[in] ap	Arguments for msg.
 * @return the number of errors drained from the stack.
 */
static int tls_strerror_vasprintf(char const *msg, va_list ap)
{
	unsigned long	error;
	char		*p = NULL;
	int		drained = 0;
	char		buffer[256];

	int		line;
	char const	*file;
	char const	*data;
	int		flags = 0;

	/*
	 *	Pop the first error, so ERR_peek_error()
	 *	can be used to determine if there are
	 *	multiple errors.
	 */
	error = ERR_get_error_line_data(&file, &line, &data, &flags);
	if (!(flags & ERR_TXT_STRING)) data = NULL;

	if (msg) {
		/*
		 *	Print the error we were passed, and
		 *	OpenSSL's error.
		 */
		p = talloc_vasprintf(NULL, msg, ap);
		if (error) {
			ERR_error_string_n(error, buffer, sizeof(buffer));
			fr_strerror_printf("%s: %s%c%s", p, buffer, data ? ':' : '\0', data ? data : "");
			talloc_free(p);
			drained++;
		/*
		 *	Print the error we were given, irrespective
		 *	of whether there were any OpenSSL errors.
		 */
		} else {
			fr_strerror_printf("%s", p);
			talloc_free(p);
		}
	} else if (error) {
		ERR_error_string_n(error, buffer, sizeof(buffer));
		fr_strerror_printf("%s%c%s", buffer, data ? ':' : '\0', data ? data : "");
		drained++;
	} else {
		return 0;
	}

	while ((error = ERR_get_error_line_data(&file, &line, &data, &flags))) {
		if (!(flags & ERR_TXT_STRING)) data = NULL;

		ERR_error_string_n(error, buffer, sizeof(buffer));
		fr_strerror_printf_push("%s%c%s", buffer, data ? ':' : '\0', data ? data : "");
		drained++;
	}

	return drained;
}
DIAG_ON(format-nonliteral)

/** Wrapper around fr_strerror_printf to log error messages for library functions calling libssl
 *
 * @note Will only drain the first error.
 *
 * @param[in] msg	Error message describing the operation being attempted.
 * @param[in] ...	Arguments for msg.
 * @return the number of errors drained from the stack.
 */
int fr_tls_log_strerror_printf(char const *msg, ...)
{
	va_list ap;
	int ret;

	va_start(ap, msg);
	ret = tls_strerror_vasprintf(msg, ap);
	va_end(ap);

	return ret;
}

/** Clear errors in the TLS thread local error stack
 *
 */
void tls_log_clear(void)
{
	while (ERR_get_error() != 0);
}

/** Converts BIO_write() calls to request log calls
 *
 * This callback is used to glue the output of OpenSSL functions into request log calls.
 *
 * @param[in] bio	that was written to.
 * @param[in] in	data being written to BIO.
 * @param[in] len	Length of data being written.
 */
static int tls_log_request_bio_write_cb(BIO *bio, char const *in, int len)
{
	fr_tls_log_bio_t	*lb = talloc_get_type_abort(BIO_get_data(bio), fr_tls_log_bio_t);
	request_t		*request = talloc_get_type_abort(lb->request, request_t);
	log_request_func_t	func;
	char			*le;

	/*
	 *	Pick the right logging function based on the type
	 */
	if ((lb->type == L_ERR) || (lb->type == L_DBG_ERR) || (lb->type == L_DBG_ERR_REQ)) {
		func = log_request_error;
	} else {
		func = log_request;
	}

	/*
	 *	OpenSSL feeds us data in fragments so we need
	 *	to aggregate it, then look for new line chars
	 *	as an indication we need to print the line.
	 */
	fr_sbuff_in_bstrncpy(&lb->sbuff, in, len);

	/*
	 *	Split incoming data on new lines
	 */
	while (fr_sbuff_behind(&lb->logged_m)) {
		le = memchr(fr_sbuff_current(&lb->logged_m), '\n',
			    fr_sbuff_current(&lb->sbuff) - fr_sbuff_current(&lb->logged_m));
		/*
		 *	Wait until we have a complete line
		 */
		if (le == NULL) break;

		/*
		 *	Skip empty lines
		 */
		if ((le - fr_sbuff_current(&lb->logged_m)) > 0) {
			func(lb->type, lb->lvl, request, __FILE__, __LINE__, "%pV",
			     fr_box_strvalue_len(fr_sbuff_current(&lb->logged_m),
			     			 le - fr_sbuff_current(&lb->logged_m)));
		}

		fr_sbuff_set(&lb->logged_m, le + 1);
	}

	/*
	 *	Clear out printed data
	 */
	fr_sbuff_shift(&lb->sbuff, fr_sbuff_used(&lb->logged_m));

	return len;	/* Amount of data written */
}

/** Converts BIO_puts() calls to request log calls
 *
 * This callback is used to glue the output of OpenSSL functions into request log calls.
 *
 * @param[in] bio	that was written to.
 * @param[in] in	data being written to BIO.
 */
static int tls_log_request_bio_puts_cb(BIO *bio, char const *in)
{
	return tls_log_request_bio_write_cb(bio, in, strlen(in));
}

/** Converts BIO_write() calls to global log calls
 *
 * This callback is used to glue the output of OpenSSL functions into global log calls.
 *
 * @param[in] bio	that was written to.
 * @param[in] in	data being written to BIO.
 * @param[in] len	Length of data being written.
 */
static int tls_log_global_bio_write_cb(BIO *bio, char const *in, int len)
{
	fr_tls_log_bio_t	*lb = talloc_get_type_abort(BIO_get_data(bio), fr_tls_log_bio_t);
	char			*le;

	/*
	 *	OpenSSL feeds us data in fragments so we need
	 *	to aggregate it, then look for new line chars
	 *	as an indication we need to print the line.
	 */
	fr_sbuff_in_bstrncpy(&lb->sbuff, in, len);

	/*
	 *	Split incoming data on new lines
	 */
	while (fr_sbuff_behind(&lb->logged_m)) {
		le = memchr(fr_sbuff_current(&lb->logged_m), '\n',
			    fr_sbuff_current(&lb->sbuff) - fr_sbuff_current(&lb->logged_m));
		/*
		 *	Wait until we have a complete line
		 */
		if (le == NULL) break;

		/*
		 *	Skip empty lines
		 */
		if ((le - fr_sbuff_current(&lb->logged_m)) > 0) {
			if (fr_debug_lvl >= lb->lvl) fr_log(&default_log, lb->type, __FILE__, __LINE__,
							    "%pV",
							    fr_box_strvalue_len(fr_sbuff_current(&lb->logged_m),
										le - fr_sbuff_current(&lb->logged_m)));
		}

		fr_sbuff_set(&lb->logged_m, le + 1);
	}

	/*
	 *	Clear out printed data
	 */
	fr_sbuff_shift(&lb->sbuff, fr_sbuff_used(&lb->logged_m));

	return len;	/* Amount of data written */
}

/** Converts BIO_puts() calls to global log calls
 *
 * This callback is used to glue the output of OpenSSL functions into global log calls.
 *
 * @param[in] bio	that was written to.
 * @param[in] in	data being written to BIO.
 */
static int tls_log_global_bio_puts_cb(BIO *bio, char const *in)
{
	return tls_log_global_bio_write_cb(bio, in, strlen(in));
}

/** Frees a logging bio and its underlying OpenSSL BIO *
 *
 */
static void _fr_tls_log_bio_free(void *log_bio)
{
	fr_tls_log_bio_t	*our_log_bio = talloc_get_type_abort(log_bio, fr_tls_log_bio_t);

	BIO_free(our_log_bio->bio);
	our_log_bio->bio = NULL;
	talloc_free(our_log_bio);
}

/** Return a request log BIO to use with OpenSSL logging functions
 *
 * @note The contents of the BIO will only be written to the logging system on finding
 *       a new line.  If data remains in the BIO when it is re-initialised (this function
 *       is called again), that data will be discarded.
 *
 * @note The returned BIO should be assumed to be invalid if the request yields.
 *
 * @param[in] file	of caller.
 * @Parma[in] line	of caller.
 * @param[in] request	to temporarily associate with logging BIO.
 * @param[in] type	to temporarily assign to logging bio.
 * @param[in] lvl	to temporarily assign to logging bio.
 * @return A thread local BIO to pass to OpenSSL logging functions.
 */
BIO *_fr_tls_request_log_bio(char const *file, int line, request_t *request, fr_log_type_t type, fr_log_lvl_t lvl)
{
	if (unlikely(!request_log_bio)) {
		fr_tls_log_bio_t	*lb;

		MEM(lb = talloc(NULL, fr_tls_log_bio_t));
		*lb = (fr_tls_log_bio_t) {
			.bio = BIO_new(tls_request_log_meth),
			.request = request,
			.type = type,
			.lvl = lvl,
			.file = file,
			.line = line
		};
		MEM(lb->bio);
		BIO_set_data(lb->bio, lb);	/* So we can retrieve the fr_tls_lb_t in the callbacks */
		fr_sbuff_init_talloc(lb, &lb->sbuff, &lb->tctx, 1024, 10 * 1024);	/* start 1k, max 10k */
		fr_atexit_thread_local(request_log_bio, _fr_tls_log_bio_free, lb);
		fr_sbuff_marker(&lb->logged_m, &lb->sbuff);
		return lb->bio;
	}

	fr_sbuff_set(&request_log_bio->logged_m, fr_sbuff_start(&request_log_bio->sbuff));
	fr_sbuff_reset_talloc(&request_log_bio->sbuff);	/* Reset to initial size */
	request_log_bio->request = request;
	request_log_bio->type = type;
	request_log_bio->lvl = lvl;
	request_log_bio->file = file;
	request_log_bio->line = line;

	return request_log_bio->bio;
}

/** Return a global log BIO to use with OpenSSL logging functions
 *
 * @note The contents of the BIO will only be written to the logging system on finding
 *       a new line.  If data remains in the BIO when it is re-initialised (this function
 *       is called again), that data will be discarded.
 *
 * @note The returned BIO should be assumed to be invalid if the current request yields.
 *
 * @param[in] file	of caller.
 * @Parma[in] line	of caller.
 * @param[in] type	to temporarily assign to logging bio.
 * @param[in] lvl	to temporarily assign to logging bio.
 * @return A thread local BIO to pass to OpenSSL logging functions.
 */
BIO *_fr_tls_global_log_bio(char const *file, int line, fr_log_type_t type, fr_log_lvl_t lvl)
{
	if (unlikely(!global_log_bio)) {
		fr_tls_log_bio_t	*lb;

		MEM(lb = talloc(NULL, fr_tls_log_bio_t));
		*lb = (fr_tls_log_bio_t) {
			.bio = BIO_new(tls_global_log_meth),
			.type = type,
			.lvl = lvl,
			.file = file,
			.line = line
		};
		MEM(lb->bio);
		BIO_set_data(lb->bio, lb);	/* So we can retrieve the fr_tls_lb_t in the callbacks */
		fr_sbuff_init_talloc(lb, &lb->sbuff, &lb->tctx, 1024, 10 * 1024);	/* start 1k, max 10k */
		fr_atexit_thread_local(global_log_bio, _fr_tls_log_bio_free, lb);
		fr_sbuff_marker(&lb->logged_m, &lb->sbuff);
		return lb->bio;
	}

	fr_sbuff_set(&global_log_bio->logged_m, fr_sbuff_start(&global_log_bio->sbuff));
	fr_sbuff_reset_talloc(&request_log_bio->sbuff);	/* Reset to initial size */
	global_log_bio->type = type;
	global_log_bio->lvl = lvl;
	global_log_bio->file = file;
	global_log_bio->line = line;

	return global_log_bio->bio;
}

/** Initialise the BIO logging meths which are used to create thread local logging BIOs
 *
 */
int fr_tls_log_init(void)
{
	/*
	 *	As per the boringSSL documentation
	 *
	 *	BIO_TYPE_START is the first user-allocated |BIO| type.
	 *	No pre-defined type, flag bits aside, may exceed this
	 *	value.
	 *
	 *	The low byte here defines the BIO ID, and the high byte
	 *	defines its capabilities.
	 */
	tls_request_log_meth = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "fr_tls_request_log");
	if (unlikely(!tls_request_log_meth)) return -1;

	BIO_meth_set_write(tls_request_log_meth, tls_log_request_bio_write_cb);
	BIO_meth_set_puts(tls_request_log_meth, tls_log_request_bio_puts_cb);

	tls_global_log_meth = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "fr_tls_global_log");
	if (unlikely(!tls_global_log_meth)) {
		BIO_meth_free(tls_request_log_meth);
		tls_request_log_meth = NULL;
		return -1;
	}

	BIO_meth_set_write(tls_global_log_meth, tls_log_global_bio_write_cb);
	BIO_meth_set_puts(tls_global_log_meth, tls_log_global_bio_puts_cb);

	return 0;
}

/** Free the global log method templates
 *
 */
void fr_tls_log_free(void)
{
	if (tls_request_log_meth) {
		BIO_meth_free(tls_request_log_meth);
		tls_request_log_meth = NULL;
	}

	if (tls_global_log_meth) {
		BIO_meth_free(tls_global_log_meth);
		tls_global_log_meth = NULL;
	}
}
#endif /* WITH_TLS */
