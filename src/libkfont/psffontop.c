/*
 * psffontop.c - aeb@cwi.nl, 990921
 */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sysexits.h>

#include "kfont.h"

#include "libcommon.h"

#include "contextP.h"
#include "paths.h"

static void
unicode_seq_free(struct unicode_seq *us)
{
	struct unicode_seq *us_next;
	while (us != NULL) {
		us_next = us->next;
		free(us);
		us = us_next;
	}
}

static void
unicode_list_free(struct unicode_list *up)
{
	struct unicode_list *ul, *ul_next;

	if (up == NULL)
		return;

	ul = up->next;

	while (ul != NULL) {
		ul_next = ul->next;
		unicode_seq_free(ul->seq);
		free(ul);
		ul = ul_next;
	}

	unicode_seq_free(up->seq);

	up->seq = NULL;
	up->next = NULL;
	up->prev = up;
}

void
unicode_list_heads_free(struct unicode_list **uclistheads, size_t fontlen)
{
	if (uclistheads == NULL || *uclistheads == NULL)
		return;

	for (size_t i = 0; i < fontlen; i++)
		unicode_list_free((*uclistheads) + i);

	free(*uclistheads);
	*uclistheads = NULL;
}

static int
addpair(struct kfont_ctx *ctx, struct unicode_list *up, unsigned int uc)
{
	struct unicode_list *ul = calloc(1, sizeof(struct unicode_list));
	struct unicode_seq  *us = calloc(1, sizeof(struct unicode_seq));

	if (ul == NULL || us == NULL)
		goto nomem;

	us->uc = uc;
	us->prev = us;
	us->next = NULL;

	ul->seq = us;
	ul->prev = up->prev;
	ul->prev->next = ul;
	ul->next = NULL;

	up->prev = ul;

	return 0;
nomem:
	free(ul);
	free(us);

	ERR(ctx, "out of memory");
	return -1;
}

static int
addseq(struct kfont_ctx *ctx, struct unicode_list *up, unsigned int uc)
{
	struct unicode_seq *us, *usl;
	struct unicode_list *ul = up->prev;

	if ((us = malloc(sizeof(struct unicode_seq))) == NULL) {
		ERR(ctx, "out of memory");
		return -1;
	}

	usl = ul->seq;
	while (usl && usl->next)
		usl = usl->next;

	us->uc = uc;
	us->next = NULL;

	if (usl) {
		us->prev = usl;
		usl->next = us;
	} else {
		us->prev = us;
	}

	return 0;
}

static unsigned int
assemble_int(unsigned char *ip)
{
	return (unsigned int) (ip[0] + (ip[1] << 8) + (ip[2] << 16) + (ip[3] << 24));
}

static void
store_int_le(unsigned char *ip, size_t num)
{
	ip[0] = (num & 0xff);
	ip[1] = ((num >> 8) & 0xff);
	ip[2] = ((num >> 16) & 0xff);
	ip[3] = ((num >> 24) & 0xff);
}

static int
assemble_ucs2(struct kfont_ctx *ctx, char **inptr, long cnt, unsigned int *res)
{
	unsigned int u1, u2;

	if (cnt < 2) {
		ERR(ctx, _("short ucs2 unicode table"));
		return -EX_DATAERR;
	}

	u1 = (unsigned char) *(*inptr)++;
	u2 = (unsigned char) *(*inptr)++;

	*res = (u1 | (u2 << 8));

	return 0;
}

/* called with cnt > 0 and **inptr not 0xff or 0xfe */
static int
assemble_utf8(struct kfont_ctx *ctx, char **inptr, long cnt, unsigned int *res)
{
	int err;
	unsigned long uc;

	uc = from_utf8(inptr, cnt, &err);
	if (err) {
		switch (err) {
			case UTF8_SHORT:
				ERR(ctx, _("short utf8 unicode table"));
				break;
			case UTF8_BAD:
				ERR(ctx, _("bad utf8"));
				break;
			default:
				ERR(ctx, _("unknown utf8 error"));
		}
		return -EX_DATAERR;
	}

	*res = uc;

	return 0;
}

/*
 * Read description of a single font position.
 */
