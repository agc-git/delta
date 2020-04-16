/*-
 * Copyright 2003-2005 Colin Percival
 * All rights reserved
 * Copyright (c) 2016 Alistair Crooks <agc@NetBSD.org>
 * All rights reserved
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted providing that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <sys/types.h>

#include <bzlib.h>
#include <err.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "delta.h"

/* an output buffer structure */
typedef struct obuf_t {
	uint64_t	 c;	/* # of characters used */
	uint64_t	 size;	/* output buffer size */
	uint8_t		*v;	/* the array of chars */
} obuf_t;

/* the binary diff structure */
typedef struct delta_t {
	obuf_t		 control;	/* output buffer for control info */
	uint8_t		*diff;		/* diff text */
	size_t		 difflen;	/* diff text length */
	uint8_t		*extra;		/* extra text */
	size_t		 extralen;	/* extra text length */
	off_t		 newsize;	/* size of output */
	uint8_t		 allocctl;	/* ctl was allocated */
	uint8_t		 allocdiff;	/* diff was allocated */
	uint8_t		 allocextra;	/* extra was allocated */
} delta_t;

#ifndef MIN
#define MIN(x,y) (((x) < (y)) ? (x) : (y))
#endif

#ifndef ABS
#define ABS(x)	(((x) < 0) ? -(x) : (x))
#endif

#ifndef howmany
#define	howmany(x, y)	(((x)+((y)-1))/(y))
#endif

/* add the info to the obuf buffer */
static int
owrite(obuf_t *obuf, const void *p, size_t size)
{
	uint8_t	*newv;
	size_t	 newsize;

	if (obuf->c + size >= obuf->size) {
		newsize = howmany(obuf->c + size, 4096) * 4096;
		newv = realloc(obuf->v, newsize);
		if (newv == NULL) {
			return 0;
		}
		obuf->size = newsize;
		obuf->v = newv;
	}
	memcpy(&obuf->v[obuf->c], p, size);
	obuf->c += size;
	return 1;
}

static void 
split(off_t *I, off_t *V, off_t start, off_t len, off_t h)
{
	off_t 		i, j, k, x, tmp, jj, kk;

	if (len < 16) {
		for (k = start; k < start + len; k += j) {
			j = 1;
			x = V[I[k] + h];
			for (i = 1; k + i < start + len; i++) {
				if (V[I[k + i] + h] < x) {
					x = V[I[k + i] + h];
					j = 0;
				}
				if (V[I[k + i] + h] == x) {
					tmp = I[k + j];
					I[k + j] = I[k + i];
					I[k + i] = tmp;
					j++;
				}
			}
			for (i = 0; i < j; i++)
				V[I[k + i]] = k + j - 1;
			if (j == 1)
				I[k] = -1;
		}
		return;
	}

	x = V[I[start + len / 2] + h];
	jj = 0;
	kk = 0;
	for (i = start; i < start + len; i++) {
		if (V[I[i] + h] < x)
			jj++;
		if (V[I[i] + h] == x)
			kk++;
	}
	jj += start;
	kk += jj;

	j = k = 0;
	for (i = start; i < jj ; ) {
		if (V[I[i] + h] < x) {
			i++;
		} else if (V[I[i] + h] == x) {
			tmp = I[i];
			I[i] = I[jj + j];
			I[jj + j] = tmp;
			j++;
		} else {
			tmp = I[i];
			I[i] = I[kk + k];
			I[kk + k] = tmp;
			k++;
		}
	}
	while (jj + j < kk) {
		if (V[I[jj + j] + h] == x) {
			j++;
		} else {
			tmp = I[jj + j];
			I[jj + j] = I[kk + k];
			I[kk + k] = tmp;
			k++;
		}
	}

	if (jj > start) {
		split(I, V, start, jj - start, h);
	}
	for (i = 0; i < kk - jj; i++) {
		V[I[jj + i]] = kk - 1;
	}
	if (jj == kk - 1) {
		I[jj] = -1;
	}
	if (start + len > kk) {
		split(I, V, kk, start + len - kk, h);
	}
}

