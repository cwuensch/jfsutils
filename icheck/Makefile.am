
INCLUDES = -I$(top_srcdir)/include -I$(top_srcdir)/libfs
LDADD = ../libfs/libfs.a -luuid

sbin_PROGRAMS = jfs_icheck
man_MANS = jfs_icheck.8

EXTRA_DIST = $(man_MANS)

jfs_icheck_SOURCES = jfs_icheck.c lib.c