static int
get_uni_entry(struct kfont_ctx *ctx, char **inptr, char **endptr, struct unicode_list *up, int utf8)
{
	unsigned char uc;
	unicode unichar;
	int rc, inseq = 0;

	up->next = NULL;
	up->seq = NULL;
	up->prev = up;

	while (1) {
		if (*endptr == *inptr) {
			ERR(ctx, _("short unicode table"));
			return -EX_DATAERR;
		}
		if (utf8) {
			uc = (unsigned char) *(*inptr)++;
			if (uc == PSF2_SEPARATOR)
				break;
			if (uc == PSF2_STARTSEQ) {
				inseq = 1;
				continue;
			}
			--(*inptr);

			rc = assemble_utf8(ctx, inptr, *endptr - *inptr, &unichar);
			if (rc < 0)
				return rc;
		} else {
			rc = assemble_ucs2(ctx, inptr, *endptr - *inptr, &unichar);
			if (rc < 0)
				return rc;

			if (unichar == PSF1_SEPARATOR)
				break;
			if (unichar == PSF1_STARTSEQ) {
				inseq = 1;
				continue;
			}
		}
		if (inseq < 2) {
			if (addpair(ctx, up, unichar) < 0)
				return -1;
		} else {
			if (addseq(ctx, up, unichar) < 0)
				return -1;
		}

		if (inseq)
			inseq++;
	}

	return 0;
}

int
kfont_read_file(struct kfont_ctx *ctx, FILE *fd, char **result, size_t *length)
{
	size_t n = 0;
	size_t len = 0;
	char *p, *buf = NULL;

	if (!fd)
		return 0;

	len = MAXFONTSIZE / 4; /* random */
	buf = malloc(len);

	if (!buf) {
		ERR(ctx, "out of memory");
		return -1;
	}

	/*
	 * We used to look at the length of the input file
	 * with stat(); now that we accept compressed files,
	 * just read the entire file.
	 */
	do {
		if (n >= len) {
			len *= 2;

			if ((p = realloc(buf, len)) == NULL) {
				free(buf);
				ERR(ctx, "out of memory");
				return -1;
			}
			buf = p;
		}

		n += fread(buf + n, 1, len - n, fd);

		if (ferror(fd)) {
			free(buf);
			ERR(ctx, _("Error reading input font"));
			return -EX_DATAERR;
		}

	} while(!feof(fd));

	if (n > 0 && len != n) {
		if ((p = realloc(buf, n)) == NULL) {
			free(buf);
			ERR(ctx, "out of memory");
			return -1;
		}
		buf = p;
	}

	*result = buf;
	*length = n;

	return 0;
}

/*
 * Read a psf font and return >= 0 on success and -1 on failure.
 * Failure means that the font was not psf (but has been read).
 * > 0 means that the Unicode table contains sequences.
 *
 * The font is read either from file (when FONT is non-NULL)
 * or from memory (namely from *ALLBUFP of size *ALLSZP).
 * In the former case, if ALLBUFP is non-NULL, a pointer to
 * the entire fontfile contents (possibly read from pipe)
 * is returned in *ALLBUFP, and the size in ALLSZP, where this
 * buffer was allocated using malloc().
 * In FONTBUFP, FONTSZP the subinterval of ALLBUFP containing
 * the font data is given.
 * The font width is stored in FONTWIDTHP.
 * The number of glyphs is stored in FONTLENP.
 * The unicodetable is stored in UCLISTHEADSP (when non-NULL), with
 * fontpositions counted from FONTPOS0 (so that calling this several
 * times can achieve font merging).
 */
int
kfont_read_psffont(struct kfont_ctx *ctx,
                   char *allbufp, size_t allszp,
                   char **fontbufp, size_t *fontszp,
                   size_t *fontwidthp, size_t *fontlenp, size_t fontpos0,
                   struct unicode_list **uclistheadsp)
{
	int utf8;
	char *inputbuf = NULL;
	size_t inputlth, fontlen, fontwidth, charsize, hastable, ftoffset;
	size_t i;
	void *p;
	int rc = 0;

	fontlen = 0;

	if (!allbufp || !allszp) {
		ERR(ctx, _("Bad call of readpsffont"));
		rc = -EX_SOFTWARE;
		goto end;
	}

	inputbuf = allbufp;
	inputlth = allszp;

