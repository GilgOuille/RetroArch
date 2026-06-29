/* RetroArch — Online ROM Library (WebDAV)
 *
 * rom_library_menu.c — Menu glue implementation (phase 3).
 *
 * Navigation (all reached through the single balisé hooks documented in
 * rom_library/INTEGRATION.md):
 *
 *   Main Menu
 *     └─ "Online ROM Library"           (MENU_ENUM_LABEL_ROM_LIBRARY_LIST)
 *          ├─ "ROM Library Settings"    (… _SETTINGS)  → URL/user/password (OSK)
 *          └─ <system>                  (DEFERRED_… _ENTRIES, path = system idx)
 *               └─ <rom>                (… _ENTRY,  OK → streaming download)
 *
 * Each navigable page is pushed as a generic deferred list (ACTION_OK_DL_GENERIC):
 * the row label selects which page rom_library_menu_deferred_push() builds, and
 * the row path carries the selected system index (read back from the menu's
 * deferred_path). The systems / entries content comes from the process-global
 * catalogue, filled asynchronously by rom_library_task.c.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <retro_common_api.h>
#include <string/stdstring.h>
#include <lists/file_list.h>
#include <queues/message_queue.h>

#include "../msg_hash.h"
#include "../configuration.h"
#include "../verbosity.h"
#include "../runloop.h"
#include "../menu/menu_displaylist.h"
#include "../menu/menu_entries.h"
#include "../menu/menu_input.h"
#include "../menu/menu_driver.h"
#include "../menu/menu_cbs.h"
#include "../input/input_driver.h"

#include "rom_library_menu.h"
#include "rom_library.h"
#include "rom_library_config.h"
#include "rom_library_task.h"

/* --------------------------------------------------------------------------
 * Small helpers
 * -------------------------------------------------------------------------- */

static const char *rl_lbl(enum msg_hash_enums e)
{
   return msg_hash_to_str(e);
}

static void rl_human_size(char *s, size_t len, int64_t bytes)
{
   static const char *units[] = { "B", "KB", "MB", "GB", "TB" };
   double             b        = (double)bytes;
   int                i        = 0;

   if (bytes < 0)
   {
      strlcpy(s, "?", len);
      return;
   }

   while (b >= 1024.0 && i < 4)
   {
      b /= 1024.0;
      i++;
   }

   if (i == 0)
      snprintf(s, len, "%d %s", (int)bytes, units[0]);
   else
      snprintf(s, len, "%.1f %s", b, units[i]);
}

/* Current menu's deferred_path (carries the selected system index string). */
static const char *rl_deferred_path(void)
{
   struct menu_state *menu_st = menu_state_get_ptr();
   menu_handle_t     *menu    = menu_st ? (menu_handle_t*)menu_st->driver_data
                                        : NULL;
   return menu ? menu->deferred_path : NULL;
}

void rom_library_menu_notify_refresh(void)
{
   struct menu_state *menu_st = menu_state_get_ptr();
   if (!menu_st)
      return;
   menu_st->flags |=  MENU_ST_FLAG_ENTRIES_NEED_REFRESH;
   menu_st->flags &= ~MENU_ST_FLAG_ENTRIES_NONBLOCKING_REFRESH;
}

void rom_library_menu_tick_refresh(void)
{
   const char *label = NULL;

   /* Only refresh when the top of the menu stack is a ROM entries page, so we
    * never disturb navigation on any other menu. The entries page container is
    * the generic deferred list pushed with this label (see the header). */
   menu_entries_get_last_stack(NULL, &label, NULL, NULL, NULL);

   if (label && string_is_equal(label,
            rl_lbl(MENU_ENUM_LABEL_DEFERRED_ROM_LIBRARY_ENTRIES)))
      rom_library_menu_notify_refresh();
}

/* Appends a non-selectable informational row (placeholder / hint). */
static int rl_append_info(file_list_t *list, const char *text)
{
   if (menu_entries_append(list, text, "",
            MENU_ENUM_LABEL_NO_ITEMS, FILE_TYPE_NONE, 0, 0, NULL))
   {
      file_list_set_alt_at_offset(list, list->size - 1, text);
      return 1;
   }
   return 0;
}

/* --------------------------------------------------------------------------
 * Page builders
 * -------------------------------------------------------------------------- */

