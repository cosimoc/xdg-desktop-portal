/*
 * Copyright © 2016 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Matthias Clasen <mclasen@redhat.com>
 */

#include "config.h"

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <flatpak.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <gio/gdesktopappinfo.h>

#include "open-uri.h"
#include "request.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"
#include "permissions.h"

#define TABLE_NAME "desktop-used-apps"
#define USE_DEFAULT_APP_THRESHOLD 5

typedef struct _OpenURI OpenURI;

typedef struct _OpenURIClass OpenURIClass;

struct _OpenURI
{
  XdpOpenURISkeleton parent_instance;
};

struct _OpenURIClass
{
  XdpOpenURISkeletonClass parent_class;
};

static XdpImplAppChooser *app_chooser_impl;
static OpenURI *open_uri;

GType open_uri_get_type (void) G_GNUC_CONST;
static void open_uri_iface_init (XdpOpenURIIface *iface);

G_DEFINE_TYPE_WITH_CODE (OpenURI, open_uri, XDP_TYPE_OPEN_URI_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_OPEN_URI, open_uri_iface_init));

static gboolean
get_latest_choice_info (const char *app_id,
                        const char *content_type,
                        gchar **latest_chosen_id,
                        gint *latest_chosen_count)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) out_perms = NULL;
  g_autoptr(GVariant) out_data = NULL;

  if (!xdp_impl_permission_store_call_lookup_sync (get_permission_store (),
                                                   TABLE_NAME,
                                                   content_type,
                                                   &out_perms,
                                                   &out_data,
                                                   NULL,
                                                   &error))
    {
      g_warning ("Error updating permission store: %s", error->message);
      g_clear_error (&error);
    }

  if (out_perms != NULL)
    {
      GVariantIter iter;
      GVariant *child;
      gboolean app_found = FALSE;

      g_variant_iter_init (&iter, out_perms);
      while (!app_found && (child = g_variant_iter_next_value (&iter)))
        {
          const char *child_app_id;
          g_autofree const char **permissions;

          g_variant_get (child, "{&s^a&s}", &child_app_id, &permissions);
          if (g_strcmp0 (child_app_id, app_id) == 0 &&
              permissions != NULL &&
              permissions[0] != NULL)
            {
              g_auto(GStrv) permission_detail = g_strsplit (permissions[0], ":", 2);
              if (g_strv_length (permission_detail) >= 2)
                {
                  *latest_chosen_id = g_strdup (permission_detail[0]);
                  *latest_chosen_count = atoi(permission_detail[1]);
                }
              app_found = TRUE;
            }
          g_variant_unref (child);
        }
    }

  return (*latest_chosen_id != NULL);
}

static void
launch_application_with_uri (const char *choice_id,
                             const char *uri,
                             const char *parent_window)
{
  g_autoptr(GAppInfo) info = G_APP_INFO (g_desktop_app_info_new (choice_id));
  g_autoptr(GAppLaunchContext) context = g_app_launch_context_new ();
  GList uris;

  g_app_launch_context_setenv (context, "PARENT_WINDOW_ID", parent_window);

  uris.data = (gpointer)uri;
  uris.next = NULL;

  g_debug ("launching %s\n", choice_id);
  g_app_info_launch_uris (info, &uris, context, NULL);
}

static void
update_permissions_store (const char *app_id,
                          const char *content_type,
                          const char *chosen_id)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *latest_chosen_id = NULL;
  gint latest_chosen_count = 0;
  g_auto(GStrv) in_permissions = NULL;

  if (get_latest_choice_info (app_id, content_type, &latest_chosen_id, &latest_chosen_count) &&
      (g_strcmp0 (chosen_id, latest_chosen_id) == 0))
    {
      /* same app chosen once again: update the counter */
      if (latest_chosen_count >= USE_DEFAULT_APP_THRESHOLD)
        latest_chosen_count = USE_DEFAULT_APP_THRESHOLD;
      else
        latest_chosen_count++;
    }
  else
    {
      /* latest_chosen_id is heap-allocated */
      latest_chosen_id = g_strdup (chosen_id);
      latest_chosen_count = 0;
    }

  in_permissions = (GStrv) g_new0 (char *, 2);
  in_permissions[0] = g_strdup_printf ("%s:%u", latest_chosen_id, latest_chosen_count);

  if (!xdp_impl_permission_store_call_set_permission_sync (get_permission_store (),
                                                           TABLE_NAME,
                                                           TRUE,
                                                           content_type,
                                                           app_id,
                                                           (const char * const*) in_permissions,
                                                           NULL,
                                                           &error))
    {
      g_warning ("Error updating permission store: %s", error->message);
      g_clear_error (&error);
    }
}

