/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 Murilo Ijanc <murilo@ijanc.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * HAMMER2 scrub: walk the on-disk topology and verify the per-block check
 * codes (CRC32 / xxhash64 / SHA192) of every reachable block.  Read-only:
 * never modifies media.
 *
 * Internal blocks (INODE / INDIRECT / VOLUME) get verified for free as the
 * walker descends -- they are loaded with HAMMER2_RESOLVE_ALWAYS and
 * hammer2_chain_load_data() invokes the existing testcheck path, recording
 * HAMMER2_ERROR_CHECK on the chain when the data does not match.
 *
 * Data leaves (BREF_TYPE_DATA) are not instantiated as chains by
 * hammer2_chain_scan(), so for those we read the raw block via
 * hammer2_io_bread() and call hammer2_check_bref() directly.
 *
 * The walker is structurally a stripped-down clone of hammer2_bulkfree_scan:
 * same depth-first descent with the same deferred-recursion list to bound
 * stack and memory usage on deep trees, without bulkfree's freemap
 * accumulation, dedup heuristic, or live-topology bitmap.
 */

#include "hammer2.h"

typedef struct hammer2_scrub_save {
	TAILQ_ENTRY(hammer2_scrub_save)	entry;
	hammer2_chain_t			*chain;
} hammer2_scrub_save_t;

TAILQ_HEAD(hammer2_scrub_save_list, hammer2_scrub_save);
typedef struct hammer2_scrub_save_list hammer2_scrub_save_list_t;

typedef struct hammer2_scrub_info {
	hammer2_dev_t			*hmp;
	int				depth;
	int				verbose;
	int				list_alert;
	int				pri;
	long				blocks_scanned;
	long				inodes_scanned;
	long				dirents_scanned;
	long				bytes_scanned;
	long				errors_found;
	long				chains_reported;
	hammer2_scrub_save_list_t	list;
	long				list_count;
	hammer2_scrub_save_t		*backout;
} hammer2_scrub_info_t;

/*
 * Read a leaf bref's media block and verify its check code without going
 * through the chain machinery.  Returns 1 on success (or HAMMER2_CHECK_NONE),
 * 0 on mismatch or I/O error.  Sets *bytesp to the size scanned on success.
 */
static int
scrub_leaf(hammer2_dev_t *hmp, const hammer2_blockref_t *bref, int *bytesp)
{
	hammer2_io_t *dio = NULL;
	void *bdata;
	int radix, bytes, err, r = 0;

	radix = (int)(bref->data_off & HAMMER2_OFF_MASK_RADIX);
	if (radix == 0) {
		*bytesp = 0;
		return (1);
	}
	bytes = 1U << radix;
	*bytesp = bytes;

	err = hammer2_io_bread(hmp, bref->type, bref->data_off, bytes, &dio);
	if (err) {
		hammer2_io_bqrelse(&dio);
		return (0);
	}
	bdata = hammer2_io_data(dio, bref->data_off);
	r = hammer2_check_bref(bref, bdata, bytes);
	hammer2_io_bqrelse(&dio);
	return (r);
}

/*
 * Walk the chain tree under `parent`, verifying check codes on every
 * reachable block.  Mirrors hammer2_bulkfree_scan in shape but uses
 * RESOLVE_ALWAYS on chain locks and reads leaf blocks directly.
 */
