lib_LTLIBRARIES = libzsync.la

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA =libzsync.pc

include_HEADERS = \
    ../include/zsync.h \
    ../include/zs_msg.h \
    ../include/zsync_msg.h \
    ../include/zsync_credit_msg.h \
    ../include/zsync_ftm_msg.h \
    ../include/zs_fmetadata.h \
    ../include/zsync_peer.h \
    ../include/zsync_ftmanager.h \
    ../include/zsync_credit.h \
    ../include/zsync_node.h \
    ../include/zsync_agent.h

libzsync_la_SOURCES = \
    zs_msg.c \
    zsync_msg.c \
    zsync_credit_msg.c \
    zsync_ftm_msg.c \
    zs_fmetadata.c \
    zsync_peer.c \
    zsync_ftmanager.c \
    zsync_credit.c \
    zsync_node.c \
    zsync_agent.c

AM_CFLAGS = -g
AM_CPPFLAGS = -I$(top_srcdir)/include    

bin_PROGRAMS = zsync_selftest

zsync_selftest_LDADD = libzsync.la
zsync_selftest_SOURCES = zsync_selftest.c

TESTS = zsync_selftest

generate-tags:
	ctags *.c ~/workspace/zeromq/zyre/src/*.c ~/workspace/zeromq/czmq/src/*.c

# Generate gsl code
XML_SOURCE_CODE = \
	zsync_msg.xml  \
	zsync_credit_msg.xml \
	zsync_ftm_msg.xml

CODE_DEPENDENCY = $(XML_SOURCE_CODE:%.xml=%)

code: $(CODE_DEPENDENCY)
	echo Generated codec!

# Make executes each line of a recipe in a seperate shell.
# Hence we need to export and generate in one line.
$(CODE_DEPENDENCY):
	export PATH=@bindir@:$$PATH; gsl -q $@;

