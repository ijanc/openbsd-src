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

#include "hammer2.h"

int
cmd_scrub(const char *sel_path)
{
	hammer2_ioc_scrub_t si;
	int fd, res, ecode = 0;

	bzero(&si, sizeof(si));
	if (VerboseOpt > 0)
		si.flags |= HAMMER2_SCRUB_F_VERBOSE;

	if ((fd = hammer2_ioctl_handle(sel_path)) < 0)
		return (1);
	res = ioctl(fd, HAMMER2IOC_SCRUB, &si);
	if (res) {
		perror("ioctl");
		ecode = 1;
		goto out;
	}

	printf("scrub: %llu blocks scanned, %llu inodes, %llu bytes, "
	    "%llu errors\n",
	    (unsigned long long)si.blocks_scanned,
	    (unsigned long long)si.inodes_scanned,
	    (unsigned long long)si.bytes_scanned,
	    (unsigned long long)si.errors_found);

	if (si.errors_found != 0)
		ecode = 2;

out:
	close(fd);
	return (ecode);
}
