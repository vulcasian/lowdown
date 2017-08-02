/*	$Id$ */
/*
 * Copyright (c) 2008, Natacha Porté
 * Copyright (c) 2011, Vicent Martí
 * Copyright (c) 2014, Xavier Mendez, Devin Torres and the Hoedown authors
 * Copyright (c) 2016--2017, Kristaps Dzonsons
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
#include "config.h"

#include <sys/queue.h>

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lowdown.h"
#include "extern.h"

/*
 * Queue entry for header names.
 * Keep these so we can make sure that headers have a unique "id" for
 * themselves.
 */
struct	hentry {
	char		*str; /* header name (raw) */
	size_t	 	 count; /* references */
	TAILQ_ENTRY(hentry) entries;
};

typedef struct html_state {
	struct {
		int header_count;
		int current_level;
		int level_offset;
		int nesting_level;
	} toc_data;
	TAILQ_HEAD(, hentry) headers_used;
	unsigned int flags;
} html_state;

static void
escape_html(hbuf *ob, const uint8_t *source, size_t length)
{

	hesc_html(ob, source, length, 0);
}

static void
escape_href(hbuf *ob, const uint8_t *source, size_t length)
{

	hesc_href(ob, source, length);
}

static int
rndr_autolink(hbuf *ob, const hbuf *link, halink_type type, void *data, int nln)
{

	if (!link || !link->size)
		return 0;

	HBUF_PUTSL(ob, "<a href=\"");
	if (type == HALINK_EMAIL)
		HBUF_PUTSL(ob, "mailto:");
	escape_href(ob, link->data, link->size);
	HBUF_PUTSL(ob, "\">");

	/*
	 * Pretty printing: if we get an email address as
	 * an actual URI, e.g. `mailto:foo@bar.com`, we don't
	 * want to print the `mailto:` prefix
	 */
	if (hbuf_prefix(link, "mailto:") == 0) {
		escape_html(ob, link->data + 7, link->size - 7);
	} else {
		escape_html(ob, link->data, link->size);
	}

	HBUF_PUTSL(ob, "</a>");

	return 1;
}

static void
rndr_blockcode(hbuf *ob, const hbuf *text, const hbuf *lang, void *data)
{
	if (ob->size) hbuf_putc(ob, '\n');

	if (lang) {
		HBUF_PUTSL(ob, "<pre><code class=\"language-");
		escape_html(ob, lang->data, lang->size);
		HBUF_PUTSL(ob, "\">");
	} else {
		HBUF_PUTSL(ob, "<pre><code>");
	}

	if (text)
		escape_html(ob, text->data, text->size);

	HBUF_PUTSL(ob, "</code></pre>\n");
}

static void
rndr_blockquote(hbuf *ob, const hbuf *content, void *data)
{
	if (ob->size)
		hbuf_putc(ob, '\n');
	HBUF_PUTSL(ob, "<blockquote>\n");
	if (content)
		hbuf_put(ob, content->data, content->size);
	HBUF_PUTSL(ob, "</blockquote>\n");
}

static int
rndr_codespan(hbuf *ob, const hbuf *text, void *data, int nln)
{

	HBUF_PUTSL(ob, "<code>");
	if (text)
		escape_html(ob, text->data, text->size);
	HBUF_PUTSL(ob, "</code>");
	return 1;
}

static int
rndr_strikethrough(hbuf *ob, const hbuf *content, void *data, int nln)
{
	if (!content || !content->size)
		return 0;

	HBUF_PUTSL(ob, "<del>");
	hbuf_put(ob, content->data, content->size);
	HBUF_PUTSL(ob, "</del>");
	return 1;
}

static int
rndr_double_emphasis(hbuf *ob, const hbuf *content, void *data, int nln)
{
	if (!content || !content->size)
		return 0;

	HBUF_PUTSL(ob, "<strong>");
	hbuf_put(ob, content->data, content->size);
	HBUF_PUTSL(ob, "</strong>");

	return 1;
}

static int
rndr_emphasis(hbuf *ob, const hbuf *content, void *data, int nln)
{
	if (!content || !content->size) return 0;
	HBUF_PUTSL(ob, "<em>");
	if (content) hbuf_put(ob, content->data, content->size);
	HBUF_PUTSL(ob, "</em>");
	return 1;
}

