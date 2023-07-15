/*
 * Pidgin-libnotify - Provides a libnotify interface for Pidgin
 * Copyright (C) 2005-2007 Duarte Henriques
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

#include "common.h"
#include "gln_intl.h"

#include <purple.h>

#ifndef PURPLE_PLUGINS
#define PURPLE_PLUGINS
#endif

#include <pidgin.h>
#include <version.h>
#include <debug.h>
#include <util.h>
#include <privacy.h>

/* for pidgin_create_prpl_icon */
#include <gtkutils.h>
#include <gtkblist.h>

#include <libnotify/notify.h>
#include <messaging-menu.h>

#include <string.h>

#define PLUGIN_ID "pidgin-libnotify"

#ifdef G_LOG_DOMAIN
#undef G_LOG_DOMAIN
#endif
#define G_LOG_DOMAIN "pidgin-libnotify-plugin"

#define PIDGIN_DESKTOP_FILE  "pidgin.desktop"
#define BLACKLIST_FILENAME   "pidgin-libnotify"
#define BLACKLIST_DIR        "indicators/messages/applications-blacklist"

/* Prototypes */
static void notify_new_message_cb (PurpleAccount *account, const gchar *sender, const gchar *message, int flags, gpointer data);
static gboolean pidgin_conversation_has_focus (PurpleConversation *);

/* Globals */
static GHashTable *buddy_hash;

static MessagingMenuApp *m_menu_app = NULL;

static gboolean  notify_supports_actions = FALSE;
static gboolean  notify_supports_append = FALSE;
static gboolean  notify_supports_truncation = FALSE;

static guint never_loaded = 0;

static void conv_delete_cb (PurpleConversation * conv, void * data);

static PurplePluginPrefFrame *
get_plugin_pref_frame (PurplePlugin *plugin)
{
	PurplePluginPrefFrame *frame;
	PurplePluginPref *ppref;

	frame = purple_plugin_pref_frame_new ();

	ppref = purple_plugin_pref_new_with_name_and_label (
                            "/plugins/gtk/libnotify/newmsg",
                            _("New messages"));
	purple_plugin_pref_frame_add (frame, ppref);

	ppref = purple_plugin_pref_new_with_name_and_label (
                            "/plugins/gtk/libnotify/newconvonly",
                            _("Only new conversations"));
	purple_plugin_pref_frame_add (frame, ppref);

	ppref = purple_plugin_pref_new_with_name_and_label (
                            "/plugins/gtk/libnotify/blocked",
                            _("Ignore events from blocked users"));
	purple_plugin_pref_frame_add (frame, ppref);

	ppref = purple_plugin_pref_new_with_name_and_label (
                            "/plugins/gtk/libnotify/signon",
                            _("Buddy signs on"));
	purple_plugin_pref_frame_add (frame, ppref);

	ppref = purple_plugin_pref_new_with_name_and_label (
                            "/plugins/gtk/libnotify/signoff",
                            _("Buddy signs off"));
	purple_plugin_pref_frame_add (frame, ppref);

	ppref = purple_plugin_pref_new_with_name_and_label (
                            "/plugins/gtk/libnotify/only_available",
                            _("Only when available"));
	purple_plugin_pref_frame_add (frame, ppref);

	ppref = purple_plugin_pref_new_with_name_and_label (
                            "/plugins/gtk/libnotify/replace_requests",
                            _("Replace Pidgin request dialogs with libnotify popups"));
	purple_plugin_pref_frame_add (frame, ppref);

	ppref = purple_plugin_pref_new_with_name_and_label (
                            "/plugins/gtk/libnotify/blocked_nicks",
                            _("Names to remove notifications for"));
	purple_plugin_pref_frame_add (frame, ppref);

	return frame;
}

/* Signon flood be gone! - thanks to the guifications devs */
static GList *just_signed_on_accounts = NULL;

static gboolean
event_connection_throttle_cb (gpointer data)
{
	PurpleAccount *account;

	account = (PurpleAccount *)data;

	if (!account)
		return FALSE;

	if (!purple_account_get_connection (account)) {
		just_signed_on_accounts = g_list_remove (just_signed_on_accounts, account);
		return FALSE;
	}

	if (!purple_account_is_connected (account))
		return TRUE;

	just_signed_on_accounts = g_list_remove (just_signed_on_accounts, account);
	return FALSE;
}

static void
event_connection_throttle (PurpleConnection *conn, gpointer data)
{
	PurpleAccount *account;

	/* TODO: this function gets called after buddy signs on for GTalk
	   users who have themselves as a buddy */
	purple_debug_info (PLUGIN_ID, "event_connection_throttle() called\n");

	if (!conn)
		return;

	account = purple_connection_get_account(conn);
	if (!account)
		return;

	just_signed_on_accounts = g_list_prepend (just_signed_on_accounts, account);
	g_timeout_add_seconds (15, event_connection_throttle_cb, (gpointer)account);

	return;
}

/* do NOT g_free() the string returned by this function */
const static gchar *
best_name (PurpleBuddy *buddy)
{
  return purple_buddy_get_contact_alias(buddy);
}

static GdkPixbuf *
pixbuf_from_buddy_icon (PurpleBuddyIcon *buddy_icon)
{
	GdkPixbuf *icon;
	const guchar *data;
	size_t len;
	GdkPixbufLoader *loader;

	data = purple_buddy_icon_get_data (buddy_icon, &len);

	loader = gdk_pixbuf_loader_new ();
	gdk_pixbuf_loader_write (loader, data, len, NULL);
	gdk_pixbuf_loader_close (loader, NULL);

	icon = gdk_pixbuf_loader_get_pixbuf (loader);

	if (icon) {
		g_object_ref (icon);
	}

	g_object_unref (loader);

	return icon;
}

/*
 * This takes a pixbuf that we want to send to the notify server, and
 * transforms it to the desired dimensions suitable for a notification.
 * We scale the pixbuf down to size * size (square), but preserve the
 * original aspect ratio and fill in the edges with transparent pixels
 * if the original pixbuf was not square.
 */
