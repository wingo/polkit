
NULL =

AM_CPPFLAGS =                                              	\
	-I$(top_builddir)/src                           	\
	-I$(top_srcdir)/src                             	\
	-DPACKAGE_LIBEXEC_DIR=\""$(libexecdir)"\"       	\
	-DPACKAGE_SYSCONF_DIR=\""$(sysconfdir)"\"       	\
	-DPACKAGE_DATA_DIR=\""$(datadir)"\"             	\
	-DPACKAGE_BIN_DIR=\""$(bindir)"\"               	\
	-DPACKAGE_LOCALSTATE_DIR=\""$(localstatedir)"\" 	\
	-DPACKAGE_LOCALE_DIR=\""$(localedir)"\"         	\
	-DPACKAGE_LIB_DIR=\""$(libdir)"\"               	\
	-D_POSIX_PTHREAD_SEMANTICS                      	\
	-D_REENTRANT	                                	\
	$(NULL)

# ----------------------------------------------------------------------------------------------------

bin_PROGRAMS = pkexec pkcheck pkaction pkttyagent

# ----------------------------------------------------------------------------------------------------

pkexec_SOURCES = pkexec.c

pkexec_CFLAGS =                             				\
	$(SUID_CFLAGS)							\
	$(GLIB_CFLAGS)							\
	$(AUTH_LIBS)							\
	$(NULL)

pkexec_LDADD =  	                      				\
	$(GLIB_LIBS)							\
	$(top_builddir)/src/polkit/libpolkit-gobject-1.la		\
	$(top_builddir)/src/polkitagent/libpolkit-agent-1.la		\
	$(NULL)

pkexec_LDFLAGS =  	                      				\
	$(SUID_LDFLAGS)							\
	$(AM_LDFLAGS)							\
	$(NULL)

# ----------------------------------------------------------------------------------------------------

pkcheck_SOURCES = pkcheck.c

pkcheck_CFLAGS =                             				\
	$(GLIB_CFLAGS)							\
	$(NULL)

pkcheck_LDADD =  	                      				\
	$(GLIB_LIBS)							\
	$(top_builddir)/src/polkit/libpolkit-gobject-1.la		\
	$(top_builddir)/src/polkitagent/libpolkit-agent-1.la		\
	$(NULL)

# ----------------------------------------------------------------------------------------------------

pkttyagent_SOURCES = pkttyagent.c

pkttyagent_CFLAGS =                             			\
	$(GLIB_CFLAGS)							\
	$(NULL)

pkttyagent_LDADD =  	                      				\
	$(GLIB_LIBS)							\
	$(top_builddir)/src/polkit/libpolkit-gobject-1.la		\
	$(top_builddir)/src/polkitagent/libpolkit-agent-1.la		\
	$(NULL)

# ----------------------------------------------------------------------------------------------------

pkaction_SOURCES = pkaction.c

pkaction_CFLAGS =                             				\
	$(GLIB_CFLAGS)							\
	$(NULL)

pkaction_LDADD =  	                      				\
	$(GLIB_LIBS)							\
	$(top_builddir)/src/polkit/libpolkit-gobject-1.la		\
	$(NULL)

# ----------------------------------------------------------------------------------------------------

clean-local :
	rm -f *~

install-exec-hook :
	-chmod 4755 $(DESTDIR)$(bindir)/pkexec

-include $(top_srcdir)/git.mk