static int
rndr_highlight(hbuf *ob, const hbuf *content, void *data, int nln)
{
	if (!content || !content->size)
		return 0;

	HBUF_PUTSL(ob, "<mark>");
	hbuf_put(ob, content->data, content->size);
	HBUF_PUTSL(ob, "</mark>");

	return 1;
}

static int
rndr_linebreak(hbuf *ob, void *data)
{

	hbuf_puts(ob, "<br/>\n");
	return 1;
}

/*
 * Given the header with non-empty content "header", fill "ob" with the
 * identifier used for the header.
 * This will reference-count the header so we don't have duplicates.
 */
static void
rndr_header_id(hbuf *ob, const hbuf *header, html_state *state)
{
	struct hentry	*hentry;

	/* 
	 * See if the header was previously already defind. 
	 * Note that in HTML5, the identifier is case sensitive.
	 */

	TAILQ_FOREACH(hentry, &state->headers_used, entries) {
		if (strlen(hentry->str) != header->size)
			continue;
		if (0 == strncmp(hentry->str, 
		    (const char *)header->data, header->size))
			break;
	}

	/* Convert to escaped values. */

	escape_href(ob, header->data, header->size);

	/*
	 * If we're non-unique, then append a "count" value.
	 * XXX: if we have a header named "foo-2", then two headers
	 * named "foo", we'll inadvertently have a collision.
	 * This is a bit much to keep track of, though...
	 */

	if (NULL != hentry) {
		hentry->count++;
		hbuf_printf(ob, "-%zu", hentry->count);
		return;
	} 

	/* Create new header entry. */

	hentry = xcalloc(1, sizeof(struct hentry));
	hentry->count = 1;
	hentry->str = xstrndup
		((const char *)header->data, header->size);
	TAILQ_INSERT_TAIL(&state->headers_used, hentry, entries);
}

static void
rndr_header(hbuf *ob, const hbuf *content, int level, void *data)
{
	html_state	*state = data;

	if (ob->size)
		hbuf_putc(ob, '\n');

	if (level <= state->toc_data.nesting_level) {
		hbuf_printf(ob, "<h%d id=\"toc_%d\">", 
			level, state->toc_data.header_count);
		state->toc_data.header_count++;
	} else if (NULL != content && content->size) {
		hbuf_printf(ob, "<h%d id=\"", level);
		rndr_header_id(ob, content, state);
		HBUF_PUTSL(ob, "\">");
	} else
		hbuf_printf(ob, "<h%d>", level);

	if (NULL != content) 
		hbuf_put(ob, content->data, content->size);

	hbuf_printf(ob, "</h%d>\n", level);
}

static int
rndr_link(hbuf *ob, const hbuf *content, const hbuf *link, const hbuf *title, void *data, int nln)
{

	HBUF_PUTSL(ob, "<a href=\"");

	if (link && link->size)
		escape_href(ob, link->data, link->size);

	if (title && title->size) {
		HBUF_PUTSL(ob, "\" title=\"");
		escape_html(ob, title->data, title->size);
	}

	HBUF_PUTSL(ob, "\">");

	if (content && content->size)
		hbuf_put(ob, content->data, content->size);
	HBUF_PUTSL(ob, "</a>");
	return 1;
}

static void
rndr_list(hbuf *ob, const hbuf *content, hlist_fl flags, void *data)
{

	if (ob->size)
		hbuf_putc(ob, '\n');
	if (flags & HLIST_ORDERED)
		HBUF_PUTSL(ob, "<ol>\n");
	else
		HBUF_PUTSL(ob, "<ul>\n");
	if (content)
		hbuf_put(ob, content->data, content->size);
	if (flags & HLIST_ORDERED)
		HBUF_PUTSL(ob, "</ol>\n");
	else
		HBUF_PUTSL(ob, "</ul>\n");
}

static void
rndr_listitem(hbuf *ob, const hbuf *content, hlist_fl flags, void *data, size_t num)
{
	size_t	 size;

	HBUF_PUTSL(ob, "<li>");
	if (content) {
		size = content->size;
		while (size && content->data[size - 1] == '\n')
			size--;

		hbuf_put(ob, content->data, size);
	}
	HBUF_PUTSL(ob, "</li>\n");
}