static void
send_response_in_thread_func (GTask *task,
                              gpointer source_object,
                              gpointer task_data,
                              GCancellable *cancellable)
{
  Request *request = (Request *)task_data;
  guint response;
  GVariant *options;
  const char *uri;
  const char *parent_window;
  const char *content_type;
  const char *choice;
  GVariantBuilder opt_builder;

  REQUEST_AUTOLOCK (request);

  response = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (request), "response"));
  options = (GVariant *)g_object_get_data (G_OBJECT (request), "options");

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

  if (response != 0)
    goto out;

  if (g_variant_lookup (options, "choice", "&s", &choice))
    {
      uri = g_object_get_data (G_OBJECT (request), "uri");
      parent_window = g_object_get_data (G_OBJECT (request), "parent-window");
      content_type = g_object_get_data (G_OBJECT (request), "content-type");

      launch_application_with_uri (choice, uri, parent_window);
      update_permissions_store (request->app_id, content_type, choice);
    }

out:
  if (request->exported)
    {
      xdp_request_emit_response (XDP_REQUEST (request), response, g_variant_builder_end (&opt_builder));
      request_unexport (request);
    }
}

static void
app_chooser_done (GObject *source,
                  GAsyncResult *result,
                  gpointer data)
{
  g_autoptr (Request) request = data;
  guint response = 2;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GTask) task = NULL;

  if (!xdp_impl_app_chooser_call_choose_application_finish (XDP_IMPL_APP_CHOOSER (source),
                                                            &response,
                                                            &options,
                                                            result,
                                                            &error))
    {
      g_warning ("Backend call failed: %s", error->message);
    }

  g_object_set_data (G_OBJECT (request), "response", GINT_TO_POINTER (response));
  if (options)
    g_object_set_data_full (G_OBJECT (request), "options", g_variant_ref (options), (GDestroyNotify)g_variant_unref);

  task = g_task_new (NULL, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, send_response_in_thread_func);
}

static void
handle_open_fd_in_thread_func (GTask *task,
                               gpointer source_object,
                               gpointer task_data,
                               GCancellable *cancellable)
{
  Request *request = (Request *)task_data;
  g_autofree char *proc_path;
  g_autofree char *proc_uri;
  g_autofree char *basename;
  g_autofree char *content_type = NULL;
  const char *parent_window;
  const char *path;
  int fd;
  guchar sniff_buffer[4096];
  gsize sniff_length = 4096;
  gboolean res;
  g_autoptr(GAppInfo) info = NULL;
  GList uris;

  fd = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (request), "fd"));
  path = (const char *)g_object_get_data (G_OBJECT (request), "path");
  parent_window = (const char *)g_object_get_data (G_OBJECT (request), "parent-window");

  REQUEST_AUTOLOCK (request);

  proc_path = g_strdup_printf ("/proc/self/fd/%d", fd);
  proc_uri = g_filename_to_uri (proc_path, NULL, NULL);
  uris.data = proc_uri;
  uris.prev = uris.next = NULL;

  basename = g_path_get_basename (path);

  res = read (fd, sniff_buffer, sniff_length);
  if (res < 0)
    {
      g_warning ("cannot read from fd");
      return;
    }

  content_type = g_content_type_guess (basename, sniff_buffer, res, NULL);
                                            
}

