/*
 * Copyright © 2018 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Matthias Clasen
 */

#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <pwd.h>

#include <glib/gi18n.h>

#include "libglnx/libglnx.h"

#ifdef HAVE_LIBSYSTEMD
#include <systemd/sd-journal.h>
#endif

#include "flatpak-builtins.h"
#include "flatpak-builtins-utils.h"
#include "flatpak-utils-private.h"
#include "flatpak-table-printer.h"
#include "flatpak-transaction-private.h"

static char *opt_since;
static char *opt_until;
static gboolean opt_show_cols;
static const char **opt_cols;

static GOptionEntry options[] = {
  { "since", 0, 0, G_OPTION_ARG_STRING, &opt_since, N_("Only show changes after TIME"), N_("TIME") },
  { "until", 0, 0, G_OPTION_ARG_STRING, &opt_until, N_("Only show changes before TIME"), N_("TIME") },
  { "show-columns", 0, 0, G_OPTION_ARG_NONE, &opt_show_cols, N_("Show available columns"), NULL },
  { "columns", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_cols, N_("What information to show"), N_("FIELD,…") },
  { NULL }
};

static Column all_columns[] = {
  { "time",         N_("Time"),           N_("Show when the change happend"),       1, 1 },
  { "change",       N_("Change"),         N_("Show the kind of change"),            1, 1 },
  { "ref",          N_("Ref"),            N_("Show the ref"),                       0, 0 },
  { "application",  N_("Application"),    N_("Show the application/runtime ID"),    1, 1 },
  { "arch",         N_("Architecture"),   N_("Show the architecture"),              1, 0 },
  { "branch",       N_("Branch"),         N_("Show the branch"),                    1, 1 },
  { "installation", N_("Installation"),   N_("Show the affected installation"),     1, 1 },
  { "remote",       N_("Remote"),         N_("Show the remote"),                    1, 1 },
  { "commit",       N_("Commit"),         N_("Show the active commit"),             1, 0 },
  { "result",       N_("Result"),         N_("Show whether change was successful"), 1, 1 },
  { "user",         N_("User"),           N_("Show the user doing the change"),     1, 0 },
  { "tool",         N_("Tool"),           N_("Show the tool that was used"),        1, 0 },
  { "version",      N_("Version"),        N_("Show the Flatpak version"),           1, 0 },
  { NULL }
};

#ifdef HAVE_LIBSYSTEMD

static const char *
dir_get_id (FlatpakDir *dir)
{
  const char *id;

  if (flatpak_dir_is_user (dir))
    return "user";

  id = flatpak_dir_get_id (dir);
  if (g_strcmp0 (id, "default") != 0)
    return id ? id : "unknown";

  return "system";
}

static char *
get_field (sd_journal *j,
           const char *name,
           GError **error)
{
  const char *data;
  gsize len;
  int r;

  if ((r = sd_journal_get_data (j, name, (const void **)&data, &len)) < 0)
    {
      if (r != -ENOENT)
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     _("Failed to get journal data (%s): %s"),
                     name, strerror (-r));

      return NULL;
    }

  return g_strndup (data + strlen (name) + 1, len - (strlen (name) + 1));
}

static GDateTime *
get_time (sd_journal *j,
          GError **error)
{
  g_autofree char *value = NULL;
  GError *local_error = NULL;
  gint64 t;

  value = get_field (j, "_SOURCE_REALTIME_TIMESTAMP", &local_error);

  if (local_error)
    {
      g_propagate_error (error, local_error);
      return NULL;
    }

  t = g_ascii_strtoll (value, NULL, 10) / 1000000;
  return g_date_time_new_from_unix_local (t);
}

