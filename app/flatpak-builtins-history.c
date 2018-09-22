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

#include <glib/gi18n.h>

#include "libglnx/libglnx.h"
#include <systemd/sd-journal.h>

#include "flatpak-builtins.h"
#include "flatpak-utils-private.h"
#include "flatpak-table-printer.h"
#include "flatpak-transaction-private.h"

static char *opt_since;

static GOptionEntry options[] = {
  { "since", 0, 0, G_OPTION_ARG_STRING, &opt_since, N_("Show entries newer than TIME"), N_("TIME") },
  { NULL }
};

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

static gboolean
print_history (GPtrArray *dirs,
               GDateTime *since,
               GCancellable *cancellable,
               GError **error)
{
  FlatpakTablePrinter *printer;
  sd_journal *j;
  int r;
  int i;

  printer = flatpak_table_printer_new ();

  i = 0;
  flatpak_table_printer_set_column_title (printer, i++, _("Time"));  
  flatpak_table_printer_set_column_title (printer, i++, _("Operation"));  
  flatpak_table_printer_set_column_title (printer, i++, _("Installation"));  
  flatpak_table_printer_set_column_title (printer, i++, _("Application"));  
  flatpak_table_printer_set_column_title (printer, i++, _("Branch"));  
  flatpak_table_printer_set_column_title (printer, i++, _("Remote"));  
  flatpak_table_printer_set_column_title (printer, i++, _("Commit"));  
  flatpak_table_printer_set_column_title (printer, i++, _("Success"));  

  if ((r = sd_journal_open (&j, 0)) < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to open journal: %s", strerror (-r));
      return FALSE;
    }

  if ((r = sd_journal_add_match (j, "_COMM=flatpak", 0)) < 0 ||
      (r = sd_journal_add_match (j, "MESSAGE_ID=" MESSAGE_TRANSACTION, 0)) < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to add match to journal: %s", strerror (-r));
      return FALSE;
    }

  SD_JOURNAL_FOREACH_BACKWARDS (j)	
    {
      struct { const char *name; char *value; } fields[] = {
        { "_SOURCE_REALTIME_TIMESTAMP", NULL },
        { "OPERATION", NULL },
        { "INSTALLATION", NULL },
        { "REF", NULL },
        { "REMOTE", NULL },
        { "COMMIT", NULL },
        { "RESULT", NULL },
      };

      for (i = 0; i < G_N_ELEMENTS(fields); i++)
        {
          const char *data;
          gsize len;

          if ((r = sd_journal_get_data (j, fields[i].name, (const void **)&data, &len)) < 0)
            {
              if (r != -ENOENT)
                {
                  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Failed to get journal data (%s): %s",
                               fields[i].name, strerror (-r));
                  return FALSE;
                }
            }
          fields[i].value = g_strdup (data + strlen (fields[i].name) + 1);
        }

      if (dirs)
        {
          gboolean include = FALSE;

          for (i = 0; i < dirs->len; i++)
            {
              const char *id = dir_get_id (dirs->pdata[i]);
              if (g_strcmp0 (id, fields[2].value) == 0)
                {
                  include = TRUE;
                  break;
                }
            }
          if (!include)
            continue;
        }

      if (fields[0].value)
        {
          gint64 t;
          g_autoptr(GDateTime) dt = NULL;
          g_autofree char *s = NULL;

          t = g_ascii_strtoll (fields[0].value, NULL, 10);
          t /= 1000000;
          dt = g_date_time_new_from_unix_local (t);

          if (since && g_date_time_difference (since, dt) >= 0)
            continue;

          s = g_date_time_format (dt, "%X");
          flatpak_table_printer_add_column (printer, s);
        }

      flatpak_table_printer_add_column (printer, fields[1].value);
      flatpak_table_printer_add_column (printer, fields[2].value);

      if (fields[3].value)
        {
          g_auto(GStrv) ref = flatpak_decompose_ref (fields[3].value, NULL);
          if (ref)
            {
              flatpak_table_printer_add_column (printer, ref[1]);
              flatpak_table_printer_add_column (printer, ref[2]);
            }
          else
            {
              flatpak_table_printer_add_column (printer, "");
              flatpak_table_printer_add_column (printer, "");
            }
        }

      flatpak_table_printer_add_column (printer, fields[4].value);
      flatpak_table_printer_add_column_len (printer, fields[5].value, 12);

      if (g_strcmp0 (fields[6].value, "0") != 0)
        flatpak_table_printer_add_column (printer, "✓");
      else
        flatpak_table_printer_add_column (printer, "");

      flatpak_table_printer_finish_row (printer);
    }
 
  flatpak_table_printer_print (printer);
  flatpak_table_printer_free (printer);

  sd_journal_close (j);
  
  return TRUE;
}

static GDateTime *
parse_since (const char *opt_since,
             GError **error)
{
  g_autoptr (GDateTime) now = NULL;
  g_auto(GStrv) parts = NULL;
  int i;
  int days = 0;
  int hours = 0;
  int minutes = 0;
  int seconds = 0;

  now = g_date_time_new_now_local ();

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
    }

  return g_date_time_add_full (now, 0, 0, -days, -hours, -minutes, -seconds);
}

gboolean
flatpak_builtin_history (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  g_autoptr(GDateTime) since = NULL;

  context = g_option_context_new (_(" - Show history"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_ALL_DIRS | FLATPAK_BUILTIN_FLAG_OPTIONAL_REPO,
                                     &dirs, cancellable, error))
    return FALSE;

  if (argc > 1)
    return usage_error (context, _("Too many arguments"), error);

  if (opt_since)
    {
      since = parse_since (opt_since, error);
      if (!since)
        return FALSE;
    }

  if (!print_history (dirs, since, cancellable, error))
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
