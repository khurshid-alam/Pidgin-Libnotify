/*
 * Pidgin-libnotify - Provides a libnotify interface for Pidgin
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <debug.h>
#include <gtkutils.h>
#include <request.h>

#include <string.h>

#include "common.h"

typedef struct {
	int id;
	PurpleRequestActionCb callback;
} ActionData;

static void
action_cb (NotifyNotification *notification, char *action,
		   ActionData *data)
{
	data->callback(g_object_get_data(G_OBJECT(notification), "user_data"),
				   data->id);
}

GSList *req_notifications = NULL;

static gboolean
closed_cb (NotifyNotification *notification)
{
	req_notifications = g_slist_remove(req_notifications, notification);
	g_object_unref(G_OBJECT(notification));

	return FALSE;
}

static gchar *
remove_accelerators (const gchar *s)
{
	gchar **split = g_strsplit(s, "_", -1);
	gchar *result = g_strjoinv("", split);
	g_strfreev(split);
	return result;
}

static PurpleRequestUiOps ops;
static PurpleRequestUiOps *original_ops = NULL;

static void *
notify_request_action_with_icon(const char *title, const char *primary,
								const char *secondary, int default_action,
								PurpleAccount *account, const char *who,
								PurpleConversation *conv,
								gconstpointer icon_data, gsize icon_size,
								void *user_data,
								size_t action_count, va_list actions)
{
	NotifyNotification *notification;
	int i;

	if (!purple_prefs_get_bool("/plugins/gtk/libnotify/replace_requests") ||
		!server_has_caps("actions")) {
		return original_ops->request_action_with_icon(title, primary, secondary,
													  default_action, account,
													  who, conv, icon_data,
													  icon_size, user_data,
													  action_count, actions);
	}

	notification = notification_new(primary, secondary);

	if (icon_data) {
		GdkPixbuf *pixbuf = pidgin_pixbuf_from_data(icon_data, icon_size);
		if (pixbuf) {
			notify_notification_set_icon_from_pixbuf(notification, pixbuf);
			g_object_unref (pixbuf);
		} else {
			purple_debug_warning(PLUGIN_ID, "notify_request_action_with_icon(), "
								 "failed to parse request icon\n");
		}
	}

	notify_notification_set_hint(notification, "resident",
								 g_variant_new_boolean(TRUE));
	notify_notification_set_timeout(notification, NOTIFY_EXPIRES_NEVER);

	g_signal_connect(notification, "closed", G_CALLBACK(closed_cb), NULL);

	for (i = 0; i != action_count; ++i) {
		char *label = remove_accelerators(va_arg (actions, const char *));
		ActionData *data = g_new(ActionData, 1);
		data->id = i;
		data->callback = va_arg(actions, PurpleRequestActionCb);

		notify_notification_add_action(notification,
									   (default_action == i) ? "default" : label,
									   label,
									   (NotifyActionCallback) action_cb,
									   data, g_free);
		g_free(label);
	}

	g_object_set_data(G_OBJECT(notification), "user_data", user_data);

	req_notifications = g_slist_append(req_notifications, notification);

	notification_show(notification);

	return notification;
}

static void *
notify_request_action(const char *title, const char *primary,
					  const char *secondary, int default_action,
					  PurpleAccount *account, const char *who,
					  PurpleConversation *conv, void *user_data,
					  size_t action_count, va_list actions)
{
	if (!purple_prefs_get_bool("/plugins/gtk/libnotify/replace_requests") ||
		!server_has_caps("actions")) {
		return original_ops->request_action(title, primary, secondary,
											default_action, account, who, conv,
											user_data, action_count, actions);
	}

	return notify_request_action_with_icon(title, primary, secondary,
									   	   default_action, account, who, conv,
									   	   NULL, 0, user_data, action_count,
									   	   actions);
}

static void
notify_close_request(PurpleRequestType type, void *ui_handle)
{
	GSList *l = g_slist_find(req_notifications, ui_handle);
	if (l) {
		NotifyNotification *notification = l->data;
		notify_notification_close(notification, NULL);
	} else {
		original_ops->close_request(type, ui_handle);
	}
}

void
attach_request_ui_ops()
{
	original_ops = purple_request_get_ui_ops();
	if (original_ops) {
		memcpy(&ops, original_ops, sizeof (ops));
		ops.request_action = notify_request_action;
		ops.request_action_with_icon = notify_request_action_with_icon;
		ops.close_request = notify_close_request;
		purple_request_set_ui_ops(&ops);
	}
}

void
detach_request_ui_ops()
{
	if (original_ops) {
		purple_request_set_ui_ops(original_ops);
	}
}