static int rl_build_systems(file_list_t *list)
{
   rom_library_t *lib = rom_library_get_global();
   size_t         n   = rom_library_system_count(lib);
   size_t         i;
   int            count = 0;

   /* Settings entry first. */
   if (menu_entries_append(list, "",
            rl_lbl(MENU_ENUM_LABEL_ROM_LIBRARY_SETTINGS),
            MENU_ENUM_LABEL_ROM_LIBRARY_SETTINGS,
            FILE_TYPE_DOWNLOAD_URL, 0, 0, NULL))
   {
      file_list_set_alt_at_offset(list, list->size - 1,
            msg_hash_to_str(MENU_ENUM_LABEL_VALUE_ROM_LIBRARY_SETTINGS));
      count++;
   }

   if (n == 0)
   {
      count += rl_append_info(list, rom_library_task_systems_busy()
            ? "Fetching system list\xe2\x80\xa6"
            : "No systems found (check Settings / connection)");
      return count;
   }

   for (i = 0; i < n; i++)
   {
      rom_library_system_t *s = rom_library_get_system(lib, i);
      char                  idxbuf[16];

      snprintf(idxbuf, sizeof(idxbuf), "%u", (unsigned)i);

      if (menu_entries_append(list, idxbuf,
               rl_lbl(MENU_ENUM_LABEL_DEFERRED_ROM_LIBRARY_ENTRIES),
               MENU_ENUM_LABEL_DEFERRED_ROM_LIBRARY_ENTRIES,
               FILE_TYPE_DOWNLOAD_URL, 0, 0, NULL))
      {
         file_list_set_alt_at_offset(list, list->size - 1,
               (s && s->name) ? s->name : idxbuf);
         count++;
      }
   }

   return count;
}

static int rl_build_entries(file_list_t *list)
{
   rom_library_t        *lib = rom_library_get_global();
   const char           *dp  = rl_deferred_path();
   int                   sysidx = (dp && *dp) ? atoi(dp) : -1;
   rom_library_system_t *s   = (sysidx >= 0)
         ? rom_library_get_system(lib, (size_t)sysidx) : NULL;
   size_t                i;
   int                   count = 0;

   if (!s)
      return rl_append_info(list, "No system selected");

   if (s->entries_count == 0)
      return rl_append_info(list, rom_library_task_entries_busy()
            ? "Fetching ROM list\xe2\x80\xa6"
            : "No ROMs found in this system");

   for (i = 0; i < s->entries_count; i++)
   {
      rom_library_entry_t *e = &s->entries[i];
      char                 idxbuf[16];
      char                 sz[32];
      char                 disp[320];
      bool                 dl      = rom_library_task_download_is_active(
            (size_t)sysidx, i);
      bool                 present = !dl
            && rom_library_task_entry_is_present(s, e);
      /* Row marker: "\xE2\x86\x93" (down arrow) = download in flight (OK to
       * cancel); "\xE2\x9C\x93" (check mark) = already downloaded locally.
       * Refreshed each time the list is built. */
      const char          *mark    = dl      ? "\xe2\x86\x93 "
                                   : present ? "\xe2\x9c\x93 "
                                   : "";

      snprintf(idxbuf, sizeof(idxbuf), "%u", (unsigned)i);
      rl_human_size(sz, sizeof(sz), e->size);
      snprintf(disp, sizeof(disp), "%s%s   (%s)",
            mark, e->name ? e->name : idxbuf, sz);

      if (menu_entries_append(list, idxbuf,
               rl_lbl(MENU_ENUM_LABEL_ROM_LIBRARY_ENTRY),
               MENU_ENUM_LABEL_ROM_LIBRARY_ENTRY,
               FILE_TYPE_DOWNLOAD_URL, 0, 0, NULL))
      {
         file_list_set_alt_at_offset(list, list->size - 1, disp);
         count++;
      }
   }

   return count;
}

static int rl_settings_row(file_list_t *list,
      enum msg_hash_enums lbl_enum, enum msg_hash_enums val_enum,
      const char *value)
{
   char disp[512];

   snprintf(disp, sizeof(disp), "%s: %s",
         msg_hash_to_str(val_enum),
         (value && *value) ? value : "(not set)");

   if (menu_entries_append(list, "", rl_lbl(lbl_enum), lbl_enum,
            FILE_TYPE_DOWNLOAD_URL, 0, 0, NULL))
   {
      file_list_set_alt_at_offset(list, list->size - 1, disp);
      return 1;
   }
   return 0;
}

