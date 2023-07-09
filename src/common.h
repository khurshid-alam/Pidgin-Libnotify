/*
 * Pidgin-libnotify - Provides a libnotify interface for Pidgin
 * Copyright (C) 2005-2007 Duarte Henriques
 * Copyright (C) 2011 Jakub Adam
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifndef PIDGIN_LIBNOTIFY_COMMON_H
#define PIDGIN_LIBNOTIFY_COMMON_H

#include <libnotify/notify.h>

#define PLUGIN_ID "pidgin-libnotify"

#ifndef NOTIFY_CHECK_VERSION
#define NOTIFY_CHECK_VERSION(x,y,z) 0
#endif

gboolean server_has_caps(const char *id);

NotifyNotification * notification_new(const gchar *title, const gchar *body);

void notification_show(NotifyNotification *notification);

#endif // PIDGIN_LIBNOTIFY_COMMON_H