	if (inputlth >= sizeof(struct psf1_header) && PSF1_MAGIC_OK((unsigned char *) inputbuf)) {
		struct psf1_header *psfhdr;

		psfhdr = (struct psf1_header *) &inputbuf[0];

		if (psfhdr->mode > PSF1_MAXMODE) {
			ERR(ctx, _("Unsupported psf file mode (%d)"), psfhdr->mode);
			rc = -EX_DATAERR;
			goto end;
		}

		fontlen = ((psfhdr->mode & PSF1_MODE512) ? 512 : 256);
		charsize = psfhdr->charsize;
		hastable = (psfhdr->mode & (PSF1_MODEHASTAB | PSF1_MODEHASSEQ));
		ftoffset = sizeof(struct psf1_header);
		fontwidth = 8;
		utf8 = 0;
	} else if (inputlth >= sizeof(struct psf2_header) && PSF2_MAGIC_OK((unsigned char *) inputbuf)) {
		struct psf2_header psfhdr;
		unsigned int flags;

		memcpy(&psfhdr, inputbuf, sizeof(struct psf2_header));

		if (psfhdr.version > PSF2_MAXVERSION) {
			ERR(ctx, _("Unsupported psf version (%d)"), psfhdr.version);
			rc = -EX_DATAERR;
			goto end;
		}

		fontlen = assemble_int((unsigned char *) &psfhdr.length);
		charsize = assemble_int((unsigned char *) &psfhdr.charsize);
		flags = assemble_int((unsigned char *) &psfhdr.flags);
		hastable = (flags & PSF2_HAS_UNICODE_TABLE);
		ftoffset = assemble_int((unsigned char *) &psfhdr.headersize);
		fontwidth = assemble_int((unsigned char *) &psfhdr.width);
		utf8 = 1;
	} else {
		rc = 1; /* not psf */
		goto end;
	}

	/* tests required - we divide by these */
	if (fontlen == 0) {
		ERR(ctx, _("zero input font length?"));
		rc = -EX_DATAERR;
		goto end;
	}

	if (charsize == 0) {
		ERR(ctx, _("zero input character size?"));
		rc = -EX_DATAERR;
		goto end;
	}

	i = ftoffset + fontlen * charsize;

	if (i > inputlth || (!hastable && i != inputlth)) {
		ERR(ctx, _("Input file: bad input length (%d)"), inputlth);
		rc = -EX_DATAERR;
		goto end;
	}

	if (fontbufp)
		*fontbufp = allbufp + ftoffset;

	if (fontszp)
		*fontszp = fontlen * charsize;

	if (fontlenp)
		*fontlenp = fontlen;

	if (fontwidthp)
		*fontwidthp = fontwidth;

	if (!uclistheadsp) {
		/* got font, don't need unicode_list */
		goto end;
	}

	p = realloc(*uclistheadsp, (fontpos0 + fontlen) * sizeof(struct unicode_list));

	if (p == NULL) {
		ERR(ctx, "out of memory");
		rc = -1;
		goto end;
	}
	*uclistheadsp = p;

	memset(p + (fontpos0 * sizeof(struct unicode_list)), 0, (fontlen * sizeof(struct unicode_list)));

	if (hastable) {
		char *inptr, *endptr;

		inptr = inputbuf + ftoffset + fontlen * charsize;
		endptr = inputbuf + inputlth;

		for (i = 0; i < fontlen; i++) {
			rc = get_uni_entry(ctx, &inptr, &endptr, &(*uclistheadsp)[fontpos0 + i], utf8);
			if (rc < 0)
				goto end;
		}

		if (inptr != endptr) {
			ERR(ctx, _("Input file: trailing garbage"));
			rc = -EX_DATAERR;
			goto end;
		}
	} else {
		for (i = 0; i < fontlen; i++) {
			unicode_list_free(&(*uclistheadsp)[fontpos0 + i]);
		}
	}

	return 0;
end:
	return rc; /* got psf font */
}

static int
has_sequences(struct unicode_list *uclistheads, size_t fontlen)
{
	struct unicode_list *ul;
	struct unicode_seq *us;
	size_t i;

	for (i = 0; i < fontlen; i++) {
		ul = uclistheads[i].next;
		while (ul) {
			us = ul->seq;
			if (us && us->next)
				return 1;
			ul = ul->next;
		}
	}
	return 0;
}

int
appendunicode(struct kfont_ctx *ctx, FILE *fp, unsigned int uc, int utf8)
{
	long ucl = uc;
	unsigned int n = 6;
	unsigned char out[6];
	char errbuf[STACKBUF_LEN];

	if (ucl & ~0x7fffffff) {
		ERR(ctx, _("appendunicode: illegal unicode %u"), uc);
		return -1;
	}
	if (!utf8) {
		out[--n] = ((uc >> 8) & 0xff);
		out[--n] = (uc & 0xff);
	} else if (uc < 0x80) {
		out[--n] = (unsigned char) uc;
	} else {
		unsigned int mask = 0x3f;
		while (uc & ~mask) {
			out[--n] = (unsigned char) (0x80 + (uc & 0x3f));
			uc >>= 6;
			mask >>= 1;
		}
		out[--n] = ((uc + ~mask + ~mask) & 0xff);
	}
	if (fwrite(out + n, 6 - n, 1, fp) != 1) {
		strerror_r(errno, errbuf, sizeof(errbuf));
		ERR(ctx, "appendunicode: %s", errbuf);
		return -1;
	}

	if (ctx->verbose > 1) {
		printf("(");
		if (!utf8)
			printf("U+");
		while (n < 6)
			printf("%02x ", out[n++]);
		printf(")");
	}

	return 0;
}

