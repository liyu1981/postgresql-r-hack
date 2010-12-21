# $PostgreSQL$

subdir = config
top_builddir = ..
include $(top_builddir)/src/Makefile.global


install: all installdirs
	cp $(srcdir)/*.sh "$(DESTDIR)$(prefix)/runscript"

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(prefix)/runscript'

uninstall:
	rm -f '$(DESTDIR)$(prefix)/runscript'
