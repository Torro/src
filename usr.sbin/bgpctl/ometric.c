/*	$OpenBSD: ometric.c,v 1.4 2022/12/01 09:14:40 claudio Exp $ */

/*
 * Copyright (c) 2022 Claudio Jeker <claudio@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/queue.h>

#include <err.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ometric.h"

struct olabel {
	STAILQ_ENTRY(olabel)	 entry;
	const char		*key;
	char			*value;
};

struct olabels {
	STAILQ_HEAD(, olabel)	 labels;
	struct olabels		*next;
	int		  	 refcnt;
};

enum ovalue_type {
	OVT_INTEGER,
	OVT_DOUBLE,
};

struct ovalue {
	STAILQ_ENTRY(ovalue)	 entry;
	struct olabels		*labels;
	union {
		unsigned long long	i;
		double			f;
	}			 value;
	enum ovalue_type	 valtype;
};

STAILQ_HEAD(ovalues, ovalue);

struct ometric {
	STAILQ_ENTRY(ometric)	 entry;
	struct ovalues		 vals;
	const char		*name;
	const char		*help;
	const char *const	*stateset;
	size_t			 setsize;
	enum ometric_type	 type;
};

STAILQ_HEAD(, ometric)	ometrics = STAILQ_HEAD_INITIALIZER(ometrics);

/*
 * Allocate and return new ometric. The name and help string need to remain
 * valid until the ometric is freed. Normally constant strings should be used.
 */
struct ometric *
ometric_new(enum ometric_type type, const char *name, const char *help)
{
	struct ometric *om;

	if ((om = calloc(1, sizeof(*om))) == NULL)
		err(1, NULL);

	om->name = name;
	om->help = help;
	om->type = type;
	STAILQ_INIT(&om->vals);

	STAILQ_INSERT_TAIL(&ometrics, om, entry);

	return om;
}

/*
 * Same as above but for a stateset. The states is an array of constant strings
 * with statecnt elements. The states, name and help pointers need to remain
 * valid until the ometric is freed.
 */
struct ometric *
ometric_new_state(const char * const *states, size_t statecnt, const char *name,
    const char *help)
{
	struct ometric *om;

	if ((om = calloc(1, sizeof(*om))) == NULL)
		err(1, NULL);

	om->name = name;
	om->help = help;
	om->type = OMT_STATESET;
	om->stateset = states;
	om->setsize = statecnt;
	STAILQ_INIT(&om->vals);

	STAILQ_INSERT_TAIL(&ometrics, om, entry);

	return om;
}

void
ometric_free_all(void)
{
	struct ometric *om;
	struct ovalue *ov;

	while ((om = STAILQ_FIRST(&ometrics)) != NULL) {
		STAILQ_REMOVE_HEAD(&ometrics, entry);
		while ((ov = STAILQ_FIRST(&om->vals)) != NULL) {
			STAILQ_REMOVE_HEAD(&om->vals, entry);
			olabels_free(ov->labels);
			free(ov);
		}
		free(om);
	}
}

static struct olabels *
olabels_ref(struct olabels *ol)
{
	struct olabels *x = ol;

	while (x != NULL) {
		x->refcnt++;
		x = x->next;
	}

	return ol;
}

/*
 * Create a new set of labels based on keys and values arrays.
 * keys must end in a NULL element. values needs to hold as many elements
 * but the elements can be NULL. values are copied for the olabel but
 * keys needs to point to constant memory.
 */
struct olabels *
olabels_new(const char * const *keys, const char **values)
{
	struct olabels *ol;
	struct olabel  *l;

	if ((ol = malloc(sizeof(*ol))) == NULL)
		err(1, NULL);
	STAILQ_INIT(&ol->labels);
	ol->refcnt = 1;
	ol->next = NULL;

	while (*keys != NULL) {
		if (*values && **values != '\0') {
			if ((l = malloc(sizeof(*l))) == NULL)
				err(1, NULL);
			l->key = *keys;
			if ((l->value = strdup(*values)) == NULL)
				err(1, NULL);
			STAILQ_INSERT_TAIL(&ol->labels, l, entry);
		}

		keys++;
		values++;
	}

	return ol;
}

/*
 * Free olables once nothing uses them anymore.
 */
void
olabels_free(struct olabels *ol)
{
	struct olabels *next;
	struct olabel  *l;

	for ( ; ol != NULL; ol = next) {
		next = ol->next;

		if (--ol->refcnt == 0) {
			while ((l = STAILQ_FIRST(&ol->labels)) != NULL) {
				STAILQ_REMOVE_HEAD(&ol->labels, entry);
				free(l->value);
				free(l);
			}
			free(ol);
		}
	}
}

/*
 * Add one extra label onto the label stack. Once no longer used the
 * value needs to be freed with olabels_free().
 */
static struct olabels *
olabels_add_extras(struct olabels *ol, const char **keys, const char **values)
{
	struct olabels *new;

	new = olabels_new(keys, values);
	new->next = olabels_ref(ol);

	return new;
}

/*
 * Output function called last.
 */