static void
rndr_paragraph(hbuf *ob, const hbuf *content, void *data, size_t par_count)
{
	html_state *state = data;
	size_t i = 0;

	if (ob->size) hbuf_putc(ob, '\n');

	if (!content || !content->size)
		return;

	while (i < content->size && isspace(content->data[i])) i++;

	if (i == content->size)
		return;

	HBUF_PUTSL(ob, "<p>");
	if (state->flags & LOWDOWN_HTML_HARD_WRAP) {
		size_t org;
		while (i < content->size) {
			org = i;
			while (i < content->size && content->data[i] != '\n')
				i++;

			if (i > org)
				hbuf_put(ob, content->data + org, i - org);

			/*
			 * do not insert a line break if this newline
			 * is the last character on the paragraph
			 */
			if (i >= content->size - 1)
				break;

			rndr_linebreak(ob, data);
			i++;
		}
	} else {
		hbuf_put(ob, content->data + i, content->size - i);
	}
	HBUF_PUTSL(ob, "</p>\n");
}

static void
rndr_raw_block(hbuf *ob, const hbuf *text, void *data)
{
	size_t org, sz;

	if (!text)
		return;

	/* FIXME: Do we *really* need to trim the HTML? How does that make a difference? */
	sz = text->size;
	while (sz > 0 && text->data[sz - 1] == '\n')
		sz--;

	org = 0;
	while (org < sz && text->data[org] == '\n')
		org++;

	if (org >= sz)
		return;

	if (ob->size)
		hbuf_putc(ob, '\n');

	hbuf_put(ob, text->data + org, sz - org);
	hbuf_putc(ob, '\n');
}

static int
rndr_triple_emphasis(hbuf *ob, const hbuf *content, void *data, int nln)
{

	if (!content || !content->size)
		return 0;

	HBUF_PUTSL(ob, "<strong><em>");
	hbuf_put(ob, content->data, content->size);
	HBUF_PUTSL(ob, "</em></strong>");
	return 1;
}

static void
rndr_hrule(hbuf *ob, void *data)
{

	if (ob->size)
		hbuf_putc(ob, '\n');
	hbuf_puts(ob, "<hr/>\n");
}

static int
rndr_image(hbuf *ob, const hbuf *link, const hbuf *title, 
	const hbuf *dims, const hbuf *alt, void *data)
{
	char	 	dimbuf[32];
	int		x, y, rc = 0;

	/*
	 * Scan in our dimensions, if applicable.
	 * It's unreasonable for them to be over 32 characters, so use
	 * that as a cap to the size.
	 */

	if (NULL != dims && dims->size &&
	    dims->size < sizeof(dimbuf) - 1) {
		memset(dimbuf, 0, sizeof(dimbuf));
		memcpy(dimbuf, dims->data, dims->size);
		rc = sscanf(dimbuf, "%ux%u", &x, &y);
	}

	HBUF_PUTSL(ob, "<img src=\"");
	if (NULL != link)
		escape_href(ob, link->data, link->size);
	HBUF_PUTSL(ob, "\" alt=\"");
	if (NULL != alt && alt->size)
		escape_html(ob, alt->data, alt->size);
	HBUF_PUTSL(ob, "\"");

	if (NULL != dims && rc > 0) {
		hbuf_printf(ob, " width=\"%u\"", x);
		if (rc > 1)
			hbuf_printf(ob, " height=\"%u\"", y);
	}

	if (title && title->size) {
		HBUF_PUTSL(ob, " title=\"");
		escape_html(ob, title->data, title->size); 
		HBUF_PUTSL(ob, "\"");
	}

	hbuf_puts(ob, " />");
	return 1;
}

static int
rndr_raw_html(hbuf *ob, const hbuf *text, void *data)
{
	html_state *state = data;

	/* ESCAPE overrides SKIP_HTML. It doesn't look to see if
	 * there are any valid tags, just escapes all of them. */
	if((state->flags & LOWDOWN_HTML_ESCAPE) != 0) {
		escape_html(ob, text->data, text->size);
		return 1;
	}

	if ((state->flags & LOWDOWN_HTML_SKIP_HTML) != 0)
		return 1;

	hbuf_put(ob, text->data, text->size);
	return 1;
}