static GdkPixbuf *
normalize_icon (GdkPixbuf *icon, gint size)
{
  gint w, h;
  int dest_x, dest_y;
  gdouble max_edge;
  gint new_width, new_height;
  GdkPixbuf *scaled_icon;
  GdkPixbuf *new_icon;

  w = gdk_pixbuf_get_width (icon);
  h = gdk_pixbuf_get_height (icon);

  dest_x = dest_y = 0;

  max_edge = MAX (w, h);

  new_width = size * (w / max_edge);
  new_height = size * (h / max_edge);

  /* Scale the image down, preserving the aspect ratio */
  scaled_icon = gdk_pixbuf_scale_simple (icon,
					 new_width,
					 new_height,
					 GDK_INTERP_HYPER);

  g_object_unref (icon);

  /* Create a square pixbuf with an alpha channel, dimensions size * size */
  new_icon = gdk_pixbuf_new (gdk_pixbuf_get_colorspace (scaled_icon),
			     TRUE,
			     gdk_pixbuf_get_bits_per_sample (scaled_icon),
			     size, size);

  /* Clear the pixbuf so it is transparent */
  gdk_pixbuf_fill (new_icon, 0x00000000);

  /* Center the aspect ratio preseved pixbuf in the square pixbuf */
  if (new_width > new_height) {
    dest_y = (new_width - new_height) / 2;
  } else if (new_height > new_width) {
    dest_x = (new_height - new_width) / 2;
  }

  /* Copy from the aspect ratio-preserved scaled pixbuf into the
   * new pixbuf, at a centered position. */
  gdk_pixbuf_copy_area (scaled_icon,
			0, 0,
			gdk_pixbuf_get_width (scaled_icon),
			gdk_pixbuf_get_height (scaled_icon),
			new_icon,
			dest_x, dest_y);

  g_object_unref (scaled_icon);

  return new_icon;
}

/* Check the name against the static list of black listed
   names that we're looking out for.  These shouldn't result
   in either notifications or indicators. */
static gboolean
name_blacklisted (PurpleAccount * account, const gchar * name)
{
	if (account != NULL) {
		const gchar * username = purple_account_get_username(account);
		gchar ** userparts = g_strsplit(username, "@", 2);
		if (g_strcmp0(name, userparts[1]) == 0) {
			g_strfreev(userparts);
			return TRUE;
		}
		g_strfreev(userparts);
	}

	GList * blacklist = purple_prefs_get_string_list("/plugins/gtk/libnotify/blocked_nicks");
	GList * pnt;
	for (pnt = blacklist; pnt != NULL; pnt = g_list_next(pnt)) {
		if (g_strcmp0(name, (gchar *)pnt->data) == 0) {
			return TRUE;
		}
	}
	return FALSE;
}

static void
action_cb (NotifyNotification *notification,
		   gchar *action, gpointer user_data)
{
	PurpleBuddy *buddy = NULL;
	PurpleConversation *conv = NULL;

	purple_debug_info (PLUGIN_ID, "action_cb(), "
					"notification: 0x%x, action: '%s'", GPOINTER_TO_UINT(notification), action);

	buddy = (PurpleBuddy *)user_data;

	if (!buddy) {
		purple_debug_warning (PLUGIN_ID, "Got no buddy!");
		return;
	}

	conv = purple_find_conversation_with_account (PURPLE_CONV_TYPE_ANY, buddy->name, buddy->account);

	if (!conv) {
		conv = purple_conversation_new (PURPLE_CONV_TYPE_IM,
									  buddy->account,
									  buddy->name);
	}
	conv->ui_ops->present (conv);

	notify_notification_close (notification, NULL);
}

static gboolean
notification_list_closed_cb (NotifyNotification *notification, PurpleConversation * conv)
{
	purple_debug_info (PLUGIN_ID, "closed_cb(), notification: 0x%x\n", GPOINTER_TO_UINT(notification));

	if (conv != NULL) {
		GList * notifylist = purple_conversation_get_data(conv, "notification-list");
		notifylist = g_list_remove(notifylist, notification);
		purple_conversation_set_data(conv, "notification-list", notifylist);
	}
	g_object_unref(notification);

	return FALSE;
}

static gboolean
closed_cb (NotifyNotification *notification, PurpleContact * contact)
{
	purple_debug_info (PLUGIN_ID, "closed_cb(), notification: 0x%x\n", GPOINTER_TO_UINT(notification));

	if (contact)
		g_hash_table_remove (buddy_hash, contact);

	g_object_unref (G_OBJECT(notification));

	return FALSE;
}

/* you must g_free the returned string
 * num_chars is utf-8 characters */
static gchar *
truncate_escape_string (const gchar *str,
			int num_chars,
			gboolean escape)
{
	gchar *escaped_str;

	if (!notify_supports_truncation && g_utf8_strlen (str, num_chars*2+1) > num_chars) {
		gchar *truncated_str;
		gchar *str2;

		/* allocate number of bytes and not number of utf-8 chars */
		str2 = g_malloc ((num_chars-1) * 2 * sizeof(gchar));

		g_utf8_strncpy (str2, str, num_chars-2);
		truncated_str = g_strdup_printf ("%s..", str2);
		escaped_str = escape ? g_markup_escape_text (truncated_str, strlen (truncated_str)) : g_strdup (truncated_str);
		g_free (str2);
		g_free (truncated_str);
	} else {
		escaped_str = escape ? g_markup_escape_text (str, strlen (str)) : g_strdup (str);
	}

	return escaped_str;
}

static gboolean
should_notify_unavailable (PurpleAccount *account)
{
	PurpleStatus *status;

	if (!purple_prefs_get_bool ("/plugins/gtk/libnotify/only_available"))
		return TRUE;

	status = purple_account_get_active_status (account);

	return purple_status_is_online (status) && purple_status_is_available (status);
}

