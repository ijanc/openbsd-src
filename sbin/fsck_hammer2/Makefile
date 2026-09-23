#	$OpenBSD$

PROG=	fsck_hammer2
SRCS=	fsck_hammer2.c test.c ondisk.c subs.c xxhash.c icrc32.c
MAN=	fsck_hammer2.8

CPPFLAGS+=	-I${.CURDIR}/../hammer2 -I${.CURDIR}/../../sys
.PATH:	${.CURDIR}/../hammer2 ${.CURDIR}/../../sys/hammer2/xxhash \
	${.CURDIR}/../../sys/lib/libkern

.include <bsd.prog.mk>