static void
rndr_table(hbuf *ob, const hbuf *content, void *data)
{

	if (ob->size)
		hbuf_putc(ob, '\n');
	HBUF_PUTSL(ob, "<table>\n");
	hbuf_put(ob, content->data, content->size);
	HBUF_PUTSL(ob, "</table>\n");
}

static void
rndr_table_header(hbuf *ob, const hbuf *content, 
	void *data, const htbl_flags *fl, size_t columns)
{

	if (ob->size)
		hbuf_putc(ob, '\n');
	HBUF_PUTSL(ob, "<thead>\n");
	hbuf_put(ob, content->data, content->size);
	HBUF_PUTSL(ob, "</thead>\n");
}

static void
rndr_table_body(hbuf *ob, const hbuf *content, void *data)
{

	if (ob->size)
		hbuf_putc(ob, '\n');
	HBUF_PUTSL(ob, "<tbody>\n");
	hbuf_put(ob, content->data, content->size);
	HBUF_PUTSL(ob, "</tbody>\n");
}

static void
rndr_tablerow(hbuf *ob, const hbuf *content, void *data)
{

	HBUF_PUTSL(ob, "<tr>\n");
	if (content)
		hbuf_put(ob, content->data, content->size);
	HBUF_PUTSL(ob, "</tr>\n");
}

static void
rndr_tablecell(hbuf *ob, const hbuf *content, htbl_flags flags, void *data, size_t col, size_t columns)
{

	if (flags & HTBL_FL_HEADER)
		HBUF_PUTSL(ob, "<th");
	else
		HBUF_PUTSL(ob, "<td");

	switch (flags & HTBL_FL_ALIGNMASK) {
	case HTBL_FL_ALIGN_CENTER:
		HBUF_PUTSL(ob, " style=\"text-align: center\">");
		break;
	case HTBL_FL_ALIGN_LEFT:
		HBUF_PUTSL(ob, " style=\"text-align: left\">");
		break;
	case HTBL_FL_ALIGN_RIGHT:
		HBUF_PUTSL(ob, " style=\"text-align: right\">");
		break;
	default:
		HBUF_PUTSL(ob, ">");
	}

	if (content)
		hbuf_put(ob, content->data, content->size);

	if (flags & HTBL_FL_HEADER)
		HBUF_PUTSL(ob, "</th>\n");
	else
		HBUF_PUTSL(ob, "</td>\n");
}

static int
rndr_superscript(hbuf *ob, const hbuf *content, void *data, int nln)
{

	if (!content || !content->size)
		return 0;

	HBUF_PUTSL(ob, "<sup>");
	hbuf_put(ob, content->data, content->size);
	HBUF_PUTSL(ob, "</sup>");
	return 1;
}

static void
rndr_normal_text(hbuf *ob, const hbuf *content, void *data, int nl)
{

	if (content)
		escape_html(ob, content->data, content->size);
}

static void
rndr_footnotes(hbuf *ob, const hbuf *content, void *data)
{

	if (ob->size)
		hbuf_putc(ob, '\n');
	HBUF_PUTSL(ob, "<div class=\"footnotes\">\n");
	hbuf_puts(ob, "<hr/>\n");
	HBUF_PUTSL(ob, "<ol>\n");

	if (content)
		hbuf_put(ob, content->data, content->size);

	HBUF_PUTSL(ob, "\n</ol>\n</div>\n");
}

static void
rndr_footnote_def(hbuf *ob, const hbuf *content, 
	unsigned int num, void *data)
{
	size_t i = 0;
	int pfound = 0;

	/* Insert anchor at the end of first paragraph block. */

	if (content) {
		while ((i+3) < content->size) {
			if (content->data[i++] != '<') 
				continue;
			if (content->data[i++] != '/') 
				continue;
			if (content->data[i++] != 'p' && 
			    content->data[i] != 'P') 
				continue;
			if (content->data[i] != '>') 
				continue;
			i -= 3;
			pfound = 1;
			break;
		}
	}

	hbuf_printf(ob, "\n<li id=\"fn%d\">\n", num);

	if (pfound) {
		hbuf_put(ob, content->data, i);
		hbuf_printf(ob, "&nbsp;"
			"<a href=\"#fnref%d\" rev=\"footnote\">"
			"&#8617;</a>", num);
		hbuf_put(ob, content->data + i, content->size - i);
	} else if (content) {
		hbuf_put(ob, content->data, content->size);
	}

	HBUF_PUTSL(ob, "</li>\n");
}