static gboolean
print_history (GPtrArray *dirs,
               Column *columns,
               GDateTime *since,
               GDateTime *until,
               GCancellable *cancellable,
               GError **error)
{
  FlatpakTablePrinter *printer;
  sd_journal *j;
  int r;
  int i;
  int k;

  if (columns[0].name == NULL)
    return TRUE;

  printer = flatpak_table_printer_new ();
  for (i = 0; columns[i].name; i++)
    flatpak_table_printer_set_column_title (printer, i, _(columns[i].title));  

  if ((r = sd_journal_open (&j, 0)) < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   _("Failed to open journal: %s"), strerror (-r));
      return FALSE;
    }

  if ((r = sd_journal_add_match (j, "MESSAGE_ID=" MESSAGE_TRANSACTION, 0)) < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   _("Failed to add match to journal: %s"), strerror (-r));
      return FALSE;
    }

  SD_JOURNAL_FOREACH_BACKWARDS (j)	
    {
      /* determine whether to skip this entry */

      if (dirs)
        {
          gboolean include = FALSE;
          g_autofree char *installation = get_field (j, "INSTALLATION", NULL);

          for (i = 0; i < dirs->len; i++)
            {
              const char *id = dir_get_id (dirs->pdata[i]);
              if (g_strcmp0 (id, installation) == 0)
                {
                  include = TRUE;
                  break;
                }
            }
          if (!include)
            continue;
        }

      if (since || until)
        {
          g_autoptr(GDateTime) time = get_time (j, NULL);

          if (time && g_date_time_difference (since, time) >= 0)
            continue;

          if (time && g_date_time_difference (time, until) >= 0)
            continue;
        }

      for (k = 0; columns[k].name; k++)
        {
          if (strcmp (columns[k].name, "time") == 0)
            {
              g_autoptr(GDateTime) time = NULL;
              g_autofree char *s = NULL;

              time = get_time (j, error);
              if (*error)
                return FALSE;

              s = g_date_time_format (time, "%X");
              flatpak_table_printer_add_column (printer, s);
            }
          else if (strcmp (columns[k].name, "change") == 0)
            {
              g_autofree char *op = get_field (j, "OPERATION", error);
              if (*error)
                return FALSE;
              flatpak_table_printer_add_column (printer, op);
            }
          else if (strcmp (columns[k].name, "ref") == 0 ||
                   strcmp (columns[k].name, "application") == 0 ||
                   strcmp (columns[k].name, "arch") == 0 ||
                   strcmp (columns[k].name, "branch") == 0)
            {
              g_autofree char *ref = get_field (j, "REF", error);
              if (*error)
                return FALSE;
              if (strcmp (columns[k].name, "ref") == 0)
                flatpak_table_printer_add_column (printer, ref);
              else
                {
                  g_auto(GStrv) pref = flatpak_decompose_ref (ref, NULL);
                  if (strcmp (columns[k].name, "application") == 0)
                    flatpak_table_printer_add_column (printer, pref ? pref[1] : "");
                  else if (strcmp (columns[k].name, "arch") == 0)
                    flatpak_table_printer_add_column (printer, pref ? pref[2] : "");
                  else
                    flatpak_table_printer_add_column (printer, pref ? pref[3] : "");
                }
            }
          else if (strcmp (columns[k].name, "installation") == 0)
            {
              g_autofree char *installation = get_field (j, "INSTALLATION", error);
              if (*error)
                return FALSE;
              flatpak_table_printer_add_column (printer, installation);
            }
          else if (strcmp (columns[k].name, "remote") == 0)
            {
              g_autofree char *remote = get_field (j, "REMOTE", error);
              if (*error)
                return FALSE;
              flatpak_table_printer_add_column (printer, remote);
            }
          else if (strcmp (columns[k].name, "commit") == 0)
            {
              g_autofree char *commit = get_field (j, "COMMIT", error);
              if (*error)
                return FALSE;
              flatpak_table_printer_add_column_len (printer, commit, 12);
            }
          else if (strcmp (columns[k].name, "result") == 0)
            {
              g_autofree char *result = get_field (j, "RESULT", error);
              if (*error)
                return FALSE;
              if (g_strcmp0 (result, "0") != 0)
                flatpak_table_printer_add_column (printer, "✓");
              else
                flatpak_table_printer_add_column (printer, "");
            }
          else if (strcmp (columns[k].name, "user") == 0)
            {
              g_autofree char *id = get_field (j, "_UID", error);
              int uid;
              struct passwd *pwd;

              if (*error)
                return FALSE;

              uid = g_ascii_strtoll (id, NULL, 10);
              pwd = getpwuid (uid);
              flatpak_table_printer_add_column (printer, pwd ? pwd->pw_name : id);
            }
          else if (strcmp (columns[k].name, "tool") == 0)
            {
              g_autofree char *tool = get_field (j, "_COMM", error);
              if (*error)
                return FALSE;
              flatpak_table_printer_add_column (printer, tool);
            }
          else if (strcmp (columns[k].name, "version") == 0)
            {
              g_autofree char *version = get_field (j, "FLATPAK_VERSION", error);
              if (*error)
                return FALSE;
              flatpak_table_printer_add_column (printer, version);
            }
        }

      flatpak_table_printer_finish_row (printer);
    }
 
  flatpak_table_printer_print (printer);
  flatpak_table_printer_free (printer);

  sd_journal_close (j);
  
  return TRUE;
}

