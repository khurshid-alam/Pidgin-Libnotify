gddir = $(LIBPURPLE_LIBDIR)/purple-2

pidgin_libnotify_la_LDFLAGS = -module -avoid-version

if PLUGINS

gd_LTLIBRARIES = pidgin-libnotify.la

pidgin_libnotify_la_SOURCES = \
	pidgin-libnotify.c \
	common.h \ 
	common.c \
	request.c \
	gln_intl.h

pidgin_libnotify_la_LIBADD = $(LIBMESSAGINGMENU_LIBS) $(LIBNOTIFY_LIBS) $(DBUS_LIBS) $(GTK_LIBS)

endif

AM_CPPFLAGS = \
	-DLIBDIR=\"$(LIBPURPLE_LIBDIR)/purple-2/\" \
	-DDATADIR=\"$(LIBPURPLE_DATADIR)\" \
	-DLOCALEDIR=\"$(LIBPURPLE_DATADIR)/locale\" \
	$(DEBUG_CFLAGS) \
	$(GTK_CFLAGS) \
	$(GLIB_CFLAGS) \
	$(PIDGIN_CFLAGS) \
	$(LIBPURPLE_CFLAGS) \
	$(LIBNOTIFY_CFLAGS) \
	$(LIBMESSAGINGMENU_CFLAGS) \
	$(DBUS_CFLAGS) \
	$(GTK_CFLAGS)