static int
rndr_footnote_ref(hbuf *ob, unsigned int num, void *data)
{

	hbuf_printf(ob, 
		"<sup id=\"fnref%d\">"
		"<a href=\"#fn%d\" rel=\"footnote\">"
		"%d</a></sup>", num, num, num);
	return 1;
}

static int
rndr_math(hbuf *ob, const hbuf *text, int displaymode, void *data)
{

	if (displaymode)
		HBUF_PUTSL(ob, "\\[");
	else
		HBUF_PUTSL(ob, "\\(");
	escape_html(ob, text->data, text->size);
	if (displaymode)
		HBUF_PUTSL(ob, "\\]");
	else
		HBUF_PUTSL(ob, "\\)");
	return 1;
}

void
lowdown_html_rndr(hbuf *ob, hrend *ref, const struct lowdown_node *root)
{
	const struct lowdown_node *n;
	hbuf	*tmp;

	tmp = hbuf_new(64);

	while (NULL != (n = TAILQ_FIRST(&root->children)))
		lowdown_html_rndr(tmp, ref, n);

	switch (n->type) {
	case (LOWDOWN_BLOCKCODE):
		rndr_blockcode(ob, tmp, 
			&n->rndr_blockcode.lang, 
			ref->opaque);
		break;
	case (LOWDOWN_BLOCKQUOTE):
		rndr_blockquote(ob, tmp, ref->opaque);
		break;
	case (LOWDOWN_HEADER):
		rndr_header(ob, tmp, 
			n->rndr_header.level,
			ref->opaque);
		break;
	case (LOWDOWN_HRULE):
		rndr_hrule(ob, ref->opaque);
		break;
	case (LOWDOWN_LIST):
		rndr_list(ob, tmp, 
			n->rndr_list.flags, 
			ref->opaque);
		break;
	case (LOWDOWN_LISTITEM):
		rndr_listitem(ob, tmp, 
			n->rndr_listitem.flags,
			ref->opaque, n->rndr_listitem.num);
		break;
	case (LOWDOWN_PARAGRAPH):
		rndr_paragraph(ob, tmp, ref->opaque, 0);
		break;
	case (LOWDOWN_TABLE_BLOCK):
		rndr_table(ob, tmp, ref->opaque);
		break;
	case (LOWDOWN_TABLE_HEADER):
		rndr_table_header(ob, tmp, ref->opaque,
			n->rndr_table_header.flags,
			n->rndr_table_header.columns);
		break;
	case (LOWDOWN_TABLE_BODY):
		rndr_table_body(ob, tmp, ref->opaque);
		break;
	case (LOWDOWN_TABLE_ROW):
		rndr_tablerow(ob, tmp, ref->opaque);
		break;
	case (LOWDOWN_TABLE_CELL):
		rndr_tablecell(ob, tmp, 
			n->rndr_table_cell.flags,
			ref->opaque,
			n->rndr_table_cell.col,
			n->rndr_table_cell.columns);
		break;
	case (LOWDOWN_FOOTNOTES_BLOCK):
		rndr_footnotes(ob, tmp, ref->opaque);
		break;
	case (LOWDOWN_FOOTNOTE_DEF):
		rndr_footnote_def(ob, tmp, 
			n->rndr_footnote_def.num,
			ref->opaque);
		break;
	case (LOWDOWN_BLOCKHTML):
		rndr_raw_block(ob, tmp, ref->opaque);
		break;
	case (LOWDOWN_LINK_AUTO):
		rndr_autolink(ob, 
			&n->rndr_autolink.link,
			n->rndr_autolink.type,
			ref->opaque, 0);
		break;
	case (LOWDOWN_CODESPAN):
		rndr_codespan(ob, tmp, ref->opaque, 0);
		break;
	case (LOWDOWN_DOUBLE_EMPHASIS):
		rndr_double_emphasis(ob, tmp, ref->opaque, 0);
		break;
	case (LOWDOWN_EMPHASIS):
		rndr_emphasis(ob, tmp, ref->opaque, 0);
		break;
	case (LOWDOWN_HIGHLIGHT):
		rndr_highlight(ob, tmp, ref->opaque, 0);
		break;
	case (LOWDOWN_IMAGE):
		rndr_image(ob, 
			&n->rndr_image.link,
			&n->rndr_image.title,
			&n->rndr_image.dims,
			&n->rndr_image.alt,
			ref->opaque);
		break;
	case (LOWDOWN_LINEBREAK):
		rndr_linebreak(ob, ref->opaque);
		break;
	case (LOWDOWN_LINK):
		rndr_link(ob, tmp, 
			&n->rndr_link.link,
			&n->rndr_link.text,
			ref->opaque, 0);
		break;
	case (LOWDOWN_TRIPLE_EMPHASIS):
		rndr_triple_emphasis(ob, tmp, ref->opaque, 0);
		break;
	case (LOWDOWN_STRIKETHROUGH):
		rndr_strikethrough(ob, tmp, ref->opaque, 0);
		break;
	case (LOWDOWN_SUPERSCRIPT):
		rndr_superscript(ob, tmp, ref->opaque, 0);
		break;
	case (LOWDOWN_FOOTNOTE_REF):
		rndr_footnote_ref(ob, 
			n->rndr_footnote_ref.num, 
			ref->opaque);
		break;
	case (LOWDOWN_MATH_BLOCK):
		rndr_math(ob, tmp, 
			n->rndr_math.displaymode,
			ref->opaque);
		break;
	case (LOWDOWN_RAW_HTML):
		rndr_raw_html(ob, tmp, ref->opaque);
		break;
	case (LOWDOWN_NORMAL_TEXT):
		rndr_normal_text(ob, tmp, ref->opaque, 0);
		break;
	default:
		break;
	}
}

