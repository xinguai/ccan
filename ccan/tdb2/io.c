 /* 
   Unix SMB/CIFS implementation.

   trivial database library

   Copyright (C) Andrew Tridgell              1999-2005
   Copyright (C) Paul `Rusty' Russell		   2000
   Copyright (C) Jeremy Allison			   2000-2003
   Copyright (C) Rusty Russell			   2010

     ** NOTE! The following LGPL license applies to the tdb
     ** library. This does NOT imply that all of Samba is released
     ** under the LGPL

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 3 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/
#include "private.h"
#include <assert.h>
#include <ccan/likely/likely.h>

void tdb_munmap(struct tdb_context *tdb)
{
	if (tdb->flags & TDB_INTERNAL)
		return;

	if (tdb->map_ptr) {
		munmap(tdb->map_ptr, tdb->map_size);
		tdb->map_ptr = NULL;
	}
}

void tdb_mmap(struct tdb_context *tdb)
{
	if (tdb->flags & TDB_INTERNAL)
		return;

	if (tdb->flags & TDB_NOMMAP)
		return;

	tdb->map_ptr = mmap(NULL, tdb->map_size, tdb->mmap_flags,
			    MAP_SHARED, tdb->fd, 0);

	/*
	 * NB. When mmap fails it returns MAP_FAILED *NOT* NULL !!!!
	 */
	if (tdb->map_ptr == MAP_FAILED) {
		tdb->map_ptr = NULL;
		tdb->log(tdb, TDB_DEBUG_WARNING, tdb->log_priv,
			 "tdb_mmap failed for size %lld (%s)\n", 
			 (long long)tdb->map_size, strerror(errno));
	}
}

/* check for an out of bounds access - if it is out of bounds then
   see if the database has been expanded by someone else and expand
   if necessary 
   note that "len" is the minimum length needed for the db
*/
static int tdb_oob(struct tdb_context *tdb, tdb_off_t len, bool probe)
{
	struct stat st;
	int ret;

	/* We can't hold pointers during this: we could unmap! */
	assert(!tdb->direct_access
	       || (tdb->flags & TDB_NOLOCK)
	       || tdb_has_expansion_lock(tdb));

	if (len <= tdb->map_size)
		return 0;
	if (tdb->flags & TDB_INTERNAL) {
		if (!probe) {
			/* Ensure ecode is set for log fn. */
			tdb->ecode = TDB_ERR_IO;
			tdb->log(tdb, TDB_DEBUG_FATAL, tdb->log_priv,
				 "tdb_oob len %lld beyond internal"
				 " malloc size %lld\n",
				 (long long)len,
				 (long long)tdb->map_size);
		}
		return -1;
	}

	if (tdb_lock_expand(tdb, F_RDLCK) != 0)
		return -1;

	ret = fstat(tdb->fd, &st);

	tdb_unlock_expand(tdb, F_RDLCK);

	if (ret == -1) {
		tdb->ecode = TDB_ERR_IO;
		return -1;
	}

	if (st.st_size < (size_t)len) {
		if (!probe) {
			/* Ensure ecode is set for log fn. */
			tdb->ecode = TDB_ERR_IO;
			tdb->log(tdb, TDB_DEBUG_FATAL, tdb->log_priv,
				 "tdb_oob len %lld beyond eof at %lld\n",
				 (long long)len, (long long)st.st_size);
		}
		return -1;
	}

	/* Unmap, update size, remap */
	tdb_munmap(tdb);

	tdb->map_size = st.st_size;
	tdb_mmap(tdb);
	return 0;
}

/* Endian conversion: we only ever deal with 8 byte quantities */
void *tdb_convert(const struct tdb_context *tdb, void *buf, tdb_len_t size)
{
	if (unlikely((tdb->flags & TDB_CONVERT)) && buf) {
		uint64_t i, *p = (uint64_t *)buf;
		for (i = 0; i < size / 8; i++)
			p[i] = bswap_64(p[i]);
	}
	return buf;
}

/* FIXME: Return the off? */
uint64_t tdb_find_nonzero_off(struct tdb_context *tdb,
			      tdb_off_t base, uint64_t start, uint64_t end)
{
	uint64_t i;
	const uint64_t *val;

	/* Zero vs non-zero is the same unconverted: minor optimization. */
	val = tdb_access_read(tdb, base + start * sizeof(tdb_off_t),
			      (end - start) * sizeof(tdb_off_t), false);
	if (!val)
		return end;

	for (i = 0; i < (end - start); i++) {
		if (val[i])
			break;
	}
	tdb_access_release(tdb, val);
	return start + i;
}