static int
hammer2_scrub_scan(hammer2_chain_t *parent, hammer2_scrub_info_t *info)
{
	hammer2_blockref_t bref;
	hammer2_chain_t *chain;
	hammer2_scrub_save_t *tail, *save;
	int error, rup_error, savepri, first = 1;
	int leaf_ok, leaf_bytes;

	++info->pri;

	chain = NULL;
	rup_error = 0;
	error = 0;

	hammer2_chain_lock(parent, HAMMER2_RESOLVE_ALWAYS |
	    HAMMER2_RESOLVE_SHARED);

	tail = TAILQ_FIRST(&info->list);

	if (parent->error & HAMMER2_ERROR_CHECK) {
		++info->errors_found;
		if (info->verbose)
			hprintf("scrub: %s parent at %016llx: check mismatch\n",
			    hammer2_breftype_to_str(parent->bref.type),
			    (long long)parent->bref.data_off);
		error = parent->error;
		goto done;
	}

	if (parent->bref.type == HAMMER2_BREF_TYPE_INODE &&
	    (parent->bref.flags & HAMMER2_BREF_FLAG_PFSROOT))
		hprintf("scrub: scanning %s\n", parent->data->ipdata.filename);

	for (;;) {
		/*
		 * Same iteration flags as bulkfree.  Leaf data blocks are
		 * not instantiated as chains by chain_scan; we read them
		 * ourselves below via hammer2_io_bread() and check via
		 * hammer2_check_bref().
		 */
		error |= hammer2_chain_scan(parent, &chain, &bref, &first,
		    HAMMER2_LOOKUP_NODATA | HAMMER2_LOOKUP_SHARED);

		if (error & ~HAMMER2_ERROR_CHECK)
			break;

		if (bref.type == HAMMER2_BREF_TYPE_DIRENT)
			++info->dirents_scanned;

		/* Brefs without a media reference (most dirents). */
		if ((bref.data_off & ~HAMMER2_OFF_MASK_RADIX) == 0)
			continue;

		++info->pri;

		switch (bref.type) {
		case HAMMER2_BREF_TYPE_DATA:
			leaf_ok = scrub_leaf(info->hmp, &bref, &leaf_bytes);
			++info->blocks_scanned;
			info->bytes_scanned += leaf_bytes;
			if (!leaf_ok) {
				++info->errors_found;
				if (info->verbose)
					hprintf("scrub: data leaf at "
					    "%016llx (key %016llx, %d bytes): "
					    "check mismatch\n",
					    (long long)bref.data_off,
					    (long long)bref.key, leaf_bytes);
			}
			break;
		case HAMMER2_BREF_TYPE_INODE:
			++info->inodes_scanned;
			++info->blocks_scanned;
			if (chain) {
				info->bytes_scanned += chain->bytes;
				if (chain->error & HAMMER2_ERROR_CHECK) {
					++info->errors_found;
					if (info->verbose)
						hprintf("scrub: inode at "
						    "%016llx: check "
						    "mismatch\n",
						    (long long)
						    bref.data_off);
				}
			}
			break;
		case HAMMER2_BREF_TYPE_INDIRECT:
		case HAMMER2_BREF_TYPE_VOLUME:
			++info->blocks_scanned;
			if (chain) {
				info->bytes_scanned += chain->bytes;
				if (chain->error & HAMMER2_ERROR_CHECK) {
					++info->errors_found;
					if (info->verbose)
						hprintf("scrub: %s at "
						    "%016llx: check "
						    "mismatch\n",
						    hammer2_breftype_to_str(
						    bref.type),
						    (long long)
						    bref.data_off);
				}
			}
			break;
		case HAMMER2_BREF_TYPE_DIRENT:
			/* Embedded in parent; nothing more to read. */
			break;
		default:
			/*
			 * Freemap nodes and other types we deliberately do
			 * not descend into for scrub v1.  They are verified
			 * incidentally if the freemap path reads them.
			 */
			break;
		}

		/*
		 * Recurse depth-first into recursable internal types.  Defer
		 * via the save list when too deep or when the in-flight
		 * chain set is too large (same policy as bulkfree).
		 */
		if (chain == NULL)
			continue;

		switch (chain->bref.type) {
		case HAMMER2_BREF_TYPE_INODE:
		case HAMMER2_BREF_TYPE_INDIRECT:
		case HAMMER2_BREF_TYPE_VOLUME:
			++info->depth;
			if (chain->error & HAMMER2_ERROR_CHECK) {
				/* Counted above; cannot safely recurse. */
			} else if (info->depth > 16 ||
			    (info->depth > hammer2_limit_scan_depth &&
			    info->list_count >=
			    (hammer2_limit_saved_chains >> 2))) {
				if (info->list_count >
				    hammer2_limit_saved_chains &&
				    info->list_alert == 0) {
					hprintf("scrub: saved chains exceeded "
					    "%d at depth %d, deferring\n",
					    hammer2_limit_saved_chains,
					    info->depth);
					info->list_alert = 1;
				}
				save = hmalloc(sizeof(*save), M_HAMMER2,
				    M_WAITOK | M_ZERO);
				save->chain = chain;
				hammer2_chain_ref(chain);

				if (info->backout)
					TAILQ_INSERT_AFTER(&info->list,
					    info->backout, save, entry);
				else
					TAILQ_INSERT_HEAD(&info->list, save,
					    entry);
				info->backout = save;
				++info->list_count;
				info->pri += 10;
			} else {
				savepri = info->pri;
				hammer2_chain_unlock(chain);
				hammer2_chain_unlock(parent);
				info->pri = 0;
				rup_error |= hammer2_scrub_scan(chain, info);
				info->pri += savepri;
				hammer2_chain_lock(parent,
				    HAMMER2_RESOLVE_ALWAYS |
				    HAMMER2_RESOLVE_SHARED);
				hammer2_chain_lock(chain,
				    HAMMER2_RESOLVE_ALWAYS |
				    HAMMER2_RESOLVE_SHARED);
			}
			--info->depth;
			break;
		default:
			break;
		}
		if (rup_error & HAMMER2_ERROR_ABORTED)
			break;

		if (info->blocks_scanned >= info->chains_reported + 1000000 ||
		    (info->blocks_scanned < 1000000 &&
		    info->blocks_scanned >= info->chains_reported + 100000)) {
			hprintf("scrub: blocks %-7ld inodes %-7ld errors %ld\n",
			    info->blocks_scanned, info->inodes_scanned,
			    info->errors_found);
			info->chains_reported = info->blocks_scanned;
		}
	}
	if (chain) {
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
	}

	/*
	 * Drain anything we deferred while inside this PFSROOT so per-PFS
	 * accounting is accurate.
	 */
	if (parent->bref.type == HAMMER2_BREF_TYPE_INODE &&
	    (parent->bref.flags & HAMMER2_BREF_FLAG_PFSROOT)) {
		for (;;) {
			save = TAILQ_FIRST(&info->list);
			if (save == tail)
				break;
			TAILQ_REMOVE(&info->list, save, entry);
			info->backout = NULL;
			--info->list_count;
			savepri = info->pri;
			info->pri = 0;
			rup_error |= hammer2_scrub_scan(save->chain, info);
			hammer2_chain_drop(save->chain);
			hfree(save, M_HAMMER2, sizeof(*save));
			info->pri = savepri;
		}
	}

	error |= rup_error;

done:
	hammer2_chain_unlock(parent);
	return (error & ~HAMMER2_ERROR_EOF);
}