static void
notify (const gchar *title,
		const gchar *body,
		PurpleBuddy *buddy,
		PurpleConversation *conv)
{
	NotifyNotification *notification = NULL;
	GdkPixbuf *icon = NULL;
	PurpleBuddyIcon *buddy_icon = NULL;
	gchar *tr_body = NULL;
	PurpleContact *contact = NULL;

	if (buddy != NULL) {
		contact = purple_buddy_get_contact (buddy);
	}

	if (body)
		tr_body = truncate_escape_string (body, 60, TRUE);
	else
		tr_body = NULL;

	/* If we're appending we shouldn't update an already
	   existing notification */
	if (conv == NULL && contact != NULL) {
		notification = g_hash_table_lookup (buddy_hash, contact);
	}

	/* This will only happen if we're a login message */
	if (notification != NULL) {
		notify_notification_update (notification, title, tr_body, NULL);

		/* this shouldn't be necessary, file a bug */
		notify_notification_show (notification, NULL);

		purple_debug_info (PLUGIN_ID, "notify(), update: "
						 "title: '%s', body: '%s', buddy: '%s'\n",
						 title, tr_body, buddy != NULL ? best_name (buddy) : "(null)");

		g_free (tr_body);
		return;
	}

	notification = notify_notification_new (title, tr_body, "notification-message-im");
	purple_debug_info (PLUGIN_ID, "notify(), new: "
					 "title: '%s', body: '%s', buddy: '%s'\n",
					 title, tr_body, buddy != NULL ? best_name (buddy) : "(null)");

	g_free (tr_body);

	if (notify_supports_append) {
		if (conv != NULL) {
			notify_notification_set_hint_string(notification, "x-canonical-append", "allow");
		}
	}

	if (buddy != NULL) {
		buddy_icon = purple_buddy_get_icon (buddy);
	}

	if (buddy_icon != NULL) {
		icon = pixbuf_from_buddy_icon (buddy_icon);
		purple_debug_info (PLUGIN_ID, "notify(), has a buddy icon.\n");
	} else {
		if (buddy != NULL) {
			icon = pidgin_create_prpl_icon (buddy->account, PIDGIN_PRPL_ICON_LARGE);
			purple_debug_info (PLUGIN_ID, "notify(), has a prpl icon.\n");
		}
	}

	icon = normalize_icon (icon, 48);

	if (icon != NULL) {
		notify_notification_set_icon_from_pixbuf (notification, icon);
		g_object_unref (icon);

		GValue iconname = {0};
		g_value_init(&iconname, G_TYPE_STRING);
		g_value_set_static_string(&iconname, "");
		g_object_set_property(G_OBJECT(notification), "icon-name", &iconname);
	}

	if (contact != NULL && conv == NULL) {
		g_hash_table_insert (buddy_hash, contact, notification);

		g_signal_connect (notification, "closed", G_CALLBACK(closed_cb), contact);
	}
	if (conv != NULL) {
		GList * notifylist = purple_conversation_get_data(conv, "notification-list");
		notifylist = g_list_append(notifylist, notification);
		purple_conversation_set_data(conv, "notification-list", notifylist);
		g_signal_connect(notification, "closed", G_CALLBACK(notification_list_closed_cb), conv);
	}
	if (contact == NULL && conv == NULL) {
		/* Should never happen, but just in case, let's not have a memory leak */
		g_signal_connect(notification, "closed", G_CALLBACK(g_object_unref), NULL);
	}

	notify_notification_set_urgency (notification, NOTIFY_URGENCY_NORMAL);

	if (notify_supports_actions) {
		notify_notification_add_action (notification, "show", _("Show"), action_cb, buddy, NULL);
	}

	if (!notify_notification_show (notification, NULL)) {
		purple_debug_error (PLUGIN_ID, "notify(), failed to send notification\n");
	}

}

static void
notify_buddy_signon_cb (PurpleBuddy *buddy,
						gpointer data)
{
	gchar *tr_name;
	gboolean blocked;

	g_return_if_fail (buddy);

	if (!purple_prefs_get_bool ("/plugins/gtk/libnotify/signon"))
		return;

	if (g_list_find (just_signed_on_accounts, buddy->account))
		return;

	blocked = purple_prefs_get_bool ("/plugins/gtk/libnotify/blocked");
	if (!purple_privacy_check (buddy->account, buddy->name) && blocked)
		return;

	if (!should_notify_unavailable (purple_buddy_get_account (buddy)))
		return;

	tr_name = truncate_escape_string (best_name (buddy), 25, FALSE);

	notify (tr_name, _("is online"), buddy, NULL);

	g_free (tr_name);
}

static void
notify_buddy_signoff_cb (PurpleBuddy *buddy,
						 gpointer data)
{
	gchar *tr_name;
	gboolean blocked;

	g_return_if_fail (buddy);

	if (!purple_prefs_get_bool ("/plugins/gtk/libnotify/signoff"))
		return;

	if (g_list_find (just_signed_on_accounts, buddy->account))
		return;

	blocked = purple_prefs_get_bool ("/plugins/gtk/libnotify/blocked");
	if (!purple_privacy_check (buddy->account, buddy->name) && blocked)
		return;

	if (!should_notify_unavailable (purple_buddy_get_account (buddy)))
		return;

	tr_name = truncate_escape_string (best_name (buddy), 25, FALSE);

	notify (tr_name, _("is offline"), buddy, NULL);

	g_free (tr_name);
}

