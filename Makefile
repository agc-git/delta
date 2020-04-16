#	$NetBSD: Makefile,v 1.1 2016/04/28 05:21:31 agc Exp $

SUBDIR=		lib .WAIT
SUBDIR+=	bin

.include <bsd.subdir.mk>

t:
	cd bin && ${MAKE} t
