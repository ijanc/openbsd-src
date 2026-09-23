#	$OpenBSD$

PROG=	mount_hammer2
SRCS=	mount_hammer2.c getmntopts.c
MAN=	mount_hammer2.8

MOUNT=	${.CURDIR}/../mount
CPPFLAGS+=	-I${MOUNT}
.PATH:	${MOUNT}

.include <bsd.prog.mk>