static void
notify_msg_sent (PurpleAccount *account,
				 const gchar *sender,
				 const gchar *message,
				 PurpleConversation * conv)
{
	PurpleBuddy *buddy = NULL;
	gchar *body = NULL, *tr_name = NULL;
	gboolean blocked;

	blocked = purple_prefs_get_bool ("/plugins/gtk/libnotify/blocked");
	if (!purple_privacy_check(account, sender) && blocked)
		return;

	if (g_list_find (just_signed_on_accounts, account))
		return;

	buddy = purple_find_buddy (account, sender);

	if (buddy != NULL) {
		tr_name = truncate_escape_string (best_name (buddy), 25, FALSE);
	} else {
		if (conv != NULL) {
			const gchar * temp = purple_conversation_get_title(conv);
			if (temp != NULL) {
				if (sender == NULL || !g_strcmp0(sender, temp)) {
					tr_name = g_strdup(temp);
				} else {
					tr_name = g_strdup_printf("%s (%s)", sender, temp);
				}
			} else {
				if (sender != NULL) {
					tr_name = g_strdup(sender);
				}
			}
		}
	}

	if (tr_name == NULL) {
		purple_debug_warning(PLUGIN_ID, "Unable to find a title for the notification");
		return;
	}

	body = purple_markup_strip_html (message);

	notify (tr_name, body, buddy, conv);

	g_free (tr_name);
	g_free (body);
}

static void
notify_new_message_cb (PurpleAccount *account,
					   const gchar *sender,
					   const gchar *message,
					   int flags,
					   gpointer data)
{
	PurpleConversation *conv;

	if (!purple_prefs_get_bool ("/plugins/gtk/libnotify/newmsg"))
		return;

	if (name_blacklisted(account, sender)) return;

	conv = purple_find_conversation_with_account (PURPLE_CONV_TYPE_IM, sender, account);

#ifndef DEBUG /* in debug mode, always show notifications */
	if (conv && purple_conversation_has_focus (conv)) {
		purple_debug_info (PLUGIN_ID, "Conversation has focus 0x%x\n", GPOINTER_TO_UINT(conv));
		return;
	}
#endif

	if (conv && purple_prefs_get_bool ("/plugins/gtk/libnotify/newconvonly")) {
		purple_debug_info (PLUGIN_ID, "Conversation is not new 0x%x\n", GPOINTER_TO_UINT(conv));
		return;
	}

	if (!should_notify_unavailable (account))
		return;

	if (pidgin_conversation_has_focus (conv)) {
		purple_debug_warning(PLUGIN_ID, "Pidgin conversation's widgets are in focus");
		return;
	}

	notify_msg_sent (account, sender, message, conv);
}

static void
notify_chat_nick (PurpleAccount *account,
				  const gchar *sender,
				  const gchar *message,
				  PurpleConversation *conv,
				  gpointer data)
{
	gchar *nick;

	nick = (gchar *)purple_conv_chat_get_nick (PURPLE_CONV_CHAT(conv));
	if (nick && !strcmp (sender, nick))
		return;

	if (!purple_utf8_has_word (message, nick))
		return;

	if (pidgin_conversation_has_focus (conv)) {
		purple_debug_warning(PLUGIN_ID, "Pidgin conversation's widgets are in focus");
		return;
	}

	if (name_blacklisted(account, sender)) return;

	notify_msg_sent (account, sender, message, conv);
}

static void
conv_delete_cb (PurpleConversation * conv, void * data)
{
	GList * notifylist = purple_conversation_get_data(conv, "notification-list");
	if (notifylist != NULL) {
		GList * i;
		for (i = notifylist; i != NULL; i = i->next) {
			NotifyNotification * notification = NOTIFY_NOTIFICATION(i->data);
			if (notification == NULL) break;

			g_signal_handlers_disconnect_by_func(G_OBJECT(notification), notification_list_closed_cb, conv);
			notify_notification_close(notification, NULL); /* Don't care if it fails, it's going to die. */
			g_object_unref(G_OBJECT(notification));
		}
		g_list_free(notifylist);

		purple_conversation_set_data(conv, "notification-list", NULL);
	}

	return;
}

static void
remove_from_blacklist (void)
{
	gchar *bpath;

	bpath = g_build_filename (g_get_user_config_dir(),
	                          BLACKLIST_DIR,
	                          BLACKLIST_FILENAME,
	                          NULL);

	if (g_file_test (bpath, G_FILE_TEST_EXISTS)) {
		GFile *bfile;
		bfile = g_file_new_for_path (bpath);

		if (bfile) {
			GError *error = NULL;
			g_file_delete (bfile, NULL, &error);

			if (error) {
				g_warning ("Unable to remove blacklist file: %s", error->message);
				g_error_free (error);
			}

			g_object_unref (bfile);
		}
	}

	g_free (bpath);

	return;
}

static gboolean
plugin_never_loaded (gpointer data)
{
	gchar  *bdir;
	gchar  *bpath;
	GError *error = NULL;

	bdir = g_build_filename (g_get_user_config_dir (),
	                         BLACKLIST_DIR,
	                         NULL);
	if (!g_file_test (bdir, G_FILE_TEST_IS_DIR)) {
		GFile *dirfile;

		dirfile = g_file_new_for_path (bdir);
		if (dirfile) {
			g_file_make_directory_with_parents (dirfile,
			                                    NULL,
			                                    &error);
			if (error) {
				g_warning ("Unable to create blacklist directory: %s",
				           error->message);
				g_error_free (error);
				g_object_unref (dirfile);
				g_free (bdir);
				return FALSE;
			}
		} else {
			g_warning ("Unable to create blacklist directory: Unable to create "
			           "GFile for path %s", bdir);
			g_free (bdir);
			return FALSE;
		}

		g_object_unref (dirfile);
	}
	g_free (bdir);

	bpath = g_build_filename (g_get_user_config_dir (),
	                          BLACKLIST_DIR,
	                          BLACKLIST_FILENAME,
	                          NULL);

	if (g_file_set_contents (bpath,
	                         PIDGIN_DESKTOP_FILE,
	                         -1,
	                         &error)) {
		g_debug ("Successfully wrote blacklist file to %s", bpath);
	} else {
		g_debug ("Unable to write blacklist file to %s: %s",
		         bpath,
		         error ? error->message : "Unknown");
		if (error)
			g_error_free (error);
	}

	g_free (bpath);

	return FALSE;
}

