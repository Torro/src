/* $OpenBSD: bio_lib.c,v 1.38 2022/11/30 01:56:18 jsing Exp $ */
/* Copyright (C) 1995-1998 Eric Young (eay@cryptsoft.com)
 * All rights reserved.
 *
 * This package is an SSL implementation written
 * by Eric Young (eay@cryptsoft.com).
 * The implementation was written so as to conform with Netscapes SSL.
 *
 * This library is free for commercial and non-commercial use as long as
 * the following conditions are aheared to.  The following conditions
 * apply to all code found in this distribution, be it the RC4, RSA,
 * lhash, DES, etc., code; not just the SSL code.  The SSL documentation
 * included with this distribution is covered by the same copyright terms
 * except that the holder is Tim Hudson (tjh@cryptsoft.com).
 *
 * Copyright remains Eric Young's, and as such any Copyright notices in
 * the code are not to be removed.
 * If this package is used in a product, Eric Young should be given attribution
 * as the author of the parts of the library used.
 * This can be in the form of a textual message at program startup or
 * in documentation (online or textual) provided with the package.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *    "This product includes cryptographic software written by
 *     Eric Young (eay@cryptsoft.com)"
 *    The word 'cryptographic' can be left out if the rouines from the library
 *    being used are not cryptographic related :-).
 * 4. If you include any Windows specific code (or a derivative thereof) from
 *    the apps directory (application code) you must include an acknowledgement:
 *    "This product includes software written by Tim Hudson (tjh@cryptsoft.com)"
 *
 * THIS SOFTWARE IS PROVIDED BY ERIC YOUNG ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * The licence and distribution terms for any publically available version or
 * derivative of this code cannot be changed.  i.e. this code cannot simply be
 * copied and put under another distribution licence
 * [including the GNU Public Licence.]
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>

#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/stack.h>

#include "bio_local.h"

/*
 * Helper function to work out whether to call the new style callback or the old
 * one, and translate between the two.
 *
 * This has a long return type for consistency with the old callback. Similarly
 * for the "long" used for "inret"
 */
static long
bio_call_callback(BIO *b, int oper, const char *argp, size_t len, int argi,
    long argl, long inret, size_t *processed)
{
	long ret;
	int bareoper;

	if (b->callback_ex != NULL)
		return b->callback_ex(b, oper, argp, len, argi, argl, inret,
		    processed);

	/*
	 * We have an old style callback, so we will have to do nasty casts and
	 * check for overflows.
	 */

	bareoper = oper & ~BIO_CB_RETURN;

	if (bareoper == BIO_CB_READ || bareoper == BIO_CB_WRITE ||
	    bareoper == BIO_CB_GETS) {
		/* In this case len is set and should be used instead of argi. */
		if (len > INT_MAX)
			return -1;
		argi = (int)len;
	}

	if (inret > 0 && (oper & BIO_CB_RETURN) && bareoper != BIO_CB_CTRL) {
		if (*processed > INT_MAX)
			return -1;
		inret = *processed;
	}

	ret = b->callback(b, oper, argp, argi, argl, inret);

	if (ret > 0 && (oper & BIO_CB_RETURN) && bareoper != BIO_CB_CTRL) {
		*processed = (size_t)ret;
		ret = 1;
	}

	return ret;
}

int
BIO_get_new_index(void)
{
	static int bio_type_index = BIO_TYPE_START;
	int index;

	/* The index will collide with the BIO flag bits if it exceeds 255. */
	index = CRYPTO_add(&bio_type_index, 1, CRYPTO_LOCK_BIO);
	if (index > 255)
		return -1;

	return index;
}

BIO *
BIO_new(const BIO_METHOD *method)
{
	BIO *ret = NULL;

	/* XXX calloc */
	ret = malloc(sizeof(BIO));
	if (ret == NULL) {
		BIOerror(ERR_R_MALLOC_FAILURE);
		return (NULL);
	}
	if (!BIO_set(ret, method)) {
		free(ret);
		ret = NULL;
	}
	return (ret);
}