static int rl_build_settings(file_list_t *list)
{
   rom_library_config_t *cfg = rom_library_config_get();
   int                   count = 0;

   count += rl_settings_row(list,
         MENU_ENUM_LABEL_ROM_LIBRARY_SETTING_URL,
         MENU_ENUM_LABEL_VALUE_ROM_LIBRARY_SETTING_URL, cfg->url);
   count += rl_settings_row(list,
         MENU_ENUM_LABEL_ROM_LIBRARY_SETTING_USERNAME,
         MENU_ENUM_LABEL_VALUE_ROM_LIBRARY_SETTING_USERNAME, cfg->username);
   count += rl_settings_row(list,
         MENU_ENUM_LABEL_ROM_LIBRARY_SETTING_PASSWORD,
         MENU_ENUM_LABEL_VALUE_ROM_LIBRARY_SETTING_PASSWORD,
         (cfg->password && *cfg->password) ? "********" : NULL);

   count += rl_append_info(list,
         "Note: the password is stored in clear text in rom_library.cfg.");

   return count;
}

int rom_library_menu_displaylist(void *info_ptr, unsigned type)
{
   menu_displaylist_info_t *info = (menu_displaylist_info_t*)info_ptr;
   file_list_t             *list = info->list;
   int                      count = 0;

   switch (type)
   {
      case DISPLAYLIST_ROM_LIBRARY_SYSTEMS:
         count = rl_build_systems(list);
         break;
      case DISPLAYLIST_ROM_LIBRARY_ENTRIES:
         count = rl_build_entries(list);
         break;
      case DISPLAYLIST_ROM_LIBRARY_SETTINGS:
         count = rl_build_settings(list);
         break;
      default:
         break;
   }

   if (count == 0)
      count = rl_append_info(list, "No entries");

   return count;
}

/* --------------------------------------------------------------------------
 * Entry point in the main menu
 * -------------------------------------------------------------------------- */

int rom_library_menu_append_main_entry(void *list_ptr)
{
   file_list_t *list = (file_list_t*)list_ptr;

   if (!list)
      return 0;

   if (menu_entries_append(list,
            msg_hash_to_str(MENU_ENUM_LABEL_VALUE_ROM_LIBRARY_LIST),
            rl_lbl(MENU_ENUM_LABEL_ROM_LIBRARY_LIST),
            MENU_ENUM_LABEL_ROM_LIBRARY_LIST,
            MENU_SETTING_ACTION, 0, 0, NULL))
      return 1;

   return 0;
}

/* --------------------------------------------------------------------------
 * Deferred push (navigation) — bound from menu_cbs_deferred_push hook
 * -------------------------------------------------------------------------- */

bool rom_library_menu_label_is_deferred(const char *label)
{
   if (!label)
      return false;
   return string_is_equal(label, rl_lbl(MENU_ENUM_LABEL_ROM_LIBRARY_LIST))
       || string_is_equal(label, rl_lbl(MENU_ENUM_LABEL_ROM_LIBRARY_SETTINGS))
       || string_is_equal(label,
             rl_lbl(MENU_ENUM_LABEL_DEFERRED_ROM_LIBRARY_ENTRIES));
}

int rom_library_menu_deferred_push(struct menu_displaylist_info *info_ptr)
{
   menu_displaylist_info_t        *info = (menu_displaylist_info_t*)info_ptr;
   settings_t                     *settings = config_get_ptr();
   const char                     *label = info ? info->label : NULL;
   enum menu_displaylist_ctl_state dl    = DISPLAYLIST_NONE;

   if (string_is_equal(label, rl_lbl(MENU_ENUM_LABEL_ROM_LIBRARY_LIST)))
      dl = DISPLAYLIST_ROM_LIBRARY_SYSTEMS;
   else if (string_is_equal(label, rl_lbl(MENU_ENUM_LABEL_ROM_LIBRARY_SETTINGS)))
      dl = DISPLAYLIST_ROM_LIBRARY_SETTINGS;
   else if (string_is_equal(label,
            rl_lbl(MENU_ENUM_LABEL_DEFERRED_ROM_LIBRARY_ENTRIES)))
      dl = DISPLAYLIST_ROM_LIBRARY_ENTRIES;
   else
      return -1;

   if (!menu_displaylist_ctl(dl, info, settings))
      return -1;
   menu_displaylist_process(info);
   return 0;
}