static gboolean
force_load_once (gpointer data)
{
	PurplePlugin * plugin = (PurplePlugin *)data;

	if (!purple_prefs_get_bool("/plugins/gtk/libnotify/auto_loaded")) {
		purple_plugin_load(plugin);
		purple_plugins_save_loaded(PIDGIN_PREFS_ROOT "/plugins/loaded");
		purple_prefs_set_bool("/plugins/gtk/libnotify/auto_loaded", TRUE);
	}

	return FALSE;
}

static void
notify_check_caps_helper (gpointer data, gpointer user_data)
{
	gchar * cap = (gchar *)data;

	if (cap == NULL) return;

	if (!strcmp(cap, "actions")) {
		notify_supports_actions = TRUE;
	} else if (!strcmp(cap, "append")) {
		notify_supports_append = TRUE;
	} else if (!strcmp(cap, "x-canonical-append")) {
		notify_supports_append = TRUE;
	} else if (!strcmp(cap, "truncation")) {
		notify_supports_truncation = TRUE;
	} else if (!strcmp(cap, "x-canonical-truncation")) {
		notify_supports_truncation = TRUE;
	}

	return;
}

static void
notify_check_caps(void)
{
	GList * caps = notify_get_server_caps();

	g_list_foreach(caps, notify_check_caps_helper, NULL);
	g_list_foreach(caps, (GFunc)g_free, NULL);
	g_list_free(caps);

	return;
}

extern void attach_request_ui_ops ();
extern void detach_request_ui_ops ();

static gchar *
messaging_unique_id (const char    *type,
					 PurpleAccount *account,
					 const char    *name)
{
	g_return_val_if_fail (type != NULL, NULL);
	g_return_val_if_fail (account != NULL, NULL);
	g_return_val_if_fail (name != NULL, NULL);

	gchar *escaped_type = g_uri_escape_string (type, NULL, TRUE);
	gchar *buddy_name = g_uri_escape_string (name, NULL, TRUE);
	gchar *account_name = g_uri_escape_string (purple_account_get_username (account), NULL, TRUE);
	gchar *protocol_id = g_uri_escape_string (purple_account_get_protocol_id (account), NULL, TRUE);
	gchar *unique_id = g_strconcat (escaped_type, ":", protocol_id, ":", account_name,
			":", buddy_name, NULL);

	g_free (escaped_type);
	g_free (buddy_name);
	g_free (account_name);
	g_free (protocol_id);

	return unique_id;
}

static PurpleConversation *
messaging_conversation_for_unique_id (const gchar *unique_id)
{
	g_return_val_if_fail (unique_id != NULL, NULL);

	gchar **tokens = g_strsplit(unique_id, ":", 0);
	guint n_tokens = g_strv_length(tokens);

	if (n_tokens != 4) {
		purple_debug_warning (PLUGIN_ID, "Invalid unique_id '%s'\n", unique_id);
		g_strfreev (tokens);
		return NULL;
	}

	gchar *type_id = g_uri_unescape_string(tokens[0], NULL);
	gchar *protocol_id = g_uri_unescape_string(tokens[1], NULL);
	gchar *account_name = g_uri_unescape_string(tokens[2], NULL);
	gchar *buddy_name = g_uri_unescape_string(tokens[3], NULL);

	PurpleAccount *account = purple_accounts_find (account_name, protocol_id);

	if (account == NULL) {
		g_free (type_id);
		g_free (protocol_id);
		g_free (account_name);
		g_free (buddy_name);
		g_strfreev (tokens);
		return NULL;
	}

	PurpleConversationType conv_type = PURPLE_CONV_TYPE_IM;
	if (g_strcmp0 (type_id, "chat") == 0)
		conv_type = PURPLE_CONV_TYPE_CHAT;

	PurpleConversation *conv = purple_find_conversation_with_account (conv_type,
			buddy_name, account);

	g_free (type_id);
	g_free (protocol_id);
	g_free (account_name);
	g_free (buddy_name);
	g_strfreev (tokens);

	return conv;
}

static const char *
messaging_type_from_conv_type (PurpleConversationType type)
{
	const char *messaging_type;

	switch (type) {
		case PURPLE_CONV_TYPE_IM:
			messaging_type = "im";
			break;
		case PURPLE_CONV_TYPE_CHAT:
			messaging_type = "chat";
			break;
		default:
			purple_debug_warning (PLUGIN_ID, "Unsupported conversation type\n");
			messaging_type = "unknown";
			break;
	}

	return messaging_type;
}

static const char *
messaging_conversation_get_name (PurpleConversation *conv,
								 PurpleAccount      *account)
{
	PurpleBuddy *buddy = NULL;

	if (purple_conversation_get_type (conv) == PURPLE_CONV_TYPE_IM)
		buddy = purple_find_buddy (account, purple_conversation_get_name (conv));
	if (buddy != NULL)
		return purple_buddy_get_name (buddy);
	return purple_conversation_get_name (conv);
}

static void
messaging_conversation_updated_cb (PurpleConversation  *conv,
								   PurpleConvUpdateType type,
								   gpointer 		    user_data)
{
	g_return_if_fail (m_menu_app != NULL);

	const char *type_name;
	const char *conv_type = messaging_type_from_conv_type (purple_conversation_get_type (conv));
	PurpleAccount *account;
	gchar *buddy_id;
	int unseen_count;

	switch (type) {
		case PURPLE_CONV_UPDATE_ADD:
			type_name = "ADD";
			break;
		case PURPLE_CONV_UPDATE_REMOVE:
			type_name = "REMOVE";
			break;
		case PURPLE_CONV_UPDATE_ACCOUNT:
			type_name = "ACCOUNT";
			break;
		case PURPLE_CONV_UPDATE_TYPING:
			type_name = "TYPING";
			break;
		case PURPLE_CONV_UPDATE_UNSEEN:
			type_name = "UNSEEN";
			break;
		case PURPLE_CONV_UPDATE_LOGGING:
			type_name = "LOGGING";
			break;
		case PURPLE_CONV_UPDATE_TOPIC:
			type_name = "TOPIC";
			break;
		default:
			type_name = "OTHER";
	}
	purple_debug_info (PLUGIN_ID, "Conversation Updated (%s)\n", type_name);

	/* Remove the attention */
	if (type == PURPLE_CONV_UPDATE_UNSEEN) {
		unseen_count = GPOINTER_TO_INT(purple_conversation_get_data (conv, "unseen-count"));
		account = purple_conversation_get_account (conv);
		buddy_id = messaging_unique_id (conv_type, account,
			messaging_conversation_get_name (conv, account));

		if (buddy_id != NULL) {
			if (messaging_menu_app_has_source (m_menu_app, buddy_id)) {
				if (unseen_count == 0) {
					purple_debug_info (PLUGIN_ID, "REMOVING ATTENTION FOR (%s) (%d)\n",
							buddy_id, unseen_count);
					messaging_menu_app_remove_attention (m_menu_app, buddy_id);
				}
				else if (purple_conversation_get_type (conv) == PURPLE_CONV_TYPE_IM) {
					messaging_menu_app_draw_attention (m_menu_app, buddy_id);
				}
			}
		}
		g_free (buddy_id);
	}
}