int
BIO_set(BIO *bio, const BIO_METHOD *method)
{
	bio->method = method;
	bio->callback = NULL;
	bio->callback_ex = NULL;
	bio->cb_arg = NULL;
	bio->init = 0;
	bio->shutdown = 1;
	bio->flags = 0;
	bio->retry_reason = 0;
	bio->num = 0;
	bio->ptr = NULL;
	bio->next_bio = NULL;
	bio->references = 1;
	bio->num_read = 0L;
	bio->num_write = 0L;
	CRYPTO_new_ex_data(CRYPTO_EX_INDEX_BIO, bio, &bio->ex_data);
	if (method->create != NULL) {
		if (!method->create(bio)) {
			CRYPTO_free_ex_data(CRYPTO_EX_INDEX_BIO, bio,
			    &bio->ex_data);
			return (0);
		}
	}
	return (1);
}

int
BIO_free(BIO *a)
{
	int ret;

	if (a == NULL)
		return (0);

	if (CRYPTO_add(&a->references, -1, CRYPTO_LOCK_BIO) > 0)
		return (1);

	if (a->callback != NULL || a->callback_ex != NULL) {
		if ((ret = (int)bio_call_callback(a, BIO_CB_FREE, NULL, 0, 0,
		    0L, 1L, NULL)) <= 0)
			return (ret);
	}

	CRYPTO_free_ex_data(CRYPTO_EX_INDEX_BIO, a, &a->ex_data);

	if (a->method != NULL && a->method->destroy != NULL)
		a->method->destroy(a);
	free(a);
	return (1);
}

void
BIO_vfree(BIO *a)
{
	BIO_free(a);
}

int
BIO_up_ref(BIO *bio)
{
	int refs = CRYPTO_add(&bio->references, 1, CRYPTO_LOCK_BIO);
	return (refs > 1) ? 1 : 0;
}

void *
BIO_get_data(BIO *a)
{
	return (a->ptr);
}

void
BIO_set_data(BIO *a, void *ptr)
{
	a->ptr = ptr;
}

int
BIO_get_init(BIO *a)
{
	return a->init;
}

void
BIO_set_init(BIO *a, int init)
{
	a->init = init;
}

int
BIO_get_shutdown(BIO *a)
{
	return (a->shutdown);
}

void
BIO_set_shutdown(BIO *a, int shut)
{
	a->shutdown = shut;
}

void
BIO_clear_flags(BIO *b, int flags)
{
	b->flags &= ~flags;
}

int
BIO_test_flags(const BIO *b, int flags)
{
	return (b->flags & flags);
}

void
BIO_set_flags(BIO *b, int flags)
{
	b->flags |= flags;
}

BIO_callback_fn
BIO_get_callback(const BIO *b)
{
	return b->callback;
}

void
BIO_set_callback(BIO *b, BIO_callback_fn cb)
{
	b->callback = cb;
}

BIO_callback_fn_ex
BIO_get_callback_ex(const BIO *b)
{
	return b->callback_ex;
}

void
BIO_set_callback_ex(BIO *b, BIO_callback_fn_ex cb)
{
	b->callback_ex = cb;
}

void
BIO_set_callback_arg(BIO *b, char *arg)
{
	b->cb_arg = arg;
}

char *
BIO_get_callback_arg(const BIO *b)
{
	return b->cb_arg;
}

const char *
BIO_method_name(const BIO *b)
{
	return b->method->name;
}

int
BIO_method_type(const BIO *b)
{
	return b->method->type;
}

int
BIO_read(BIO *b, void *out, int outl)
{
	size_t readbytes = 0;
	int ret;

	if (b == NULL) {
		BIOerror(ERR_R_PASSED_NULL_PARAMETER);
		return (-1);
	}

	if (outl <= 0)
		return (0);

	if (out == NULL) {
		BIOerror(ERR_R_PASSED_NULL_PARAMETER);
		return (-1);
	}

	if (b->method == NULL || b->method->bread == NULL) {
		BIOerror(BIO_R_UNSUPPORTED_METHOD);
		return (-2);
	}

	if (b->callback != NULL || b->callback_ex != NULL) {
		if ((ret = (int)bio_call_callback(b, BIO_CB_READ, out, outl, 0,
		    0L, 1L, NULL)) <= 0)
			return (ret);
	}

	if (!b->init) {
		BIOerror(BIO_R_UNINITIALIZED);
		return (-2);
	}

	if ((ret = b->method->bread(b, out, outl)) > 0)
		readbytes = (size_t)ret;

	b->num_read += readbytes;

	if (b->callback != NULL || b->callback_ex != NULL) {
		ret = (int)bio_call_callback(b, BIO_CB_READ | BIO_CB_RETURN,
		    out, outl, 0, 0L, (ret > 0) ? 1 : ret, &readbytes);
	}

	if (ret > 0) {
		if (readbytes > INT_MAX) {
			BIOerror(BIO_R_LENGTH_TOO_LONG);
			ret = -1;
		} else {
			ret = (int)readbytes;
		}
	}

	return (ret);
}