/* --------------------------------------------------------------------------
 * OK action — bound from menu_cbs_ok hook
 * -------------------------------------------------------------------------- */

bool rom_library_menu_enum_is_ours(unsigned enum_idx)
{
   switch (enum_idx)
   {
      case MENU_ENUM_LABEL_ROM_LIBRARY_LIST:
      case MENU_ENUM_LABEL_ROM_LIBRARY_SETTINGS:
      case MENU_ENUM_LABEL_DEFERRED_ROM_LIBRARY_ENTRIES:
      case MENU_ENUM_LABEL_ROM_LIBRARY_ENTRY:
      case MENU_ENUM_LABEL_ROM_LIBRARY_SETTING_URL:
      case MENU_ENUM_LABEL_ROM_LIBRARY_SETTING_USERNAME:
      case MENU_ENUM_LABEL_ROM_LIBRARY_SETTING_PASSWORD:
         return true;
      default:
         break;
   }
   return false;
}

/* OSK completion callbacks: persist the typed value, then ask for a refresh so
 * the settings page shows the new value. */
/* After a credential/URL change: persist it, refresh the settings page, and
 * kick a background refetch of the systems list so it is ready (with the new
 * connection details) once the user navigates back. */
static void rl_setting_changed(void)
{
   rom_library_config_save();
   rom_library_task_push_list_systems(rom_library_get_global());
   rom_library_menu_notify_refresh();
}

static void rl_kb_done_url(void *userdata, const char *str)
{
   if (str)
   {
      rom_library_config_set_url(str);
      rl_setting_changed();
   }
   menu_input_dialog_end();
}

static void rl_kb_done_user(void *userdata, const char *str)
{
   if (str)
   {
      rom_library_config_set_username(str);
      rl_setting_changed();
   }
   menu_input_dialog_end();
}

static void rl_kb_done_pass(void *userdata, const char *str)
{
   if (str)
   {
      rom_library_config_set_password(str);
      rl_setting_changed();
   }
   menu_input_dialog_end();
}

static int rl_open_kb(const char *prompt, const char *initial,
      input_keyboard_line_complete_t cb)
{
   menu_input_ctx_line_t line;

   memset(&line, 0, sizeof(line));
   line.label         = prompt;
   line.label_setting = "rom_library_osk";
   line.cb            = cb;

   if (!menu_input_dialog_start(&line))
      return -1;

   /* menu_input_dialog_start() begins with an empty keyboard line; seed it with
    * the current value so editing starts from the existing text. */
   if (initial && *initial)
   {
      input_driver_state_t *input_st = input_state_get_ptr();
      if (input_st)
         input_keyboard_line_append(&input_st->keyboard_line,
               initial, strlen(initial));
   }
   return 0;
}