static void
messaging_conversation_deleted_cb (PurpleConversation *conv,
								   gpointer 		   user_data)
{
	g_return_if_fail (conv != NULL);

	PurpleAccount *account;
	const char *conv_type = messaging_type_from_conv_type (purple_conversation_get_type (conv));
	gchar *buddy_id;

	account = purple_conversation_get_account (conv);
	buddy_id = messaging_unique_id (conv_type, account,
		messaging_conversation_get_name (conv, account));

	if (buddy_id != NULL) {
		if (messaging_menu_app_has_source (m_menu_app, buddy_id)) {
			purple_debug_info (PLUGIN_ID, "Removing source (%s)\n", buddy_id);
			messaging_menu_app_remove_source (m_menu_app, buddy_id);
		}
	}

	g_free (buddy_id);
}

static gboolean
pidgin_conversation_has_focus (PurpleConversation *conv)
{
	if (conv == NULL)
		return FALSE;

	PidginConversation * pconv = PIDGIN_CONVERSATION(conv);

	if (pconv != NULL) {
		if (pconv->entry != NULL && pconv->imhtml != NULL) {
			if (GTK_WIDGET_HAS_FOCUS(pconv->entry) || GTK_WIDGET_HAS_FOCUS(pconv->imhtml)) {
				return TRUE;
			}
		}
	}

	return FALSE;
}

static void
messaging_new_message_cb (PurpleAccount *account,
					   	  const gchar   *sender,
					   	  const gchar   *message,
					   	  int 			 flags,
					   	  gpointer 		 user_data)
{
	g_return_if_fail (m_menu_app != NULL);

	PurpleBuddy *buddy = NULL;
	gchar *buddy_id;
	const char *buddy_name = NULL;

	if (name_blacklisted(account, sender))
		return;

	if (g_list_find (just_signed_on_accounts, account))
		return;

	if (account == NULL || sender == NULL)
		return;

	buddy = purple_find_buddy (account, sender);

	if (buddy != NULL)
		buddy_name = purple_buddy_get_name (buddy);

	buddy_id = messaging_unique_id ("im", account, buddy_name != NULL ? buddy_name : sender);

	if (buddy_id != NULL) {
		purple_debug_info (PLUGIN_ID, "Sender: (%s) ID (%s) \n", sender, buddy_id);
		if (messaging_menu_app_has_source (m_menu_app, buddy_id)) {
			messaging_menu_app_set_source_time (m_menu_app, buddy_id, g_get_real_time ());
		}
		else {
			messaging_menu_app_append_source (m_menu_app, buddy_id, NULL,
					buddy != NULL ? best_name (buddy) : sender);
		}
	}

	g_free (buddy_id);
}

static void
messaging_new_chat_cb (PurpleAccount      *account,
				  	   const gchar 		  *sender,
				  	   const gchar 		  *message,
				  	   PurpleConversation *conv,
				       gpointer 		   user_data)
{
	g_return_if_fail (m_menu_app != NULL);

	purple_debug_info (PLUGIN_ID, "Chat (%s) (%s)\n", sender, message);

	if (name_blacklisted(account, sender)) return;

	const char *nick = purple_conv_chat_get_nick (PURPLE_CONV_CHAT(conv));
	if (nick && !g_strcmp0 (sender, nick))
		return;

	if (!purple_utf8_has_word (message, nick))
		return;

	char *buddy_id = messaging_unique_id ("chat", account, purple_conversation_get_name (conv));

	if (buddy_id != NULL) {
		if (messaging_menu_app_has_source (m_menu_app, buddy_id)) {
			messaging_menu_app_set_source_time (m_menu_app, buddy_id, g_get_real_time ());
		}
		else {
			messaging_menu_app_append_source (m_menu_app, buddy_id, NULL,
					purple_conversation_get_title (conv));
		}
		if (!pidgin_conversation_has_focus (conv))
			messaging_menu_app_draw_attention (m_menu_app, buddy_id);
	}

	g_free (buddy_id);
}

static MessagingMenuStatus
purple_status_to_messaging_status (PurpleStatusPrimitive status)
{
	MessagingMenuStatus messaging_status_value;

	switch (status) {
	  	case PURPLE_STATUS_AVAILABLE:
			messaging_status_value = MESSAGING_MENU_STATUS_AVAILABLE;
			break;
		case PURPLE_STATUS_AWAY:
			messaging_status_value = MESSAGING_MENU_STATUS_AWAY;
			break;
		case PURPLE_STATUS_UNAVAILABLE:
			messaging_status_value = MESSAGING_MENU_STATUS_BUSY;
			break;
		case PURPLE_STATUS_INVISIBLE:
			messaging_status_value = MESSAGING_MENU_STATUS_INVISIBLE;
			break;
		case PURPLE_STATUS_OFFLINE:
			messaging_status_value = MESSAGING_MENU_STATUS_OFFLINE;
			break;
		default:
			messaging_status_value = MESSAGING_MENU_STATUS_AVAILABLE;
	}

	return messaging_status_value;
}