#else

static gboolean
print_history (GPtrArray *dirs,
               Column *columns,
               GDateTime *since,
               GDateTime *until,
               GCancellable *cancellable,
               GError **error)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "history not available without libsystemd");
  return FALSE;
}

#endif

static GDateTime *
parse_time (const char *opt_since)
{
  g_autoptr (GDateTime) now = NULL;
  g_auto(GStrv) parts = NULL;
  int i;
  int days = 0;
  int hours = 0;
  int minutes = 0;
  int seconds = 0;
  const char *fmts[] = {
    "%H:%M",
    "%H:%M:%S",
    "%Y-%m-%d",
    "%Y-%m-%d %H:%M:%S"
  };

  now = g_date_time_new_now_local ();

  for (i = 0; i < G_N_ELEMENTS(fmts); i++)
    {
      const char *rest;
      struct tm tm;

      tm.tm_year = g_date_time_get_year (now);
      tm.tm_mon = g_date_time_get_month (now);
      tm.tm_mday = g_date_time_get_day_of_month (now);
      tm.tm_hour = 0;
      tm.tm_min = 0;
      tm.tm_sec = 0;

      rest = strptime (opt_since, fmts[i], &tm);
      if (rest && *rest == '\0')
         return g_date_time_new_local (tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    }

  parts = g_strsplit (opt_since, " ", -1);

  for (i = 0; parts[i]; i++)
    {
      gint64 n;
      char *end;

      n = g_ascii_strtoll (parts[i], &end, 10);
      if (g_strcmp0 (end, "d") == 0 ||
          g_strcmp0 (end, "day") == 0 ||
          g_strcmp0 (end, "days") == 0)
        days = (int) n;
      else if (g_strcmp0 (end, "h") == 0 ||
               g_strcmp0 (end, "hour") == 0 ||
               g_strcmp0 (end, "hours") == 0)
        hours = (int) n;
      else if (g_strcmp0 (end, "m") == 0 ||
               g_strcmp0 (end, "minute") == 0 ||
               g_strcmp0 (end, "minutes") == 0)
        minutes = (int) n;
      else if (g_strcmp0 (end, "s") == 0 ||
               g_strcmp0 (end, "second") == 0 ||
               g_strcmp0 (end, "seconds") == 0)
        seconds = (int) n;
      else
        return NULL;
    }

  return g_date_time_add_full (now, 0, 0, -days, -hours, -minutes, -seconds);
}

gboolean
flatpak_builtin_history (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  g_autoptr(GDateTime) since = NULL;
  g_autoptr(GDateTime) until = NULL;
  g_autofree char *col_help = NULL;
  g_autofree Column *columns = NULL;

  context = g_option_context_new (_(" - Show history"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);
  col_help = column_help (all_columns);
  g_option_context_set_description (context, col_help);

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_ALL_DIRS | FLATPAK_BUILTIN_FLAG_OPTIONAL_REPO,
                                     &dirs, cancellable, error))
    return FALSE;

  if (argc > 1)
    return usage_error (context, _("Too many arguments"), error);

  if (opt_since)
    {
      since = parse_time (opt_since);
      if (!since)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                       _("Failed to parse the --since option"));
          return FALSE;
        }
    }

  if (opt_until)
    {
      until = parse_time (opt_until);
      if (!until)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                       _("Failed to parse the --until option"));
          return FALSE;
        }
    }
  columns = handle_column_args (all_columns,
                                opt_show_cols, FALSE, opt_cols,
                                error);
  if (columns == NULL)
    return FALSE;

  if (!print_history (dirs, columns, since, until, cancellable, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_complete_history (FlatpakCompletion *completion)
{
  flatpak_complete_options (completion, global_entries);
  flatpak_complete_options (completion, options);
  return TRUE;
}