static void 
qsufsort(off_t *I, off_t *V, const uint8_t *old, off_t oldsize)
{
	off_t 		buckets[256];
	off_t 		i, h, len;

	memset(buckets, 0x0, sizeof(buckets));
	for (i = 0; i < oldsize; i++) {
		buckets[old[i]]++;
	}
	for (i = 1; i < 256; i++) {
		buckets[i] += buckets[i - 1];
	}
	for (i = 255; i > 0; i--) {
		buckets[i] = buckets[i - 1];
	}
	buckets[0] = 0;

	for (i = 0; i < oldsize; i++) {
		I[++buckets[old[i]]] = i;
	}
	I[0] = oldsize;
	for (i = 0; i < oldsize; i++) {
		V[i] = buckets[old[i]];
	}
	V[oldsize] = 0;
	for (i = 1; i < 256; i++) {
		if (buckets[i] == buckets[i - 1] + 1) {
			I[buckets[i]] = -1;
		}
	}
	I[0] = -1;

	for (h = 1; I[0] != -(oldsize + 1); h += h) {
		for (len = 0, i = 0; i < oldsize + 1;) {
			if (I[i] < 0) {
				len -= I[i];
				i -= I[i];
			} else {
				if (len) {
					I[i - len] = -len;
				}
				len = V[I[i]] + 1 - i;
				split(I, V, i, len, h);
				i += len;
				len = 0;
			}
		}
		if (len) {
			I[i - len] = -len;
		}
	}

	for (i = 0; i < oldsize + 1; i++) {
		I[V[i]] = i;
	}
}

static off_t 
matchlen(const uint8_t *old, off_t oldsize, const uint8_t *newv, off_t newsize)
{
	off_t 		i;

	for (i = 0; i < oldsize && i < newsize && old[i] == newv[i] ; i++) {
	}
	return i;
}

static off_t 
search(off_t *I, const uint8_t *old, off_t oldsize,
       const uint8_t *newv, off_t newsize, off_t st, off_t en, off_t *pos)
{
	off_t 		x, y;

	if (en - st < 2) {
		x = matchlen(old + I[st], oldsize - I[st], newv, newsize);
		y = matchlen(old + I[en], oldsize - I[en], newv, newsize);
		if (x > y) {
			*pos = I[st];
			return x;
		} else {
			*pos = I[en];
			return y;
		}
	}
	x = st + (en - st) / 2;
	if (memcmp(old + I[x], newv, MIN(oldsize - I[x], newsize)) < 0) {
		return search(I, old, oldsize, newv, newsize, x, en, pos);
	} else {
		return search(I, old, oldsize, newv, newsize, st, x, pos);
	}
}

/* encode in zigzag encoding */
static int
put64(off_t x, uint8_t *buf)
{
	uint64_t	n;
	uint8_t		*b;
	int		 len;
	int		 i;

	n = (x << 1) ^ (x >> 63);
	for (len = 0, b = buf, i = 0 ; i < 9 ; i++, b += len) {
		*b = ((n >> ((9 - i) * 7)) & 0x7f) | 0x80;
		if (len == 0 && *b != 0x80) {
			len = 1;
		}
	}
	*b++ = (n & 0x7f);
	return (int)(b - buf);
}

/* decode from zigzag encoding */
static int64_t
get64(uint8_t *buf, size_t *len)
{
	uint64_t	n;

	for (n = 0, *len = 0 ; *len < 10 ; *len += 1) {
		n = (n << 7) + (buf[*len] & 0x7f);
                if ((buf[*len] & 0x80) == 0) {
			*len += 1;
                        break;
                }
	}
	return (n >> 1) ^ (-(n & 1));
}

/* read the file into the array */
static int
readfile(const char *f, uint8_t **v, off_t *size)
{
	ssize_t	cc;
	ssize_t	rc;
	int	fd;

	if (((fd = open(f, O_RDONLY, 0)) < 0) ||
	    ((*size = lseek(fd, 0, SEEK_END)) == -1) ||
	    ((*v = malloc(*size + 1)) == NULL) ||
	    (lseek(fd, 0, SEEK_SET) != 0)) {
		warn("can't read file '%s'", f);
		return 0;
	}
	for (cc = 0 ; cc < *size ; cc += rc) {
		if ((rc = read(fd, &(*v)[cc], *size - cc)) < 0) {
			break;
		}
	}
	close(fd);
	return cc == *size;
}

/* write the array to the file */
static int
writefile(const char *f, uint8_t *v, off_t size)
{
	ssize_t	cc;
	ssize_t	wc;
	int	fd;

	/* Write the new file */
	if ((fd = open(f, O_CREAT | O_TRUNC | O_WRONLY, 0666)) < 0) {
		warn("can't write file '%s'", f);
		return 0;
	}
	for (cc = 0 ; cc < size ; cc += wc) {
		if ((wc = write(fd, &v[cc], size - cc)) < 0) {
			break;
		}
	}
	close(fd);
	return cc == size;
}

