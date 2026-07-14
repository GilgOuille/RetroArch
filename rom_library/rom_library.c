/* RetroArch — Online ROM Library (WebDAV)
 *
 * rom_library.c — Catalogue model implementation.
 *
 * Phase 2a: skeleton. The catalogue holds a growable array of systems, each
 * holding a growable array of ROM entries. Population (PROPFIND parsing) is
 * implemented in rom_library_webdav.c (phase 2c); this module only owns the
 * data structures and their lifetime.
 */

#include <stdlib.h>
#include <string.h>

#include <retro_common_api.h>

#include "rom_library.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__) || defined(__APPLE__) || defined(__unix__)
#include <sys/statvfs.h>
#define ROM_LIBRARY_HAVE_STATVFS 1
#endif

int64_t rom_library_disk_free(const char *dir)
{
#ifdef _WIN32
   ULARGE_INTEGER freeb;
   const char *d = (dir && *dir) ? dir : ".";
   if (GetDiskFreeSpaceExA(d, &freeb, NULL, NULL))
      return (int64_t)freeb.QuadPart;
   return -1;
#elif defined(ROM_LIBRARY_HAVE_STATVFS)
   struct statvfs vfs;
   const char *d = (dir && *dir) ? dir : ".";
   if (statvfs(d, &vfs) == 0)
      return (int64_t)vfs.f_bavail * (int64_t)vfs.f_frsize;
   return -1;
#else
   (void)dir;
   return -1;
#endif
}

struct rom_library
{
   rom_library_system_t *systems;       /* Growable array of systems. */
   size_t                systems_count; /* Valid systems.             */
};

static void rom_library_free_entry(rom_library_entry_t *entry)
{
   if (!entry)
      return;

   free(entry->name);
   free(entry->href);
   entry->name = NULL;
   entry->href = NULL;
   entry->size = -1;
}

static void rom_library_free_system(rom_library_system_t *system)
{
   size_t i;

   if (!system)
      return;

   for (i = 0; i < system->entries_count; i++)
      rom_library_free_entry(&system->entries[i]);

   free(system->entries);
   free(system->name);
   free(system->href);
   system->entries       = NULL;
   system->entries_count = 0;
   system->name          = NULL;
   system->href          = NULL;
}

rom_library_t *rom_library_init(void)
{
   rom_library_t *lib = (rom_library_t *)calloc(1, sizeof(*lib));
   return lib;
}

void rom_library_reset(rom_library_t *lib)
{
   size_t i;

   if (!lib)
      return;

   for (i = 0; i < lib->systems_count; i++)
      rom_library_free_system(&lib->systems[i]);

   free(lib->systems);
   lib->systems       = NULL;
   lib->systems_count = 0;
}

void rom_library_free(rom_library_t *lib)
{
   if (!lib)
      return;

   rom_library_reset(lib);
   free(lib);
}

size_t rom_library_system_count(rom_library_t *lib)
{
   if (!lib)
      return 0;
   return lib->systems_count;
}

rom_library_system_t *rom_library_get_system(rom_library_t *lib, size_t idx)
{
   if (!lib || idx >= lib->systems_count)
      return NULL;
   return &lib->systems[idx];
}

/* strdup that tolerates NULL (returns NULL) and reports OOM by returning NULL
 * for a non-NULL input — callers treat that as failure. */
static char *rom_library_strdup(const char *s)
{
   size_t n;
   char  *out;

   if (!s)
      return NULL;

   n   = strlen(s) + 1;
   out = (char*)malloc(n);
   if (out)
      memcpy(out, s, n);
   return out;
}

rom_library_system_t *rom_library_add_system(
      rom_library_t *lib, const char *name, const char *href)
{
   rom_library_system_t *grown;
   rom_library_system_t *sys;

   if (!lib)
      return NULL;

   grown = (rom_library_system_t*)realloc(lib->systems,
         (lib->systems_count + 1) * sizeof(*lib->systems));
   if (!grown)
      return NULL;
   lib->systems = grown;

   sys = &lib->systems[lib->systems_count];
   memset(sys, 0, sizeof(*sys));

   if (name && !(sys->name = rom_library_strdup(name)))
      return NULL;
   if (href && !(sys->href = rom_library_strdup(href)))
   {
      free(sys->name);
      sys->name = NULL;
      return NULL;
   }

   lib->systems_count++;
   return sys;
}

/********************/
/* Process-global   */
/********************/

static rom_library_t *rom_library_global = NULL;

rom_library_t *rom_library_get_global(void)
{
   if (!rom_library_global)
      rom_library_global = rom_library_init();
   return rom_library_global;
}

void rom_library_global_free(void)
{
   rom_library_free(rom_library_global);
   rom_library_global = NULL;
}

void rom_library_move_systems(rom_library_t *dst, rom_library_t *src)
{
   if (!dst || !src)
      return;

   rom_library_reset(dst);

   dst->systems       = src->systems;
   dst->systems_count = src->systems_count;
   src->systems       = NULL;
   src->systems_count = 0;
}

void rom_library_move_entries(
      rom_library_system_t *dst, rom_library_system_t *src)
{
   size_t i;

   if (!dst || !src)
      return;

   for (i = 0; i < dst->entries_count; i++)
      rom_library_free_entry(&dst->entries[i]);
   free(dst->entries);

   dst->entries       = src->entries;
   dst->entries_count = src->entries_count;
   src->entries       = NULL;
   src->entries_count = 0;
}

rom_library_entry_t *rom_library_add_entry(
      rom_library_system_t *system,
      const char *name, const char *href, int64_t size)
{
   rom_library_entry_t *grown;
   rom_library_entry_t *entry;

   if (!system)
      return NULL;

   grown = (rom_library_entry_t*)realloc(system->entries,
         (system->entries_count + 1) * sizeof(*system->entries));
   if (!grown)
      return NULL;
   system->entries = grown;

   entry = &system->entries[system->entries_count];
   memset(entry, 0, sizeof(*entry));
   entry->size = size;

   if (name && !(entry->name = rom_library_strdup(name)))
      return NULL;
   if (href && !(entry->href = rom_library_strdup(href)))
   {
      free(entry->name);
      entry->name = NULL;
      return NULL;
   }

   system->entries_count++;
   return entry;
}