static void
messaging_set_status_from_savedstatus (PurpleSavedStatus *status)
{
	g_return_if_fail (m_menu_app != NULL);

	PurpleStatusPrimitive purple_status = purple_savedstatus_get_type (status);
	const char *status_id = purple_primitive_get_id_from_type (purple_status);
	MessagingMenuStatus messaging_status = purple_status_to_messaging_status (purple_status);
	messaging_menu_app_set_status (m_menu_app, messaging_status);
	purple_debug_info (PLUGIN_ID, "Updating status set from pidgin to '%s'\n", status_id);
}

static void
messaging_savedstatus_changed_cb (PurpleSavedStatus *now,
								  PurpleSavedStatus *old,
								  gpointer 			 user_data)
{
	messaging_set_status_from_savedstatus (now);
}

static PurpleStatusPrimitive
messaging_status_to_purple_status (MessagingMenuStatus status)
{
	PurpleStatusPrimitive purple_status_value;

	switch (status) {
	  	case MESSAGING_MENU_STATUS_AVAILABLE:
			purple_status_value = PURPLE_STATUS_AVAILABLE;
			break;
		case MESSAGING_MENU_STATUS_AWAY:
			purple_status_value = PURPLE_STATUS_AWAY;
			break;
		case MESSAGING_MENU_STATUS_BUSY:
			purple_status_value = PURPLE_STATUS_UNAVAILABLE;
			break;
		case MESSAGING_MENU_STATUS_INVISIBLE:
			purple_status_value = PURPLE_STATUS_INVISIBLE;
			break;
		case MESSAGING_MENU_STATUS_OFFLINE:
			purple_status_value = PURPLE_STATUS_OFFLINE;
			break;
		default:
			purple_status_value = PURPLE_STATUS_UNSET;
	}

	return purple_status_value;
}

static void
messaging_status_changed_cb (MessagingMenuApp   *app,
							 MessagingMenuStatus status,
							 gpointer            user_data)
{
	g_return_if_fail (app != NULL);

	PurpleStatusPrimitive status_type = messaging_status_to_purple_status (status);
	const char *status_id = purple_primitive_get_id_from_type (status_type);
	PurpleSavedStatus *saved_status;

	purple_debug_info (PLUGIN_ID, "Updating accounts to status '%s'\n", status_id);

	saved_status = purple_savedstatus_find_transient_by_type_and_message (status_type, NULL);

	if (saved_status == NULL) {
		saved_status = purple_savedstatus_new (NULL, status_type);
		purple_debug_info (PLUGIN_ID, "Status not found\n");
	}
	purple_savedstatus_activate (saved_status);
	messaging_menu_app_set_status (app, status);
}

static void
messaging_source_activated_cb (MessagingMenuApp *app,
							   const gchar      *source_id,
							   gpointer          user_data)
{
	PurpleConversation *conv;
	purple_debug_info (PLUGIN_ID, "SOURCE activated '%s'\n", source_id);
	/* find conversation with given source id */
	conv = messaging_conversation_for_unique_id (source_id);

	g_return_if_fail(conv != NULL);
	pidgin_conv_present_conversation(conv);
}

static gboolean
plugin_load (PurplePlugin *plugin)
{
	void *conv_handle, *blist_handle, *conn_handle, *savedstatus_handle;

	if (!notify_is_initted () && !notify_init ("Pidgin")) {
		purple_debug_error (PLUGIN_ID, "libnotify not running!\n");
		return FALSE;
	}

	/* They really do love me! */
	if (never_loaded != 0) {
		g_source_remove(never_loaded);
	}
	remove_from_blacklist();

	notify_check_caps();

	if (m_menu_app == NULL) {
		m_menu_app = messaging_menu_app_new (PIDGIN_DESKTOP_FILE);
		messaging_menu_app_register (m_menu_app);
		g_signal_connect(m_menu_app, "status-changed",
							G_CALLBACK(messaging_status_changed_cb), NULL);
		g_signal_connect(m_menu_app, "activate-source",
							G_CALLBACK(messaging_source_activated_cb), NULL);
		messaging_set_status_from_savedstatus (purple_savedstatus_get_current ());
		pidgin_blist_visibility_manager_add ();
	}

	conv_handle = purple_conversations_get_handle ();
	blist_handle = purple_blist_get_handle ();
	conn_handle = purple_connections_get_handle();
	savedstatus_handle = purple_savedstatuses_get_handle ();

	buddy_hash = g_hash_table_new (NULL, NULL);

	purple_signal_connect (blist_handle, "buddy-signed-on", plugin,
						PURPLE_CALLBACK(notify_buddy_signon_cb), NULL);

	purple_signal_connect (blist_handle, "buddy-signed-off", plugin,
						PURPLE_CALLBACK(notify_buddy_signoff_cb), NULL);

	purple_signal_connect (conv_handle, "received-im-msg", plugin,
						PURPLE_CALLBACK(notify_new_message_cb), NULL);

	purple_signal_connect (conv_handle, "received-chat-msg", plugin,
						PURPLE_CALLBACK(notify_chat_nick), NULL);

	purple_signal_connect (conv_handle, "deleting-conversation", plugin,
	                    PURPLE_CALLBACK(conv_delete_cb), NULL);

	purple_signal_connect (conv_handle, "conversation-updated", plugin,
						PURPLE_CALLBACK(messaging_conversation_updated_cb), NULL);

	purple_signal_connect (conv_handle, "deleting-conversation", plugin,
	                    PURPLE_CALLBACK(messaging_conversation_deleted_cb), NULL);

	purple_signal_connect (conv_handle, "received-im-msg", plugin,
						PURPLE_CALLBACK(messaging_new_message_cb), NULL);

	purple_signal_connect (conv_handle, "received-chat-msg", plugin,
						PURPLE_CALLBACK(messaging_new_chat_cb), NULL);

	purple_signal_connect (savedstatus_handle, "savedstatus-changed", plugin,
						PURPLE_CALLBACK(messaging_savedstatus_changed_cb), NULL);

	/* used just to not display the flood of guifications we'd get */
	purple_signal_connect (conn_handle, "signed-on", plugin,
						PURPLE_CALLBACK(event_connection_throttle), NULL);

	attach_request_ui_ops ();

	return TRUE;
}