/* allocates a regular HTML renderer */
hrend *
hrend_html_new(unsigned int render_flags, int nesting_level)
{
	static const hrend cb_default = {
		NULL,

		rndr_blockcode,
		rndr_blockquote,
		rndr_header,
		rndr_hrule,
		rndr_list,
		rndr_listitem,
		rndr_paragraph,
		rndr_table,
		rndr_table_header,
		rndr_table_body,
		rndr_tablerow,
		rndr_tablecell,
		rndr_footnotes,
		rndr_footnote_def,
		rndr_raw_block,

		rndr_autolink,
		rndr_codespan,
		rndr_double_emphasis,
		rndr_emphasis,
		rndr_highlight,
		rndr_image,
		rndr_linebreak,
		rndr_link,
		rndr_triple_emphasis,
		rndr_strikethrough,
		rndr_superscript,
		rndr_footnote_ref,
		rndr_math,
		rndr_raw_html,

		NULL,
		rndr_normal_text,
		NULL,

		NULL,
		NULL
	};

	html_state *state;
	hrend *renderer;

	/* Prepare the state pointer */
	state = xmalloc(sizeof(html_state));
	memset(state, 0x0, sizeof(html_state));
	TAILQ_INIT(&state->headers_used);

	state->flags = render_flags;
	state->toc_data.nesting_level = nesting_level;

	/* Prepare the renderer */
	renderer = xmalloc(sizeof(hrend));
	memcpy(renderer, &cb_default, sizeof(hrend));

	if (render_flags & LOWDOWN_HTML_SKIP_HTML || 
	    render_flags & LOWDOWN_HTML_ESCAPE)
		renderer->blockhtml = NULL;

	renderer->opaque = state;
	return renderer;
}

/* 
 * Deallocate an HTML renderer. 
 */
void
hrend_html_free(hrend *renderer)
{
	html_state	*state = renderer->opaque;
	struct hentry	*hentry;

	while (NULL != (hentry = TAILQ_FIRST(&state->headers_used))) {
		TAILQ_REMOVE(&state->headers_used, hentry, entries);
		free(hentry->str);
		free(hentry);
	}

	free(state);
	free(renderer);
}