int
BIO_write(BIO *b, const void *in, int inl)
{
	size_t writebytes = 0;
	int ret;

	if (b == NULL) {
		BIOerror(ERR_R_PASSED_NULL_PARAMETER);
		return (-1);
	}

	if (inl <= 0)
		return (0);

	if (in == NULL) {
		BIOerror(ERR_R_PASSED_NULL_PARAMETER);
		return (-1);
	}

	if (b->method == NULL || b->method->bwrite == NULL) {
		BIOerror(BIO_R_UNSUPPORTED_METHOD);
		return (-2);
	}

	if (b->callback != NULL || b->callback_ex != NULL) {
		if ((ret = (int)bio_call_callback(b, BIO_CB_WRITE, in, inl, 0,
		    0L, 1L, NULL)) <= 0)
			return (ret);
	}

	if (!b->init) {
		BIOerror(BIO_R_UNINITIALIZED);
		return (-2);
	}

	if ((ret = b->method->bwrite(b, in, inl)) > 0)
		writebytes = ret;

	b->num_write += writebytes;

	if (b->callback != NULL || b->callback_ex != NULL) {
		ret = (int)bio_call_callback(b, BIO_CB_WRITE | BIO_CB_RETURN,
		    in, inl, 0, 0L, (ret > 0) ? 1 : ret, &writebytes);
	}

	if (ret > 0) {
		if (writebytes > INT_MAX) {
			BIOerror(BIO_R_LENGTH_TOO_LONG);
			ret = -1;
		} else {
			ret = (int)writebytes;
		}
	}

	return (ret);
}

int
BIO_puts(BIO *b, const char *in)
{
	size_t writebytes = 0;
	int ret;

	if (b == NULL || b->method == NULL || b->method->bputs == NULL) {
		BIOerror(BIO_R_UNSUPPORTED_METHOD);
		return (-2);
	}

	if (b->callback != NULL || b->callback_ex != NULL) {
		if ((ret = (int)bio_call_callback(b, BIO_CB_PUTS, in, 0, 0, 0L,
		    1L, NULL)) <= 0)
			return (ret);
	}

	if (!b->init) {
		BIOerror(BIO_R_UNINITIALIZED);
		return (-2);
	}

	if ((ret = b->method->bputs(b, in)) > 0)
		writebytes = ret;

	b->num_write += writebytes;

	if (b->callback != NULL || b->callback_ex != NULL) {
		ret = (int)bio_call_callback(b, BIO_CB_PUTS | BIO_CB_RETURN,
		    in, 0, 0, 0L, (ret > 0) ? 1 : ret, &writebytes);
	}

	if (ret > 0) {
		if (writebytes > INT_MAX) {
			BIOerror(BIO_R_LENGTH_TOO_LONG);
			ret = -1;
		} else {
			ret = (int)writebytes;
		}
	}

	return (ret);
}