/***********************************************************************/

#define HEADER		"/_\\1"	/* delta v1 */
#define HEADERLEN	4

/* write the patch file from info held in delta struct */
int
delta_write_patch_file(delta_t *delta, const char *patchfile)
{
	unsigned	 zsize;
	uint8_t		 buf[10];
	size_t		 cc;
	size_t		 wc;
	size_t		 n;
	obuf_t		 o;
	FILE		*fp;
	char		*z;

	if (delta == NULL || patchfile == NULL) {
		return 0;
	}
	memset(&o, 0x0, sizeof(o));
	owrite(&o, HEADER, HEADERLEN);
	n = put64(delta->control.c, buf);
	owrite(&o, buf, n);
	n = put64(delta->difflen, buf);
	owrite(&o, buf, n);
	n = put64(delta->extralen, buf);
	owrite(&o, buf, n);
	n = put64(delta->newsize, buf);
	owrite(&o, buf, n);
	if ((fp = fopen(patchfile, "w")) == NULL) {
		warn("can't open '%s' for writing", patchfile);
		return 0;
	}
	for (cc = 0 ; cc < o.c ; cc += wc) {
		if ((wc = fwrite(&o.v[cc], 1, o.c - cc, fp)) == 0) {
			break;
		}
	}
	o.c = 0;
	/* control info */
	owrite(&o, delta->control.v, delta->control.c);
	/* diff info */
	owrite(&o, delta->diff, delta->difflen);
	/* extra info */
	owrite(&o, delta->extra, delta->extralen);
	n = delta->control.c + delta->difflen + delta->extralen;
	zsize = ((n + 128) * 101) / 100;
	z = calloc(1, zsize);
	BZ2_bzBuffToBuffCompress(z, &zsize, (char *)o.v, o.c, 9, 0, 0);
	for (cc = 0 ; cc < zsize ; cc += wc) {
		if ((wc = fwrite(&z[cc], 1, zsize - cc, fp)) == 0) {
			break;
		}
	}
	free(z);
	free(o.v);
	fclose(fp);
	return cc == zsize;
}

/* read the patch file into the delta struct */
int
delta_read_patch_file(delta_t *delta, const char *patchfile)
{
	unsigned	 zsize;
	size_t		 len;
	size_t		 cc;
	obuf_t		 o;
	off_t	 	 size;
	char		*z;

	if (delta == NULL || patchfile == NULL) {
		return 0;
	}
	memset(delta, 0x0, sizeof(*delta));
	memset(&o, 0x0, sizeof(o));
	if (!readfile(patchfile, &o.v, &size)) {
		return 0;
	}
	o.c = o.size = size;
	if (memcmp(o.v, HEADER, HEADERLEN) != 0) {
		warnx("not a patch at offset 0");
		return 0;
	}
	cc = HEADERLEN;
	delta->control.c = get64(&o.v[cc], &len);
	cc += len;
	delta->difflen = get64(&o.v[cc], &len);
	cc += len;
	delta->extralen = get64(&o.v[cc], &len);
	cc += len;
	delta->newsize = get64(&o.v[cc], &len);
	cc += len;
	zsize = delta->control.c + delta->difflen + delta->extralen;
	if ((z = calloc(1, zsize)) == NULL) {
		warn("resource allocation reading patch file");
		return 0;
	}
	if (BZ2_bzBuffToBuffDecompress(z, &zsize, (char *)&o.v[cc], size - cc, 0, 0) != BZ_OK) {
		warn("resource allocation reading patch file");
		return 0;
	}
	delta->allocctl = 1;
	delta->control.v = (uint8_t *)z;
	delta->diff = (uint8_t *)&z[delta->control.c];
	delta->extra = (uint8_t *)&z[delta->control.c + delta->difflen];
	free(o.v);
	return 1;
}