int
appendseparator(struct kfont_ctx *ctx, FILE *fp, int seq, int utf8)
{
	char errbuf[STACKBUF_LEN];
	size_t n;

	if (utf8) {
		unsigned char u = (seq ? PSF2_STARTSEQ : PSF2_SEPARATOR);
		n = fwrite(&u, sizeof(u), 1, fp);
	} else {
		unsigned short u = (seq ? PSF1_STARTSEQ : PSF1_SEPARATOR);
		n = fwrite(&u, sizeof(u), 1, fp);
	}
	if (n != 1) {
		strerror_r(errno, errbuf, sizeof(errbuf));
		ERR(ctx, "appendseparator: %s", errbuf);
		return -1;
	}
	return 0;
}

int
kfont_write_psffontheader(struct kfont_ctx *ctx,
                          FILE *ofil, size_t width, size_t height, size_t fontlen,
                          int *psftype, int flags)
{
	size_t bytewidth, charsize;
	size_t ret;

	bytewidth = (width + 7) / 8;
	charsize = bytewidth * height;

	if ((fontlen != 256 && fontlen != 512) || width != 8)
		*psftype = 2;

	if (*psftype == 2) {
		struct psf2_header h;
		unsigned int flags2 = 0;

		if (flags & WPSFH_HASTAB)
			flags2 |= PSF2_HAS_UNICODE_TABLE;

		h.magic[0] = PSF2_MAGIC0;
		h.magic[1] = PSF2_MAGIC1;
		h.magic[2] = PSF2_MAGIC2;
		h.magic[3] = PSF2_MAGIC3;

		store_int_le((unsigned char *) &h.version, 0);
		store_int_le((unsigned char *) &h.headersize, sizeof(h));
		store_int_le((unsigned char *) &h.flags, flags2);
		store_int_le((unsigned char *) &h.length, fontlen);
		store_int_le((unsigned char *) &h.charsize, charsize);
		store_int_le((unsigned char *) &h.width, width);
		store_int_le((unsigned char *) &h.height, height);

		ret = fwrite(&h, sizeof(h), 1, ofil);
	} else {
		struct psf1_header h;

		h.magic[0] = PSF1_MAGIC0;
		h.magic[1] = PSF1_MAGIC1;
		h.mode = 0;

		if (fontlen == 512)
			h.mode |= PSF1_MODE512;

		if (flags & WPSFH_HASSEQ)
			h.mode |= PSF1_MODEHASSEQ;
		else if (flags & WPSFH_HASTAB)
			h.mode |= PSF1_MODEHASTAB;

		h.charsize = (unsigned char) charsize;

		ret = fwrite(&h, sizeof(h), 1, ofil);
	}

	if (ret != 1) {
		ERR(ctx, _("Cannot write font file header"));
		return -EX_IOERR;
	}

	return 0;
}

int
kfont_write_psffont(struct kfont_ctx *ctx,
                    FILE *ofil, char *fontbuf, size_t width, size_t height, size_t fontlen,
                    int psftype, struct unicode_list *uclistheads)
{
	size_t bytewidth, charsize;
	int flags, utf8;
	size_t i;

	bytewidth = (width + 7) / 8;
	charsize = bytewidth * height;
	flags = 0;

	if (uclistheads) {
		flags |= WPSFH_HASTAB;
		if (has_sequences(uclistheads, fontlen))
			flags |= WPSFH_HASSEQ;
	}

	if (kfont_write_psffontheader(ctx, ofil, width, height, fontlen, &psftype, flags) < 0)
		return -EX_IOERR;

	utf8 = (psftype == 2);

	if ((fwrite(fontbuf, charsize, fontlen, ofil)) != fontlen) {
		ERR(ctx, _("Cannot write font file"));
		return -EX_IOERR;
	}

	/* unimaps: -1 => do nothing: caller will append map */
	if (uclistheads != NULL && uclistheads != (struct unicode_list *) -1) {
		struct unicode_list *ul;
		struct unicode_seq *us;

		for (i = 0; i < fontlen; i++) {
			ul = uclistheads[i].next;
			while (ul) {
				us = ul->seq;

				if (us && us->next) {
					if (appendseparator(ctx, ofil, 1, utf8) < 0)
						return -1; // exit 1
				}

				while (us) {
					if (appendunicode(ctx, ofil, us->uc, utf8) < 0)
						return -1; // exit 1
					us = us->next;
				}

				ul = ul->next;
			}

			if (appendseparator(ctx, ofil, 0, utf8) < 0)
				return -1; // exit 1
		}
	}

	return 0;
}