int
BIO_gets(BIO *b, char *in, int inl)
{
	size_t readbytes = 0;
	int ret;

	if (b == NULL || b->method == NULL || b->method->bgets == NULL) {
		BIOerror(BIO_R_UNSUPPORTED_METHOD);
		return (-2);
	}

	if (b->callback != NULL || b->callback_ex != NULL) {
		if ((ret = (int)bio_call_callback(b, BIO_CB_GETS, in, inl, 0, 0L,
		    1, NULL)) <= 0)
			return (ret);
	}

	if (!b->init) {
		BIOerror(BIO_R_UNINITIALIZED);
		return (-2);
	}

	if ((ret = b->method->bgets(b, in, inl)) > 0)
		readbytes = ret;

	if (b->callback != NULL || b->callback_ex != NULL) {
		ret = (int)bio_call_callback(b, BIO_CB_GETS | BIO_CB_RETURN, in,
		    inl, 0, 0L, (ret > 0) ? 1 : ret, &readbytes);
	}

	if (ret > 0) {
		if (readbytes > INT_MAX) {
			BIOerror(BIO_R_LENGTH_TOO_LONG);
			ret = -1;
		} else {
			ret = (int)readbytes;
		}
	}

	return (ret);
}

int
BIO_indent(BIO *b, int indent, int max)
{
	if (indent > max)
		indent = max;
	if (indent < 0)
		indent = 0;
	while (indent--)
		if (BIO_puts(b, " ") != 1)
			return 0;
	return 1;
}

long
BIO_int_ctrl(BIO *b, int cmd, long larg, int iarg)
{
	int i;

	i = iarg;
	return (BIO_ctrl(b, cmd, larg, (char *)&i));
}

char *
BIO_ptr_ctrl(BIO *b, int cmd, long larg)
{
	char *p = NULL;

	if (BIO_ctrl(b, cmd, larg, (char *)&p) <= 0)
		return (NULL);
	else
		return (p);
}

long
BIO_ctrl(BIO *b, int cmd, long larg, void *parg)
{
	long ret;

	if (b == NULL)
		return (0);

	if (b->method == NULL || b->method->ctrl == NULL) {
		BIOerror(BIO_R_UNSUPPORTED_METHOD);
		return (-2);
	}

	if (b->callback != NULL || b->callback_ex != NULL) {
		if ((ret = bio_call_callback(b, BIO_CB_CTRL, parg, 0, cmd, larg,
		    1L, NULL)) <= 0)
			return (ret);
	}

	ret = b->method->ctrl(b, cmd, larg, parg);

	if (b->callback != NULL || b->callback_ex != NULL) {
		ret = bio_call_callback(b, BIO_CB_CTRL | BIO_CB_RETURN, parg, 0,
		    cmd, larg, ret, NULL);
	}

	return (ret);
}

long
BIO_callback_ctrl(BIO *b, int cmd, BIO_info_cb *fp)
{
	long ret;

	if (b == NULL)
		return (0);

	if (b->method == NULL || b->method->callback_ctrl == NULL ||
	    cmd != BIO_CTRL_SET_CALLBACK) {
		BIOerror(BIO_R_UNSUPPORTED_METHOD);
		return (-2);
	}

	if (b->callback != NULL || b->callback_ex != NULL) {
		if ((ret = bio_call_callback(b, BIO_CB_CTRL, (void *)&fp, 0,
		    cmd, 0, 1L, NULL)) <= 0)
			return (ret);
	}

	ret = b->method->callback_ctrl(b, cmd, fp);

	if (b->callback != NULL || b->callback_ex != NULL) {
		ret = bio_call_callback(b, BIO_CB_CTRL | BIO_CB_RETURN,
		    (void *)&fp, 0, cmd, 0, ret, NULL);
	}

	return (ret);
}

/* It is unfortunate to duplicate in functions what the BIO_(w)pending macros
 * do; but those macros have inappropriate return type, and for interfacing
 * from other programming languages, C macros aren't much of a help anyway. */
size_t
BIO_ctrl_pending(BIO *bio)
{
	return BIO_ctrl(bio, BIO_CTRL_PENDING, 0, NULL);
}

size_t
BIO_ctrl_wpending(BIO *bio)
{
	return BIO_ctrl(bio, BIO_CTRL_WPENDING, 0, NULL);
}


/* put the 'bio' on the end of b's list of operators */
BIO *
BIO_push(BIO *b, BIO *bio)
{
	BIO *lb;

	if (b == NULL)
		return (bio);
	lb = b;
	while (lb->next_bio != NULL)
		lb = lb->next_bio;
	lb->next_bio = bio;
	/* called to do internal processing */
	BIO_ctrl(b, BIO_CTRL_PUSH, 0, lb);
	return (b);
}