int
hammer2_scrub_pass(hammer2_dev_t *hmp, hammer2_chain_t *vchain,
    hammer2_ioc_scrub_t *si)
{
	hammer2_scrub_info_t info;
	hammer2_scrub_save_t *save;
	int error;

	bzero(&info, sizeof(info));
	info.hmp = hmp;
	info.verbose = (si->flags & HAMMER2_SCRUB_F_VERBOSE) ? 1 : 0;
	TAILQ_INIT(&info.list);

	error = hammer2_scrub_scan(vchain, &info);

	while ((save = TAILQ_FIRST(&info.list)) != NULL &&
	    (error & ~HAMMER2_ERROR_CHECK) == 0) {
		TAILQ_REMOVE(&info.list, save, entry);
		--info.list_count;
		info.pri = 0;
		info.backout = NULL;
		error |= hammer2_scrub_scan(save->chain, &info);
		hammer2_chain_drop(save->chain);
		hfree(save, M_HAMMER2, sizeof(*save));
	}

	while ((save = TAILQ_FIRST(&info.list)) != NULL) {
		TAILQ_REMOVE(&info.list, save, entry);
		--info.list_count;
		hammer2_chain_drop(save->chain);
		hfree(save, M_HAMMER2, sizeof(*save));
	}

	si->blocks_scanned = info.blocks_scanned;
	si->inodes_scanned = info.inodes_scanned;
	si->bytes_scanned = info.bytes_scanned;
	si->errors_found = info.errors_found;

	hprintf("scrub: done: %ld blocks, %ld inodes, %ld errors\n",
	    info.blocks_scanned, info.inodes_scanned, info.errors_found);

	return (error & ~HAMMER2_ERROR_CHECK);
}