int rom_library_menu_action_ok(
      const char *path, const char *label, unsigned type,
      size_t idx, size_t entry_idx)
{
   if (!label)
      return -1;

   /* Navigation: systems / settings pages (generic deferred push). */
   if (string_is_equal(label, rl_lbl(MENU_ENUM_LABEL_ROM_LIBRARY_LIST)))
   {
      rom_library_task_push_list_systems(rom_library_get_global());
      return generic_action_ok_displaylist_push(path, NULL, label, type,
            idx, entry_idx, ACTION_OK_DL_GENERIC);
   }

   if (string_is_equal(label, rl_lbl(MENU_ENUM_LABEL_ROM_LIBRARY_SETTINGS)))
      return generic_action_ok_displaylist_push(path, NULL, label, type,
            idx, entry_idx, ACTION_OK_DL_GENERIC);

   /* Navigation: a system row -> its ROM list (path carries the system idx). */
   if (string_is_equal(label,
            rl_lbl(MENU_ENUM_LABEL_DEFERRED_ROM_LIBRARY_ENTRIES)))
   {
      int sysidx = (path && *path) ? atoi(path) : -1;
      if (sysidx >= 0)
         rom_library_task_push_list_entries(rom_library_get_global(),
               (size_t)sysidx);
      return generic_action_ok_displaylist_push(path, NULL, label, type,
            idx, entry_idx, ACTION_OK_DL_GENERIC);
   }

   /* Settings: open the on-screen keyboard for the chosen field, pre-filled
    * with the current value. */
   {
      rom_library_config_t *cfg = rom_library_config_get();

      if (string_is_equal(label,
               rl_lbl(MENU_ENUM_LABEL_ROM_LIBRARY_SETTING_URL)))
         return rl_open_kb(
               msg_hash_to_str(MENU_ENUM_LABEL_VALUE_ROM_LIBRARY_SETTING_URL),
               cfg->url, rl_kb_done_url);
      if (string_is_equal(label,
               rl_lbl(MENU_ENUM_LABEL_ROM_LIBRARY_SETTING_USERNAME)))
         return rl_open_kb(
               msg_hash_to_str(MENU_ENUM_LABEL_VALUE_ROM_LIBRARY_SETTING_USERNAME),
               cfg->username, rl_kb_done_user);
      if (string_is_equal(label,
               rl_lbl(MENU_ENUM_LABEL_ROM_LIBRARY_SETTING_PASSWORD)))
         return rl_open_kb(
               msg_hash_to_str(MENU_ENUM_LABEL_VALUE_ROM_LIBRARY_SETTING_PASSWORD),
               cfg->password, rl_kb_done_pass);
   }

   /* A ROM row: push a streaming download into the system's local folder.
    * The current page's deferred_path carries the system index; the row path
    * carries the ROM index within that system. */
   if (string_is_equal(label, rl_lbl(MENU_ENUM_LABEL_ROM_LIBRARY_ENTRY)))
   {
      rom_library_t        *lib    = rom_library_get_global();
      const char           *dp     = rl_deferred_path();
      int                   sysidx = (dp && *dp)     ? atoi(dp)   : -1;
      int                   romidx = (path && *path) ? atoi(path) : -1;
      rom_library_system_t *sys    = (sysidx >= 0)
            ? rom_library_get_system(lib, (size_t)sysidx) : NULL;

      if (sys && romidx >= 0 && (size_t)romidx < sys->entries_count)
      {
         rom_library_entry_t *e = &sys->entries[romidx];

         /* Toggle: OK on a ROM already downloading cancels it (clean abort of
          * the .part). */
         if (rom_library_task_cancel_download((size_t)sysidx, (size_t)romidx))
            RARCH_LOG("[ROMLib] download cancel requested (sys=%d rom=%d)\n",
                  sysidx, romidx);
         /* Already downloaded (same size): don't re-fetch. A size mismatch
          * (updated/partial file) makes is_present() false, so it falls through
          * and re-downloads. */
         else if (rom_library_task_entry_is_present(sys, e))
         {
            char m[320];
            size_t l = snprintf(m, sizeof(m), "Already downloaded: %s",
                  e->name ? e->name : "ROM");
            runloop_msg_queue_push(m, l, 1, 150, true, NULL,
                  MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
            RARCH_LOG("[ROMLib] already present, skip (sys=%d rom=%d)\n",
                  sysidx, romidx);
         }
         /* One download at a time (avoids concurrent post-download tasks):
          * refuse a new one until the current download + its pipeline finish.
          * The toggle-cancel above still lets OK abort the running download. */
         else if (rom_library_task_download_busy())
         {
            const char *m = "A download is already in progress";
            runloop_msg_queue_push(m, strlen(m), 1, 150, true, NULL,
                  MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
            RARCH_LOG("[ROMLib] download refused: one already in progress "
                  "(sys=%d rom=%d)\n", sysidx, romidx);
         }
         else if (!rom_library_task_push_download(sys, (size_t)sysidx,
               e, (size_t)romidx))
            RARCH_ERR("[ROMLib] could not start download (sys=%d rom=%d)\n",
                  sysidx, romidx);
      }
      else
         RARCH_WARN("[ROMLib] ROM OK: bad index (sys=%d rom=%d)\n",
               sysidx, romidx);
      return 0;
   }

   return 0;
}