static void
handle_open_in_thread_func (GTask *task,
                            gpointer source_object,
                            gpointer task_data,
                            GCancellable *cancellable)
{
  Request *request = (Request *)task_data;
  const char *parent_window;
  const char *uri;
  const char *app_id = request->app_id;
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpImplRequest) impl_request = NULL;
  g_auto(GStrv) choices = NULL;
  guint n_choices = 0;
  GList *infos, *l;
  g_autofree char *uri_scheme = NULL;
  g_autofree char *scheme_down = NULL;
  g_autofree char *content_type = NULL;
  g_autofree char *latest_chosen_id = NULL;
  gint latest_chosen_count = 0;
  GVariantBuilder opts_builder;
  gboolean use_first_choice = FALSE;
  int i;

  parent_window = (const char *)g_object_get_data (G_OBJECT (request), "parent-window");
  uri = (const char *)g_object_get_data (G_OBJECT (request), "uri");

  REQUEST_AUTOLOCK (request);

  uri_scheme = g_uri_parse_scheme (uri);
  if (uri_scheme && uri_scheme[0] != '\0')
    scheme_down = g_ascii_strdown (uri_scheme, -1);

  if ((scheme_down != NULL) && (strcmp (scheme_down, "file") != 0))
    {
      content_type = g_strconcat ("x-scheme-handler/", scheme_down, NULL);
    }
  else
    {
      g_autoptr(GError) error = NULL;
      g_autoptr(GFile) file = g_file_new_for_uri (uri);
      g_autoptr(GFileInfo) info = g_file_query_info (file,
                                                     G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
                                                     0,
                                                     NULL,
                                                     &error);

      if (info != NULL)
        {
          content_type = g_strdup (g_file_info_get_content_type (info));
        }
      else
        {
          g_debug ("failed to fetch content type for uri %s: %s", uri, error->message);
          return;
        }
    }

  infos = g_app_info_get_recommended_for_type (content_type);
  n_choices = g_list_length (infos);
  choices = g_new (char *, n_choices + 1);
  for (l = infos, i = 0; l; l = l->next)
    {
      GAppInfo *info = l->data;
      choices[i++] = g_strdup (g_app_info_get_id (info));
    }
  choices[i] = NULL;
  g_list_free_full (infos, g_object_unref);

  /* We normally want a dialog to show up at least a few times, but for http[s] we can
     make an exception in case there's only one candidate application to handle it */
  if ((n_choices == 1) &&
      ((g_strcmp0 (scheme_down, "http") == 0) || (g_strcmp0 (scheme_down, "https") == 0)))
    {
      use_first_choice = TRUE;
    }

  if (use_first_choice ||
      (get_latest_choice_info (app_id, content_type, &latest_chosen_id, &latest_chosen_count) &&
       (latest_chosen_count >= USE_DEFAULT_APP_THRESHOLD)))
    {
      /* If a recommended choice is found, just use it and skip the chooser dialog */
      launch_application_with_uri (use_first_choice ? choices[0] : latest_chosen_id,
                                   uri,
                                   parent_window);

      if (request->exported)
        {
          g_variant_builder_init (&opts_builder, G_VARIANT_TYPE_VARDICT);
          xdp_request_emit_response (XDP_REQUEST (request), 0, g_variant_builder_end (&opts_builder));
          request_unexport (request);
        }
      return;
    }

  g_variant_builder_init (&opts_builder, G_VARIANT_TYPE_VARDICT);

  if (latest_chosen_id != NULL)
    {
      /* Add extra options to the request for the backend */
      g_variant_builder_add (&opts_builder,
                             "{sv}",
                             "latest-choice",
                             g_variant_new_string (latest_chosen_id));
    }

  g_object_set_data_full (G_OBJECT (request), "content-type", g_strdup (content_type), g_free);

  impl_request = xdp_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (app_chooser_impl)),
                                                  G_DBUS_PROXY_FLAGS_NONE,
                                                  g_dbus_proxy_get_name (G_DBUS_PROXY (app_chooser_impl)),
                                                  request->id,
                                                  NULL, NULL);

  request_set_impl_request (request, impl_request);

  xdp_impl_app_chooser_call_choose_application (app_chooser_impl,
                                                request->id,
                                                app_id,
                                                parent_window,
                                                (const char * const *)choices,
                                                g_variant_builder_end (&opts_builder),
                                                NULL,
                                                app_chooser_done,
                                                g_object_ref (request));
}

