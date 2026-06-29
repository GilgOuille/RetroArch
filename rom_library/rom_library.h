/* RetroArch — Online ROM Library (WebDAV)
 *
 * rom_library.h — Catalogue model (systems + entries) and global state.
 *
 * Part of the OnlineRomLibrary fork feature. All code for this feature lives
 * under rom_library/ to keep the diff against upstream master minimal
 * (see rom_library/INTEGRATION.md and CLAUDE.md §2).
 *
 * This file is provided under the same MIT-style license as the rest of the
 * RetroArch tree.
 */

#ifndef __ROM_LIBRARY_H
#define __ROM_LIBRARY_H

#include <stdint.h>
#include <stddef.h>

#include <retro_common_api.h>
#include <boolean.h>

RETRO_BEGIN_DECLS

/* A single ROM file within a system collection. */
typedef struct
{
   char    *name;  /* Decoded display name (No-Intro/Redump label).        */
   char    *href;  /* URL-encoded href exactly as returned by PROPFIND;
                    * concatenated with the base URL to build the GET URL.  */
   int64_t  size;  /* File size in bytes, or -1 when unknown.               */
} rom_library_entry_t;

/* A system collection: one WebDAV top-level folder == one libretro system.
 * The folder name is the exact libretro system name, i.e. the playlist
 * db_name (see CLAUDE.md §4). */
typedef struct
{
   char                *name;          /* libretro system name (== db_name). */
   char                *href;          /* URL-encoded href of the folder.    */
   rom_library_entry_t *entries;       /* ROM files (NULL until listed).     */
   size_t               entries_count; /* Number of valid entries.           */
} rom_library_system_t;

/* Opaque catalogue handle (systems + cached state). */
typedef struct rom_library rom_library_t;

/**************************************/
/* Initialisation / De-initialisation */
/**************************************/

/* Creates a new, empty catalogue. Returns NULL on failure. */
rom_library_t *rom_library_init(void);

/* Removes all systems/entries but keeps the handle valid. */
void rom_library_reset(rom_library_t *lib);

/* Frees a catalogue created by rom_library_init(). */
void rom_library_free(rom_library_t *lib);

/***********/
/* Getters */
/***********/

/* Number of systems currently held in the catalogue. */
size_t rom_library_system_count(rom_library_t *lib);

/* Fetches the system at index idx. Returns NULL if out of range. */
rom_library_system_t *rom_library_get_system(rom_library_t *lib, size_t idx);

/********************/
/* Process-global   */
/********************/

/* Returns the process-global catalogue, creating it on first access. The menu
 * reads it; the listing tasks fill it (all on the main thread). NULL only on
 * allocation failure. */
rom_library_t *rom_library_get_global(void);

/* Frees the process-global catalogue (shutdown). */
void rom_library_global_free(void);

/* Moves every system from src into dst (dst is reset first); src is left empty
 * but valid. Used by the listing task callback to install freshly parsed data
 * into the global catalogue without copying. */
void rom_library_move_systems(rom_library_t *dst, rom_library_t *src);

/* Moves the entries of src into dst (dst's entries are freed first); src is
 * left without entries. dst's name/href are untouched. */
void rom_library_move_entries(
      rom_library_system_t *dst, rom_library_system_t *src);

/***********/
/* Builders */
/***********/

/* Appends a system to the catalogue, copying name/href (href may be NULL).
 * Returns the new system (owned by lib), or NULL on allocation failure. */
rom_library_system_t *rom_library_add_system(
      rom_library_t *lib, const char *name, const char *href);

/* Appends a ROM entry to a system, copying name/href (href may be NULL).
 * size is the file size in bytes, or -1 when unknown. Returns the new entry
 * (owned by the system), or NULL on allocation failure. */
rom_library_entry_t *rom_library_add_entry(
      rom_library_system_t *system,
      const char *name, const char *href, int64_t size);

RETRO_END_DECLS

#endif /* __ROM_LIBRARY_H */