/* diff 2 areas of memory */
int
delta_diff_mem(delta_t *delta, const void *aold, size_t oldc, const void *anewv, size_t newc)
{
	const uint8_t	*old = (const uint8_t *)aold;
	const uint8_t	*newv = (const uint8_t *)anewv;
	uint8_t		 buf[10];
	off_t		 oldsize = (off_t)oldc;
	off_t		 newsize = (off_t)newc;
	off_t		 *I, *V;
	off_t		 scan, pos, len;
	off_t		 lastscan, lastpos, lastoffset;
	off_t		 oldscore, scsc;
	off_t		 s, Sf, lenf, Sb, lenb;
	off_t		 overlap, Ss, lens;
	off_t		 i;
	int		 buflen;

	if (delta == NULL || old == NULL || newv == NULL) {
		return 0;
	}
	memset(delta, 0x0, sizeof(*delta));
	if (((I = malloc((oldsize + 1) * sizeof(*I))) == NULL) ||
	    ((V = malloc((oldsize + 1) * sizeof(*V))) == NULL)) {
		warn("allocation failure in bsd_diff_mem");
		return 0;
	}
	qsufsort(I, V, old, oldsize);
	free(V);
	/*
	 * Allocate newsize+1 bytes instead of newsize bytes to ensure that
	 * we never try to malloc(0) and get a NULL pointer
	 */
	delta->newsize = newsize;
	if (((delta->diff = malloc(newsize + 1)) == NULL) ||
	    ((delta->extra = malloc(newsize + 1)) == NULL)) {
		warn("allocation failure 2 in bsd_diff_mem");
		return 0;
	}
	delta->allocdiff = delta->allocextra = 1;
	scan = 0;
	len = 0;
	lastscan = 0;
	lastpos = 0;
	lastoffset = 0;
	while (scan < newsize) {
		oldscore = 0;
		for (scsc = scan += len; scan < newsize; scan++) {
			len = search(I, old, oldsize, newv + scan, newsize - scan,
				     0, oldsize, &pos);

			for (; scsc < scan + len; scsc++)
				if ((scsc + lastoffset < oldsize) &&
				    (old[scsc + lastoffset] == newv[scsc])) {
					oldscore++;
			}
			if (((len == oldscore) && (len != 0)) ||
			    (len > oldscore + 8)) {
				break;
			}
			if ((scan + lastoffset < oldsize) &&
			    (old[scan + lastoffset] == newv[scan])) {
				oldscore--;
			}
		}
		if ((len != oldscore) || (scan == newsize)) {
			s = 0;
			Sf = 0;
			lenf = 0;
			for (i = 0; (lastscan + i < scan) && (lastpos + i < oldsize);) {
				if (old[lastpos + i] == newv[lastscan + i])
					s++;
				i++;
				if (s * 2 - i > Sf * 2 - lenf) {
					Sf = s;
					lenf = i;
				}
			}
			lenb = 0;
			if (scan < newsize) {
				s = 0;
				Sb = 0;
				for (i = 1; (scan >= lastscan + i) && (pos >= i); i++) {
					if (old[pos - i] == newv[scan - i])
						s++;
					if (s * 2 - i > Sb * 2 - lenb) {
						Sb = s;
						lenb = i;
					}
				}
			}
			if (lastscan + lenf > scan - lenb) {
				overlap = (lastscan + lenf) - (scan - lenb);
				s = 0;
				Ss = 0;
				lens = 0;
				for (i = 0; i < overlap; i++) {
					if (newv[lastscan + lenf - overlap + i] ==
					  old[lastpos + lenf - overlap + i]) {
						s++;
					}
					if (newv[scan - lenb + i] ==
					    old[pos - lenb + i]) {
						s--;
					}
					if (s > Ss) {
						Ss = s;
						lens = i + 1;
					}
				}
				lenf += lens - overlap;
				lenb -= lens;
			}
			for (i = 0; i < lenf; i++) {
				delta->diff[delta->difflen + i] = newv[lastscan + i] - old[lastpos + i];
			}
			for (i = 0; i < (scan - lenb) - (lastscan + lenf); i++) {
				delta->extra[delta->extralen + i] = newv[lastscan + lenf + i];
			}
			delta->difflen += lenf;
			delta->extralen += (scan - lenb) - (lastscan + lenf);

			buflen = put64(lenf, buf);
			owrite(&delta->control, buf, buflen);

			buflen = put64((scan - lenb) - (lastscan + lenf), buf);
			owrite(&delta->control, buf, buflen);
			buflen = put64((pos - lenb) - (lastpos + lenf), buf);
			owrite(&delta->control, buf, buflen);
			lastscan = scan - lenb;
			lastpos = pos - lenb;
			lastoffset = pos - scan;
		}
	}
	free(I);
	return 1;
}

