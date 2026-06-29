/* RetroArch — Online ROM Library (WebDAV)
 *
 * rom_library_task.h — Async tasks pushed onto RetroArch's task queue:
 * PROPFIND listing, streaming download, and the post-download pipeline
 * (scan -> playlist refresh -> thumbnail). Nothing here runs on the main
 * thread (CLAUDE.md §6, §7).
 */

#ifndef __ROM_LIBRARY_TASK_H
#define __ROM_LIBRARY_TASK_H

#include <retro_common_api.h>
#include <boolean.h>

#include "rom_library.h"

RETRO_BEGIN_DECLS

/* Pushes a task that PROPFINDs the top-level collection and fills lib with the
 * list of systems. Returns false if the task could not be queued. lib is the
 * catalogue to fill (normally rom_library_get_global()). */
bool rom_library_task_push_list_systems(rom_library_t *lib);

/* Pushes a task that PROPFINDs the folder of the system at sys_index inside lib
 * and fills that system's entries. Returns false if the task could not be
 * queued (e.g. index out of range). */
bool rom_library_task_push_list_entries(
      rom_library_t *lib, size_t sys_index);

/* True while a systems / entries listing task is in flight (main-thread state,
 * used by the menu to show a "fetching…" placeholder). */
bool rom_library_task_systems_busy(void);
bool rom_library_task_entries_busy(void);

/* True while a download and its post-download pipeline (scan -> playlist
 * registration) are in flight. Only one download is allowed at a time to avoid
 * concurrent post-download tasks racing; the menu uses this to refuse a second
 * download until the current one's pipeline has completed. */
bool rom_library_task_download_busy(void);

/* Pushes a streaming download of entry within system into the system's local
 * content directory, then (on success) runs the post-download pipeline.
 * sys_index / rom_index identify the entry in the global catalogue so the
 * download can later be located (progress display, cancellation). */
bool rom_library_task_push_download(
      rom_library_system_t *system, size_t sys_index,
      const rom_library_entry_t *entry, size_t rom_index);

/* True while a download for the given catalogue entry is queued/running. */
bool rom_library_task_download_is_active(size_t sys_index, size_t rom_index);

/* Requests cancellation of the in-flight download for the given entry (clean
 * abort: the partial *.part file is removed). Returns true if such a download
 * was found and signalled. */
bool rom_library_task_cancel_download(size_t sys_index, size_t rom_index);

/* True if entry has already been fully downloaded into system's local folder,
 * either as:
 *   - the destination file itself (size must match the server's, when known —
 *     a mismatch from an updated ROM / partial leftover reports false so the
 *     download is re-triggered), or
 *   - for an archive, its extraction directory (the post-download workflow
 *     decompresses the archive into <ROM name>/ and deletes it, so only the
 *     directory remains; its presence alone counts as downloaded).
 * Used to skip redundant downloads and to flag present ROMs in the menu. */
bool rom_library_task_entry_is_present(
      const rom_library_system_t *system, const rom_library_entry_t *entry);

RETRO_END_DECLS

#endif /* __ROM_LIBRARY_TASK_H */
