INCLUDES = $(all_includes) -I$(top_srcdir)/include
AM_CFLAGS=-Wall -ggdb3 $(LIBOSMOCORE_CFLAGS)

noinst_PROGRAMS = db_test

db_test_SOURCES = db_test.c
db_test_LDADD = $(top_builddir)/src/libbsc.a $(top_builddir)/src/libmsc.a $(top_builddir)/src/libbsc.a $(LIBOSMOCORE_LIBS) -ldl -ldbi