/* Remove the first and return the rest */
BIO *
BIO_pop(BIO *b)
{
	BIO *ret;

	if (b == NULL)
		return (NULL);
	ret = b->next_bio;

	BIO_ctrl(b, BIO_CTRL_POP, 0, b);

	b->next_bio = NULL;
	return (ret);
}

BIO *
BIO_get_retry_BIO(BIO *bio, int *reason)
{
	BIO *b, *last;

	b = last = bio;
	for (;;) {
		if (!BIO_should_retry(b))
			break;
		last = b;
		b = b->next_bio;
		if (b == NULL)
			break;
	}
	if (reason != NULL)
		*reason = last->retry_reason;
	return (last);
}

int
BIO_get_retry_reason(BIO *bio)
{
	return (bio->retry_reason);
}

void
BIO_set_retry_reason(BIO *bio, int reason)
{
	bio->retry_reason = reason;
}

BIO *
BIO_find_type(BIO *bio, int type)
{
	int mt, mask;

	if (!bio)
		return NULL;
	mask = type & 0xff;
	do {
		if (bio->method != NULL) {
			mt = bio->method->type;
			if (!mask) {
				if (mt & type)
					return (bio);
			} else if (mt == type)
				return (bio);
		}
		bio = bio->next_bio;
	} while (bio != NULL);
	return (NULL);
}

BIO *
BIO_next(BIO *b)
{
	if (!b)
		return NULL;
	return b->next_bio;
}

void
BIO_set_next(BIO *b, BIO *next)
{
	b->next_bio = next;
}

void
BIO_free_all(BIO *bio)
{
	BIO *b;
	int ref;

	while (bio != NULL) {
		b = bio;
		ref = b->references;
		bio = bio->next_bio;
		BIO_free(b);
		/* Since ref count > 1, don't free anyone else. */
		if (ref > 1)
			break;
	}
}

BIO *
BIO_dup_chain(BIO *in)
{
	BIO *ret = NULL, *eoc = NULL, *bio, *new_bio;

	for (bio = in; bio != NULL; bio = bio->next_bio) {
		if ((new_bio = BIO_new(bio->method)) == NULL)
			goto err;
		new_bio->callback = bio->callback;
		new_bio->callback_ex = bio->callback_ex;
		new_bio->cb_arg = bio->cb_arg;
		new_bio->init = bio->init;
		new_bio->shutdown = bio->shutdown;
		new_bio->flags = bio->flags;

		/* This will let SSL_s_sock() work with stdin/stdout */
		new_bio->num = bio->num;

		if (!BIO_dup_state(bio, (char *)new_bio)) {
			BIO_free(new_bio);
			goto err;
		}

		/* copy app data */
		if (!CRYPTO_dup_ex_data(CRYPTO_EX_INDEX_BIO,
		    &new_bio->ex_data, &bio->ex_data))
			goto err;

		if (ret == NULL) {
			eoc = new_bio;
			ret = eoc;
		} else {
			BIO_push(eoc, new_bio);
			eoc = new_bio;
		}
	}
	return (ret);
err:
	BIO_free(ret);
	return (NULL);

}

void
BIO_copy_next_retry(BIO *b)
{
	BIO_set_flags(b, BIO_get_retry_flags(b->next_bio));
	b->retry_reason = b->next_bio->retry_reason;
}

int
BIO_get_ex_new_index(long argl, void *argp, CRYPTO_EX_new *new_func,
    CRYPTO_EX_dup *dup_func, CRYPTO_EX_free *free_func)
{
	return CRYPTO_get_ex_new_index(CRYPTO_EX_INDEX_BIO, argl, argp,
	    new_func, dup_func, free_func);
}

int
BIO_set_ex_data(BIO *bio, int idx, void *data)
{
	return (CRYPTO_set_ex_data(&(bio->ex_data), idx, data));
}

void *
BIO_get_ex_data(BIO *bio, int idx)
{
	return (CRYPTO_get_ex_data(&(bio->ex_data), idx));
}

unsigned long
BIO_number_read(BIO *bio)
{
	if (bio)
		return bio->num_read;
	return 0;
}

unsigned long
BIO_number_written(BIO *bio)
{
	if (bio)
		return bio->num_write;
	return 0;
}