static const char *
ometric_type(enum ometric_type type)
{
	switch (type) {
	case OMT_GAUGE:
		return "gauge";
	case OMT_COUNTER:
		return "counter";
	case OMT_STATESET:
		return "stateset";
	case OMT_HISTOGRAM:
		return "histogram";
	case OMT_SUMMARY:
		return "summary";
	default:
		return "unknown";
	}
}

static int
ometric_output_labels(FILE *out, const struct olabels *ol)
{
	struct olabel *l;
	const char *comma = "";

	if (ol == NULL)
		return fprintf(out, " ");

	if (fprintf(out, "{") < 0)
		return -1;

	while (ol != NULL) {
		STAILQ_FOREACH(l, &ol->labels, entry) {
			if (fprintf(out, "%s%s=\"%s\"", comma, l->key,
			    l->value) < 0)
				return -1;
			comma = ",";
		}
		ol = ol->next;
	}

	return fprintf(out, "} ");
}

static int
ometric_output_value(FILE *out, const struct ovalue *ov)
{
	switch (ov->valtype) {
	case OVT_INTEGER:
		return fprintf(out, "%llu", ov->value.i);
	case OVT_DOUBLE:
		return fprintf(out, "%g", ov->value.f);
	}
	return -1;
}

/*
 * Output all metric values with TYPE and optional HELP strings.
 */
int
ometric_output_all(FILE *out)
{
	struct ometric *om;
	struct ovalue *ov;

	STAILQ_FOREACH(om, &ometrics, entry) {
		if (om->help)
			if (fprintf(out, "# HELP %s %s\n", om->name,
			    om->help) < 0)
				return -1;

		if (fprintf(out, "# TYPE %s %s\n", om->name,
		    ometric_type(om->type)) < 0)
			return -1;

		STAILQ_FOREACH(ov, &om->vals, entry) {
			if (fprintf(out, "%s", om->name) < 0)
				return -1;
			if (ometric_output_labels(out, ov->labels) < 0)
				return -1;
			if (ometric_output_value(out, ov) < 0)
				return -1;
			if (fprintf(out, "\n") < 0)
				return -1;
		}
	}

	if (fprintf(out, "# EOF\n") < 0)
		return -1;
	return 0;
}

/*
 * Value setters
 */
static void
ometric_set_int_value(struct ometric *om, uint64_t val, struct olabels *ol)
{
	struct ovalue *ov;

	if ((ov = malloc(sizeof(*ov))) == NULL)
		err(1, NULL);

	ov->value.i = val;
	ov->valtype = OVT_INTEGER;
	ov->labels = olabels_ref(ol);

	STAILQ_INSERT_TAIL(&om->vals, ov, entry);
}

/*
 * Set an integer value with label ol. ol can be NULL.
 */
void
ometric_set_int(struct ometric *om, uint64_t val, struct olabels *ol)
{
	if (om->type != OMT_COUNTER && om->type != OMT_GAUGE)
		errx(1, "%s incorrect ometric type", __func__);

	ometric_set_int_value(om, val, ol);
}

/*
 * Set a floating point value with label ol. ol can be NULL.
 */
void
ometric_set_float(struct ometric *om, double val, struct olabels *ol)
{
	struct ovalue *ov;

	if (om->type != OMT_COUNTER && om->type != OMT_GAUGE)
		errx(1, "%s incorrect ometric type", __func__);

	if ((ov = malloc(sizeof(*ov))) == NULL)
		err(1, NULL);

	ov->value.f = val;
	ov->valtype = OVT_DOUBLE;
	ov->labels = olabels_ref(ol);

	STAILQ_INSERT_TAIL(&om->vals, ov, entry);
}

/*
 * Add an info value (which is the value 1 but with extra key-value pairs).
 */
void
ometric_set_info(struct ometric *om, const char **keys, const char **values,
    struct olabels *ol)
{
	struct olabels *extra = NULL;

	if (om->type != OMT_INFO)
		errx(1, "%s incorrect ometric type", __func__);

	if (keys != NULL)
		extra = olabels_add_extras(ol, keys, values);

	ometric_set_int_value(om, 1, extra != NULL ? extra : ol);
	olabels_free(extra);
}

/*
 * Set a stateset to one of its states.
 */
void
ometric_set_state(struct ometric *om, const char *state, struct olabels *ol)
{
	size_t i;
	int val;

	if (om->type != OMT_STATESET)
		errx(1, "%s incorrect ometric type", __func__);

	for (i = 0; i < om->setsize; i++) {	
		if (strcasecmp(state, om->stateset[i]) == 0)
			val = 1;
		else
			val = 0;

		ometric_set_int_with_labels(om, val, OKV(om->name),
		    OKV(om->stateset[i]), ol);
	}
}

/*
 * Set a value with an extra label, the key should be a constant string while
 * the value is copied into the extra label.
 */
void
ometric_set_int_with_labels(struct ometric *om, uint64_t val,
    const char **keys, const char **values, struct olabels *ol)
{
	struct olabels *extra;

	extra = olabels_add_extras(ol, keys, values);
	ometric_set_int(om, val, extra);
	olabels_free(extra);
}
