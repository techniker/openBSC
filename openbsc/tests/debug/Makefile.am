INCLUDES = $(all_includes) -I$(top_srcdir)/include
AM_CFLAGS=-Wall $(LIBOSMOCORE_CFLAGS)
noinst_PROGRAMS = debug_test

debug_test_SOURCES = debug_test.c $(top_srcdir)/src/debug.c 
debug_test_LDADD = $(LIBOSMOCORE_LIBS)