static gboolean
plugin_unload (PurplePlugin *plugin)
{
	void *conv_handle, *blist_handle, *conn_handle, *savedstatus_handle;

	conv_handle = purple_conversations_get_handle ();
	blist_handle = purple_blist_get_handle ();
	conn_handle = purple_connections_get_handle();
	savedstatus_handle = purple_savedstatuses_get_handle ();

	purple_signal_disconnect (blist_handle, "buddy-signed-on", plugin,
							PURPLE_CALLBACK(notify_buddy_signon_cb));

	purple_signal_disconnect (blist_handle, "buddy-signed-off", plugin,
							PURPLE_CALLBACK(notify_buddy_signoff_cb));

	purple_signal_disconnect (conv_handle, "received-im-msg", plugin,
							PURPLE_CALLBACK(notify_new_message_cb));

	purple_signal_disconnect (conv_handle, "received-chat-msg", plugin,
							PURPLE_CALLBACK(notify_chat_nick));

	purple_signal_disconnect (conv_handle, "deleting-conversation", plugin,
							PURPLE_CALLBACK(conv_delete_cb));

	purple_signal_disconnect (conv_handle, "conversation-updated", plugin,
							PURPLE_CALLBACK(messaging_conversation_updated_cb));

	purple_signal_disconnect (conv_handle, "deleting-conversation", plugin,
							PURPLE_CALLBACK(messaging_conversation_deleted_cb));

	purple_signal_disconnect (conv_handle, "received-im-msg", plugin,
							PURPLE_CALLBACK(messaging_new_message_cb));

	purple_signal_disconnect (conv_handle, "received-chat-msg", plugin,
							PURPLE_CALLBACK(messaging_new_chat_cb));

	purple_signal_disconnect (savedstatus_handle, "savedstatus-changed", plugin,
							PURPLE_CALLBACK(messaging_savedstatus_changed_cb));

	purple_signal_disconnect (conn_handle, "signed-on", plugin,
							PURPLE_CALLBACK(event_connection_throttle));

	detach_request_ui_ops ();

	g_hash_table_destroy (buddy_hash);

	notify_uninit ();

	if (m_menu_app) {
		g_object_unref (m_menu_app);
		m_menu_app = NULL;
		pidgin_blist_visibility_manager_remove ();
	}

	/* If this goes off, we were unloaded by the user
	   and not by shutdown.  Same thing as us never
	   getting loaded at all. */
	never_loaded = g_timeout_add_seconds(30, plugin_never_loaded, NULL);

	return TRUE;
}

static PurplePluginUiInfo prefs_info = {
    get_plugin_pref_frame,
    0,						/* page num (Reserved) */
    NULL					/* frame (Reserved) */
};

static PurplePluginInfo info = {
    PURPLE_PLUGIN_MAGIC,										/* api version */
    PURPLE_MAJOR_VERSION,
    PURPLE_MINOR_VERSION,
    PURPLE_PLUGIN_STANDARD,									/* type */
    0,														/* ui requirement */
    0,														/* flags */
    NULL,													/* dependencies */
    PURPLE_PRIORITY_DEFAULT,									/* priority */
    
    PLUGIN_ID,												/* id */
    NULL,													/* name */
    VERSION,												/* version */
    NULL,													/* summary */
    NULL,													/* description */
    
    "Duarte Henriques <duarte.henriques@gmail.com>",		/* author */
    "http://sourceforge.net/projects/gaim-libnotify/",		/* homepage */
    
    plugin_load,			/* load */
    plugin_unload,			/* unload */
    NULL,					/* destroy */
    NULL,					/* ui info */
    NULL,					/* extra info */
    &prefs_info				/* prefs info */
};

static void
init_plugin (PurplePlugin *plugin)
{
	bindtextdomain (PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (PACKAGE, "UTF-8");

	info.name = _("Libnotify Popups");
	info.summary = _("Displays popups via libnotify.");
	info.description = _("Pidgin-libnotify:\nDisplays popups via libnotify.");

	/* If we get init'd and we never get loaded
	   chances are the user hasn't enabled this
	   plugin. */
	never_loaded = g_timeout_add_seconds(30, plugin_never_loaded, NULL);

	purple_prefs_add_none ("/plugins/gtk/libnotify");

	/* Create a list of nicks that are commonly used by
	   IRC servers but don't represent real people. */
	GList * nicklist = NULL;
	nicklist = g_list_append(nicklist, "NickServ");
	nicklist = g_list_append(nicklist, "ChanServ");
	nicklist = g_list_append(nicklist, "MsgServ");
	nicklist = g_list_append(nicklist, "freenode-connect");

	purple_prefs_add_string_list ("/plugins/gtk/libnotify/blocked_nicks", nicklist);
	g_list_free(nicklist);

	purple_prefs_add_bool ("/plugins/gtk/libnotify/newmsg", TRUE);
	purple_prefs_add_bool ("/plugins/gtk/libnotify/blocked", TRUE);
	purple_prefs_add_bool ("/plugins/gtk/libnotify/newconvonly", FALSE);
	purple_prefs_add_bool ("/plugins/gtk/libnotify/signon", TRUE);
	purple_prefs_add_bool ("/plugins/gtk/libnotify/signoff", FALSE);
	purple_prefs_add_bool ("/plugins/gtk/libnotify/only_available", FALSE);
	purple_prefs_add_bool ("/plugins/gtk/libnotify/auto_loaded", FALSE);

	g_idle_add(force_load_once, plugin);
}

PURPLE_INIT_PLUGIN(notify, init_plugin, info)