/* diff two files, putting results in patch file */
int
delta_diff_file(const char *oldfile, const char *newfile, const char *patchfile)
{
	delta_t	 delta;
	uint8_t         *oldv;
	uint8_t         *newv;
	off_t 		 oldsize;
	off_t 		 newsize;
	int		 ok;

	if (oldfile == NULL || newfile == NULL || patchfile == NULL) {
		return 0;
	}
	memset(&delta, 0x0, sizeof(delta));
	if (!readfile(oldfile, &oldv, &oldsize) ||
	    !readfile(newfile, &newv, &newsize)) {
		return 0;
	}
	if (!delta_diff_mem(&delta, oldv, oldsize, newv, newsize)) {
		return 0;
	}
	ok = delta_write_patch_file(&delta, patchfile);
	if (!ok) {
		return 0;
	}
	/* Free the memory we used */
	delta_free(&delta);
	free(oldv);
	free(newv);
	return 1;
}

/* patch using a binary patch */
int
delta_patch_mem(delta_t *delta, const void *aold, size_t oldsize, void *anewv, size_t *newsize)
{
	const uint8_t	 *old = (const uint8_t *)aold;
	uint8_t		**newv = (uint8_t **)anewv;
	size_t		  extrac;
	size_t		  diffc;
	size_t		  ctlc;
	size_t		  len;
	off_t 		  ctrl[3];
	off_t 		  oldpos;
	off_t 		  newpos;
	off_t		  i;

	if (delta == NULL || old == NULL || newv == NULL) {
		return 0;
	}
	if ((*newv = malloc(delta->newsize + 1)) == NULL) {
		warn("allocation failure %zu bytes", delta->newsize + 1);
		return 0;
	}
	for (oldpos = newpos = 0, ctlc = diffc = extrac = 0; newpos < delta->newsize ; ) {
		/* Read control data */
		for (i = 0; i < 3; i++) {
			ctrl[i] = get64(&delta->control.v[ctlc], &len);
			ctlc += len;
		}
		/* sanity check for negative offsets */
		if (ctrl[0] < 0 || ctrl[1] < 0) {
			warnx("negative offset, found corrupt patch");
			return 0;
		}
		/* Sanity-check */
		if (newpos + ctrl[0] > delta->newsize) {
			warnx("Corrupt patch 1");
			return 0;
		}
		/* Read diff string */
		memcpy(&(*newv)[newpos], &delta->diff[diffc], ctrl[0]);
		diffc += ctrl[0];
		/* Add old data to diff string */
		for (i = 0; i < ctrl[0]; i++) {
			if ((oldpos + i >= 0) && (oldpos + i < (off_t)oldsize)) {
				(*newv)[newpos + i] += old[oldpos + i];
			}
		}
		/* Adjust pointers */
		newpos += ctrl[0];
		oldpos += ctrl[0];
		/* Sanity-check */
		if (newpos + ctrl[1] > delta->newsize) {
			warnx("Corrupt patch 2");
			return 0;
		}
		/* Read extra string */
		memcpy(*newv + newpos, &delta->extra[extrac], ctrl[1]);
		extrac += ctrl[1];
		/* Adjust pointers */
		newpos += ctrl[1];
		oldpos += ctrl[2];
	}
	*newsize = delta->newsize;
	return 1;
}

/* patch using a binary patch */
int
delta_patch_file(const char *oldfile, const char *newfile, const char *patchfile)
{
	delta_t	 delta;
	ssize_t 	 oldsize;
	uint8_t         *newv;
	uint8_t         *old;
	size_t 	 	 newsize;

	if (oldfile == NULL || newfile == NULL || patchfile == NULL) {
		return 0;
	}
	memset(&delta, 0x0, sizeof(delta));
	if (!delta_read_patch_file(&delta, patchfile)) {
		return 0;
	}
	if (!readfile(oldfile, &old, &oldsize)) {
		warn("%s", oldfile);
		return 0;
	}
	if (!delta_patch_mem(&delta, old, oldsize, &newv, &newsize)) {
		return 0;
	}
	if (!writefile(newfile, newv, delta.newsize)) {
		warn("%s", newfile);
		return 0;
	}
	delta_free(&delta);
	return 1;
}

/* free resources associated with delta struct */
void
delta_free(delta_t *delta)
{
	if (delta) {
		if (delta->allocctl) {
			free(delta->control.v);
		}
		if (delta->allocdiff) {
			free(delta->diff);
		}
		if (delta->allocextra) {
			free(delta->extra);
		}
	}
}

/* make a new struct */
delta_t *
delta_new(void)
{
	return calloc(1, sizeof(delta_t));
}