/* Return first zero offset in num offset array, or num. */
uint64_t tdb_find_zero_off(struct tdb_context *tdb, tdb_off_t off,
			   uint64_t num)
{
	uint64_t i;
	const uint64_t *val;

	/* Zero vs non-zero is the same unconverted: minor optimization. */
	val = tdb_access_read(tdb, off, num * sizeof(tdb_off_t), false);
	if (!val)
		return num;

	for (i = 0; i < num; i++) {
		if (!val[i])
			break;
	}
	tdb_access_release(tdb, val);
	return i;
}

int zero_out(struct tdb_context *tdb, tdb_off_t off, tdb_len_t len)
{
	char buf[8192] = { 0 };
	void *p = tdb->methods->direct(tdb, off, len);

	if (tdb->read_only) {
		tdb->ecode = TDB_ERR_RDONLY;
		return -1;
	}

	if (p) {
		memset(p, 0, len);
		return 0;
	}
	while (len) {
		unsigned todo = len < sizeof(buf) ? len : sizeof(buf);
		if (tdb->methods->write(tdb, off, buf, todo) == -1)
			return -1;
		len -= todo;
		off += todo;
	}
	return 0;
}

tdb_off_t tdb_read_off(struct tdb_context *tdb, tdb_off_t off)
{
	tdb_off_t ret;

	if (likely(!(tdb->flags & TDB_CONVERT))) {
		tdb_off_t *p = tdb->methods->direct(tdb, off, sizeof(*p));
		if (p)
			return *p;
	}

	if (tdb_read_convert(tdb, off, &ret, sizeof(ret)) == -1)
		return TDB_OFF_ERR;
	return ret;
}

/* Even on files, we can get partial writes due to signals. */
bool tdb_pwrite_all(int fd, const void *buf, size_t len, tdb_off_t off)
{
	while (len) {
		ssize_t ret;
		ret = pwrite(fd, buf, len, off);
		if (ret < 0)
			return false;
		if (ret == 0) {
			errno = ENOSPC;
			return false;
		}
		buf = (char *)buf + ret;
		off += ret;
		len -= ret;
	}
	return true;
}

/* Even on files, we can get partial reads due to signals. */
bool tdb_pread_all(int fd, void *buf, size_t len, tdb_off_t off)
{
	while (len) {
		ssize_t ret;
		ret = pread(fd, buf, len, off);
		if (ret < 0)
			return false;
		if (ret == 0) {
			/* ETOOSHORT? */
			errno = EWOULDBLOCK;
			return false;
		}
		buf = (char *)buf + ret;
		off += ret;
		len -= ret;
	}
	return true;
}

bool tdb_read_all(int fd, void *buf, size_t len)
{
	while (len) {
		ssize_t ret;
		ret = read(fd, buf, len);
		if (ret < 0)
			return false;
		if (ret == 0) {
			/* ETOOSHORT? */
			errno = EWOULDBLOCK;
			return false;
		}
		buf = (char *)buf + ret;
		len -= ret;
	}
	return true;
}

/* write a lump of data at a specified offset */
static int tdb_write(struct tdb_context *tdb, tdb_off_t off, 
		     const void *buf, tdb_len_t len)
{
	if (len == 0) {
		return 0;
	}

	if (tdb->read_only) {
		tdb->ecode = TDB_ERR_RDONLY;
		return -1;
	}

	if (tdb->methods->oob(tdb, off + len, 0) != 0)
		return -1;

	if (tdb->map_ptr) {
		memcpy(off + (char *)tdb->map_ptr, buf, len);
	} else {
		if (!tdb_pwrite_all(tdb->fd, buf, len, off)) {
			tdb->ecode = TDB_ERR_IO;
			tdb->log(tdb, TDB_DEBUG_FATAL, tdb->log_priv,
				 "tdb_write failed at %llu len=%llu (%s)\n",
				 (long long)off, (long long)len,
				 strerror(errno));
			return -1;
		}
	}
	return 0;
}

