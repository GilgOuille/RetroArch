/* RetroArch — Online ROM Library (WebDAV)
 *
 * rom_library_menu.h — Menu glue: displaylist builders and OK/deferred
 * callbacks for browsing systems -> ROMs, OSK-edited settings, download and
 * delete actions. Reached from the single balisé hooks in
 * menu/menu_displaylist.c, menu/cbs/menu_cbs_ok.c and
 * menu/cbs/menu_cbs_deferred_push.c (CLAUDE.md §2.C, §11 phase 3).
 * See rom_library/INTEGRATION.md for the exact hook list.
 *
 * The shared-file hooks only need the entry points below; the menu types are
 * resolved inside rom_library_menu.c. menu_displaylist_info_t is forward
 * declared so this header stays light for the shared files that include it.
 */

#ifndef __ROM_LIBRARY_MENU_H
#define __ROM_LIBRARY_MENU_H

#include <stddef.h>

#include <retro_common_api.h>
#include <boolean.h>

RETRO_BEGIN_DECLS

struct menu_displaylist_info;

/* Appends the top-level "Online ROM Library" entry into a main-menu file list
 * (file_list_t*). Returns the number of rows added (0 or 1). */
int rom_library_menu_append_main_entry(void *list);

/* Builds one ROM Library page into info->list, dispatching on the
 * DISPLAYLIST_ROM_LIBRARY_* value in type. info is a menu_displaylist_info_t*.
 * Returns the number of entries appended. */
int rom_library_menu_displaylist(void *info, unsigned type);

/* action_deferred_push callback: maps the pushed list label to the matching
 * DISPLAYLIST_ROM_LIBRARY_* and builds it. Bound from the menu_cbs_deferred_push
 * hook. Returns 0 on success. */
int rom_library_menu_deferred_push(struct menu_displaylist_info *info);

/* action_ok callback for every ROM Library entry (enter systems/settings,
 * enter a system's ROMs, edit a setting via OSK, select a ROM). Bound from the
 * menu_cbs_ok hook. Returns 0 on success. */
int rom_library_menu_action_ok(
      const char *path, const char *label, unsigned type,
      size_t idx, size_t entry_idx);

/* True if enum_idx (a msg_hash_enums) names a ROM Library entry whose OK action
 * we own — used by the single menu_cbs_ok hook. */
bool rom_library_menu_enum_is_ours(unsigned enum_idx);

/* True if the menu list label names one of our deferred (navigable) pages —
 * used by the single menu_cbs_deferred_push hook. */
bool rom_library_menu_label_is_deferred(const char *label);

/* Requests a rebuild of the currently displayed menu list (called from the
 * listing task callbacks, on the main thread, once data has arrived). */
void rom_library_menu_notify_refresh(void);

/* Periodic refresh hook (main thread): if the user is currently viewing a ROM
 * entries page, requests a rebuild so the in-flight download markers update.
 * Called by the download task's per-frame tick (throttled). Selection position
 * is preserved because the rebuilt list keeps the same size and the refresh
 * path does not reset the navigation pointer. No-op on any other page. */
void rom_library_menu_tick_refresh(void);

RETRO_END_DECLS

#endif /* __ROM_LIBRARY_MENU_H */
