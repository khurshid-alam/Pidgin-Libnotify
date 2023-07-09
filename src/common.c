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

#include "common.h"

#include <debug.h>
#include <request.h>

gboolean
server_has_caps(const char *id)
{
	GList *caps = notify_get_server_caps();
	gboolean result;
	result = (g_list_find_custom(caps, id, (GCompareFunc)g_strcmp0) != NULL);
	g_list_free_full(caps, g_free);
	return result;
}

typedef struct {
	int id;
	PurpleRequestActionCb callback;
} ActionData;

NotifyNotification *
notification_new(const gchar *title, const gchar *body)
{
	NotifyNotification *notification;

/* libnotify 0.7.0 and later has no support for attaching to widgets */
#if NOTIFY_CHECK_VERSION(0,7,0)
	notification = notify_notification_new(title, body, NULL);
#else
	notification = notify_notification_new(title, body, NULL, NULL);
#endif
	purple_debug_info(PLUGIN_ID, "notification_new(), title: '%s', body: '%s'",
					  title, body);

	notify_notification_set_urgency(notification, NOTIFY_URGENCY_NORMAL);

	return notification;
}

void
notification_show(NotifyNotification *notification)
{
	if (!notify_notification_show (notification, NULL)) {
		purple_debug_error (PLUGIN_ID, "notification_show(), "
						    "failed to send notification\n");
	}
}