/* read a lump of data at a specified offset */
static int tdb_read(struct tdb_context *tdb, tdb_off_t off, void *buf,
		    tdb_len_t len)
{
	if (tdb->methods->oob(tdb, off + len, 0) != 0) {
		return -1;
	}

	if (tdb->map_ptr) {
		memcpy(buf, off + (char *)tdb->map_ptr, len);
	} else {
		if (!tdb_pread_all(tdb->fd, buf, len, off)) {
			/* Ensure ecode is set for log fn. */
			tdb->ecode = TDB_ERR_IO;
			tdb->log(tdb, TDB_DEBUG_FATAL, tdb->log_priv,
				 "tdb_read failed at %lld "
				 "len=%lld (%s) map_size=%lld\n",
				 (long long)off, (long long)len,
				 strerror(errno),
				 (long long)tdb->map_size);
			return -1;
		}
	}
	return 0;
}

int tdb_write_convert(struct tdb_context *tdb, tdb_off_t off,
		      const void *rec, size_t len)
{
	int ret;
	if (unlikely((tdb->flags & TDB_CONVERT))) {
		void *conv = malloc(len);
		if (!conv) {
			tdb->ecode = TDB_ERR_OOM;
			tdb->log(tdb, TDB_DEBUG_FATAL, tdb->log_priv,
				 "tdb_write: no memory converting %zu bytes\n",
				 len);
			return -1;
		}
		memcpy(conv, rec, len);
		ret = tdb->methods->write(tdb, off,
					  tdb_convert(tdb, conv, len), len);
		free(conv);
	} else
		ret = tdb->methods->write(tdb, off, rec, len);

	return ret;
}

int tdb_read_convert(struct tdb_context *tdb, tdb_off_t off,
		      void *rec, size_t len)
{
	int ret = tdb->methods->read(tdb, off, rec, len);
	tdb_convert(tdb, rec, len);
	return ret;
}

int tdb_write_off(struct tdb_context *tdb, tdb_off_t off, tdb_off_t val)
{
	if (tdb->read_only) {
		tdb->ecode = TDB_ERR_RDONLY;
		return -1;
	}

	if (likely(!(tdb->flags & TDB_CONVERT))) {
		tdb_off_t *p = tdb->methods->direct(tdb, off, sizeof(*p));
		if (p) {
			*p = val;
			return 0;
		}
	}
	return tdb_write_convert(tdb, off, &val, sizeof(val));
}

static void *_tdb_alloc_read(struct tdb_context *tdb, tdb_off_t offset,
			     tdb_len_t len, unsigned int prefix)
{
	void *buf;

	/* some systems don't like zero length malloc */
	buf = malloc(prefix + len ? prefix + len : 1);
	if (unlikely(!buf)) {
		tdb->ecode = TDB_ERR_OOM;
		tdb->log(tdb, TDB_DEBUG_ERROR, tdb->log_priv,
			 "tdb_alloc_read malloc failed len=%lld\n",
			 (long long)prefix + len);
	} else if (unlikely(tdb->methods->read(tdb, offset, buf+prefix, len))) {
		free(buf);
		buf = NULL;
	}
	return buf;
}

/* read a lump of data, allocating the space for it */
void *tdb_alloc_read(struct tdb_context *tdb, tdb_off_t offset, tdb_len_t len)
{
	return _tdb_alloc_read(tdb, offset, len, 0);
}

static int fill(struct tdb_context *tdb,
		const void *buf, size_t size,
		tdb_off_t off, tdb_len_t len)
{
	while (len) {
		size_t n = len > size ? size : len;

		if (!tdb_pwrite_all(tdb->fd, buf, n, off)) {
			tdb->ecode = TDB_ERR_IO;
			tdb->log(tdb, TDB_DEBUG_FATAL, tdb->log_priv,
				 "fill write failed: giving up!\n");
			return -1;
		}
		len -= n;
		off += n;
	}
	return 0;
}

/* expand a file.  we prefer to use ftruncate, as that is what posix
  says to use for mmap expansion */
