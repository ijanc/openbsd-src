#	$OpenBSD$

PROG=	hammer2
SRCS=	cmd_bulkfree.c cmd_cleanup.c cmd_debug.c cmd_destroy.c \
	cmd_emergency.c cmd_growfs.c cmd_info.c cmd_pfs.c cmd_recover.c \
	cmd_scrub.c cmd_setcheck.c cmd_setcomp.c cmd_snapshot.c cmd_stat.c \
	cmd_volume.c hammer2_compression.c main.c ondisk.c print_inode.c \
	subs.c
SRCS+=	hammer2_lz4.c xxhash.c icrc32.c
MAN=	hammer2.8

CPPFLAGS+=	-I${.CURDIR}/../../sys -DZLIB_CONST
.PATH:	${.CURDIR}/../../sys/hammer2 ${.CURDIR}/../../sys/hammer2/xxhash \
	${.CURDIR}/../../sys/lib/libkern

DPADD=	${LIBZ}
LDADD=	-lz

.include <bsd.prog.mk>