static char *
uri_from_app_symlink (const char *app_symlink,
                      Request *request)
{
  char *path;
  const char *deploy_dir;
  g_autofree char *real_path = NULL;
  g_autoptr(FlatpakInstalledRef) app_ref = NULL;
  g_autoptr(FlatpakInstallation) user_install = NULL;

  path = strstr (app_symlink, "/app");
  if (!path)
    return NULL;

  path += strlen ("/app");

  user_install = flatpak_installation_new_user (NULL, NULL);

  if (user_install)
    {
      app_ref =
        flatpak_installation_get_current_installed_app (user_install,
                                                        request->app_id,
                                                        NULL, NULL);
    }

  if (!app_ref)
    {
      g_autoptr(FlatpakInstallation) system_install =
        flatpak_installation_new_system (NULL, NULL);

      if (system_install)
        {
          app_ref =
            flatpak_installation_get_current_installed_app (system_install,
                                                            request->app_id,
                                                            NULL, NULL);
        }
    }

  if (!app_ref)
    return NULL;

  deploy_dir = flatpak_installed_ref_get_deploy_dir (app_ref);
  real_path = g_build_filename (deploy_dir, "files", path, NULL);

  return g_filename_to_uri (real_path, NULL, NULL);
}

static gboolean
handle_open_fd (XdpOpenURI *object,
                GDBusMethodInvocation *invocation,
                GUnixFDList *fd_list,
                const gchar *arg_parent_window,
                GVariant *arg_fd,
                GVariant *arg_options)

{
  int idx, fd;
  g_autofree char *proc_path = NULL;
  g_autofree char *uri = NULL;
  ssize_t symlink_size;
  char path_buffer[PATH_MAX + 1];
  Request *request;
  g_autoptr(GFile) file = NULL;
  g_autoptr(GTask) task = NULL;

  g_variant_get (arg_fd, "h", &idx);
  fd = g_unix_fd_list_get (fd_list, idx, NULL);
  proc_path = g_strdup_printf ("/proc/self/fd/%d", fd);

  if ((symlink_size = readlink (proc_path, path_buffer, PATH_MAX)) < 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Cannot access file descriptor");
      return TRUE;
    }

  path_buffer[symlink_size] = 0;

  request = request_from_invocation (invocation);
  /* XXX: is there any other way to do this using the FD directly? */
  uri = uri_from_app_symlink (path_buffer, request);

  g_object_set_data (G_OBJECT (request), "fd", G_INT_TO_POINTER (fd));
  g_object_set_data_full (G_OBJECT (request), "path", g_strdup (path_buffer), g_free);
  g_object_set_data_full (G_OBJECT (request), "parent-window", g_strdup (arg_parent_window), g_free);

  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  task = g_task_new (object, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, handle_open_fd_in_thread_func);

  return TRUE;
}

static gboolean
handle_open_uri (XdpOpenURI *object,
                 GDBusMethodInvocation *invocation,
                 const gchar *arg_parent_window,
                 const gchar *arg_uri,
                 GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  g_autoptr(GTask) task = NULL;

  g_object_set_data_full (G_OBJECT (request), "uri", g_strdup (arg_uri), g_free);
  g_object_set_data_full (G_OBJECT (request), "parent-window", g_strdup (arg_parent_window), g_free);

  request_export (request, g_dbus_method_invocation_get_connection (invocation));
  xdp_open_uri_complete_open_uri (object, invocation, request->id);

  task = g_task_new (object, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, handle_open_in_thread_func);

  return TRUE;
}

static void
open_uri_iface_init (XdpOpenURIIface *iface)
{
  iface->handle_open_uri = handle_open_uri;
  iface->handle_open_fd = handle_open_fd;
}

static void
open_uri_init (OpenURI *fc)
{
}

static void
open_uri_class_init (OpenURIClass *klass)
{
}

GDBusInterfaceSkeleton *
open_uri_create (GDBusConnection *connection,
                 const char *dbus_name)
{
  g_autoptr(GError) error = NULL;

  app_chooser_impl = xdp_impl_app_chooser_proxy_new_sync (connection,
                                                          G_DBUS_PROXY_FLAGS_NONE,
                                                          dbus_name,
                                                          DESKTOP_PORTAL_OBJECT_PATH,
                                                          NULL, &error);
  if (app_chooser_impl == NULL)
    {
      g_warning ("Failed to create app chooser proxy: %s", error->message);
      return NULL;
    }

  open_uri = g_object_new (open_uri_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (open_uri);
}