static int tdb_expand_file(struct tdb_context *tdb, tdb_len_t addition)
{
	char buf[8192];

	if (tdb->read_only) {
		tdb->ecode = TDB_ERR_RDONLY;
		return -1;
	}

	if (tdb->flags & TDB_INTERNAL) {
		char *new = realloc(tdb->map_ptr, tdb->map_size + addition);
		if (!new) {
			tdb->ecode = TDB_ERR_OOM;
			return -1;
		}
		tdb->map_ptr = new;
		tdb->map_size += addition;
	} else {
		/* Unmap before trying to write; old TDB claimed OpenBSD had
		 * problem with this otherwise. */
		tdb_munmap(tdb);

		/* If this fails, we try to fill anyway. */
		if (ftruncate(tdb->fd, tdb->map_size + addition))
			;

		/* now fill the file with something. This ensures that the
		   file isn't sparse, which would be very bad if we ran out of
		   disk. This must be done with write, not via mmap */
		memset(buf, 0x43, sizeof(buf));
		if (0 || fill(tdb, buf, sizeof(buf), tdb->map_size, addition) == -1)
			return -1;
		tdb->map_size += addition;
		tdb_mmap(tdb);
	}
	return 0;
}

/* This is only neded for tdb_access_commit, but used everywhere to simplify. */
struct tdb_access_hdr {
	tdb_off_t off;
	tdb_len_t len;
	bool convert;
};

const void *tdb_access_read(struct tdb_context *tdb,
			    tdb_off_t off, tdb_len_t len, bool convert)
{
	const void *ret = NULL;	

	if (likely(!(tdb->flags & TDB_CONVERT)))
		ret = tdb->methods->direct(tdb, off, len);

	if (!ret) {
		struct tdb_access_hdr *hdr;
		hdr = _tdb_alloc_read(tdb, off, len, sizeof(*hdr));
		if (hdr) {
			ret = hdr + 1;
			if (convert)
				tdb_convert(tdb, (void *)ret, len);
		}
	} else
		tdb->direct_access++;

	return ret;
}

void *tdb_access_write(struct tdb_context *tdb,
		       tdb_off_t off, tdb_len_t len, bool convert)
{
	void *ret = NULL;

	if (tdb->read_only) {
		tdb->ecode = TDB_ERR_RDONLY;
		return NULL;
	}

	if (likely(!(tdb->flags & TDB_CONVERT)))
		ret = tdb->methods->direct(tdb, off, len);

	if (!ret) {
		struct tdb_access_hdr *hdr;
		hdr = _tdb_alloc_read(tdb, off, len, sizeof(*hdr));
		if (hdr) {
			hdr->off = off;
			hdr->len = len;
			hdr->convert = convert;
			ret = hdr + 1;
			if (convert)
				tdb_convert(tdb, (void *)ret, len);
		}
	} else
		tdb->direct_access++;

	return ret;
}

bool is_direct(const struct tdb_context *tdb, const void *p)
{
	return (tdb->map_ptr
		&& (char *)p >= (char *)tdb->map_ptr
		&& (char *)p < (char *)tdb->map_ptr + tdb->map_size);
}

void tdb_access_release(struct tdb_context *tdb, const void *p)
{
	if (is_direct(tdb, p))
		tdb->direct_access--;
	else
		free((struct tdb_access_hdr *)p - 1);
}

int tdb_access_commit(struct tdb_context *tdb, void *p)
{
	int ret = 0;

	if (!tdb->map_ptr
	    || (char *)p < (char *)tdb->map_ptr
	    || (char *)p >= (char *)tdb->map_ptr + tdb->map_size) {
		struct tdb_access_hdr *hdr;

		hdr = (struct tdb_access_hdr *)p - 1;
		if (hdr->convert)
			ret = tdb_write_convert(tdb, hdr->off, p, hdr->len);
		else
			ret = tdb_write(tdb, hdr->off, p, hdr->len);
		free(hdr);
	} else
		tdb->direct_access--;

	return ret;
}

static void *tdb_direct(struct tdb_context *tdb, tdb_off_t off, size_t len)
{
	if (unlikely(!tdb->map_ptr))
		return NULL;

	if (unlikely(tdb_oob(tdb, off + len, true) == -1))
		return NULL;
	return (char *)tdb->map_ptr + off;
}

void add_stat_(struct tdb_context *tdb, uint64_t *stat, size_t val)
{
	if ((uintptr_t)stat < (uintptr_t)tdb->stats + tdb->stats->size)
		*stat += val;
}

static const struct tdb_methods io_methods = {
	tdb_read,
	tdb_write,
	tdb_oob,
	tdb_expand_file,
	tdb_direct,
};

/*
  initialise the default methods table
*/
void tdb_io_init(struct tdb_context *tdb)
{
	tdb->methods = &io_methods;
}
