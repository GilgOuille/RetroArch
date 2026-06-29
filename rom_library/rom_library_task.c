/* RetroArch — Online ROM Library (WebDAV)
 *
 * rom_library_task.c — Async listing tasks pushed onto RetroArch's task queue.
 *
 * Phase 3: the two PROPFIND listing tasks (top-level systems, and one system's
 * ROM entries). The blocking HTTPS fetch runs in the task handler (worker
 * thread); the parsed result is installed into the process-global catalogue in
 * the task callback (main thread, safe to touch menu/catalogue state). The menu
 * is then asked to refresh.
 *
 * The download task and post-download pipeline land in phases 4/5.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include <retro_common_api.h>
#include <retro_miscellaneous.h>
#include <string/stdstring.h>
#include <file/file_path.h>
#include <lists/dir_list.h>
#include <lists/string_list.h>
#include <streams/file_stream.h>
#include <features/features_cpu.h>
#include <queues/task_queue.h>
#include <queues/message_queue.h>

#include "../verbosity.h"
#include "../configuration.h"
#include "../runloop.h"
#include "../playlist.h"
#include "../manual_content_scan.h"

#include "rom_library_task.h"
#include "rom_library_config.h"
#include "rom_library_http.h"
#include "rom_library_webdav.h"
#include "rom_library_menu.h"

/* Post-download pipeline reaches two upstream task pushers. Declared here to
 * keep the heavy tasks/tasks_internal.h out of rom_library; signatures mirror
 * tasks_internal.h exactly (both are stable public task APIs). */
bool task_push_manual_content_scan(bool do_menu_refresh);
bool task_push_pl_entry_thumbnail_download(const char *system,
      playlist_t *playlist, unsigned idx, bool overwrite, bool mute);

/* Archive extraction task (tasks/task_decompress.c). Signature mirrors
 * tasks_internal.h; only zip/apk/7z (and zst when built) are handled — it
 * returns NULL for anything else, which we treat as "not an archive". */
void *task_push_decompress(
      const char *source_file, const char *target_dir,
      const char *target_file, const char *subdir, const char *valid_ext,
      retro_task_callback_t cb, void *user_data, void *frontend_userdata,
      bool mute);

/* Main-thread-only "in flight" flags (set on push, cleared in callback). */
static bool rom_library_systems_loading = false;
static bool rom_library_entries_loading = false;

/* Single-download policy: only one ROM may be in flight end-to-end, to avoid
 * concurrent post-download tasks (scan/playlist/thumbnail) racing each other.
 * The lock is taken when a download is pushed and released only once the whole
 * pipeline (download -> scan -> playlist registration) has completed; ownership
 * is handed from the download task to the post-download thumbnail watcher, which
 * clears it in its cleanup. Main-thread only (push, download callback and the
 * watcher all run there). */
static bool rom_library_download_busy = false;

bool rom_library_task_systems_busy(void) { return rom_library_systems_loading; }
bool rom_library_task_entries_busy(void) { return rom_library_entries_loading; }
bool rom_library_task_download_busy(void) { return rom_library_download_busy; }

/* Derives the scheme://host[:port] origin of a base URL (everything before the
 * first '/' that follows "://"). */
static void rom_library_url_origin(char *s, size_t len, const char *base)
{
   const char *scheme;
   const char *host;
   const char *slash;

   if (!base || !*base)
   {
      if (len)
         s[0] = '\0';
      return;
   }

   scheme = strstr(base, "://");
   if (!scheme)
   {
      strlcpy(s, base, len);
      return;
   }

   host  = scheme + 3;
   slash = strchr(host, '/');

   if (slash)
   {
      size_t n = (size_t)(slash - base);
      if (n >= len)
         n = len - 1;
      memcpy(s, base, n);
      s[n] = '\0';
   }
   else
      strlcpy(s, base, len);
}

/*********************/
/* Systems listing   */
/*********************/

typedef struct
{
   rom_library_t *result;   /* Freshly parsed catalogue (owned until callback). */
   char          *url;
   char          *user;
   char          *pass;
   bool           ok;
} rom_library_systems_state_t;

static void rom_library_systems_handler(retro_task_t *task)
{
   rom_library_systems_state_t *st = (rom_library_systems_state_t*)task->state;
   char                        *xml = NULL;
   size_t                       len = 0;
   int                          http = 0;
   enum rom_library_http_status rc;

   if (!st)
   {
      task_set_flags(task, RETRO_TASK_FLG_FINISHED, true);
      return;
   }

   rc = rom_library_http_propfind(st->url, st->user, st->pass,
         &xml, &len, &http);

   if (rc == ROM_LIBRARY_HTTP_OK && xml)
   {
      st->result = rom_library_init();
      if (st->result
            && rom_library_webdav_parse_systems(st->result, xml, len))
         st->ok = true;
   }
   else
      RARCH_WARN("[ROMLib] systems PROPFIND failed rc=%d http=%d\n",
            (int)rc, http);

   free(xml);

   task_set_progress(task, 100);
   task_set_flags(task, RETRO_TASK_FLG_FINISHED, true);
}

static void rom_library_systems_callback(retro_task_t *task,
      void *task_data, void *user_data, const char *error)
{
   rom_library_systems_state_t *st = (rom_library_systems_state_t*)task->state;

   if (st && st->ok && st->result)
   {
      rom_library_move_systems(rom_library_get_global(), st->result);
      RARCH_LOG("[ROMLib] systems loaded: %u\n",
            (unsigned)rom_library_system_count(rom_library_get_global()));
   }
   else
   {
      /* Fetch/parse failed (401, timeout, bad XML...): drop the stale cache so
       * the menu shows the "no systems" placeholder instead of outdated data. */
      rom_library_reset(rom_library_get_global());
      RARCH_WARN("[ROMLib] systems fetch failed; cleared cached list.\n");
   }

   rom_library_systems_loading = false;
   rom_library_menu_notify_refresh();
}

static void rom_library_systems_cleanup(retro_task_t *task)
{
   rom_library_systems_state_t *st = (rom_library_systems_state_t*)task->state;
   if (!st)
      return;
   rom_library_free(st->result);
   free(st->url);
   free(st->user);
   free(st->pass);
   free(st);
   task->state = NULL;
}

bool rom_library_task_push_list_systems(rom_library_t *lib)
{
   retro_task_t                *task;
   rom_library_systems_state_t *st;
   rom_library_config_t        *cfg = rom_library_config_get();

   (void)lib;

   if (rom_library_systems_loading)
      return true;

   if (!(st = (rom_library_systems_state_t*)calloc(1, sizeof(*st))))
      return false;

   st->url  = strdup((cfg->url && *cfg->url) ? cfg->url
         : ROM_LIBRARY_DEFAULT_URL);
   st->user = (cfg->username && *cfg->username) ? strdup(cfg->username) : NULL;
   st->pass = (cfg->password && *cfg->password) ? strdup(cfg->password) : NULL;

   if (!(task = task_init()))
   {
      free(st->url);
      free(st->user);
      free(st->pass);
      free(st);
      return false;
   }

   task->handler  = rom_library_systems_handler;
   task->callback = rom_library_systems_callback;
   task->cleanup  = rom_library_systems_cleanup;
   task->state    = st;
   task->title    = strdup("Fetching ROM library system list");
   task->progress = 0;

   rom_library_systems_loading = true;

   if (!task_queue_push(task))
   {
      /* task_queue_push frees the task on failure via its own path; guard the
       * flag so the menu does not get stuck on "fetching". */
      rom_library_systems_loading = false;
      return false;
   }

   return true;
}

/*********************/
/* Entries listing   */
/*********************/

typedef struct
{
   size_t               sys_index;
   char                *url;
   char                *user;
   char                *pass;
   rom_library_system_t result;   /* entries only */
   bool                 ok;
} rom_library_entries_state_t;

static void rom_library_entries_handler(retro_task_t *task)
{
   rom_library_entries_state_t *st = (rom_library_entries_state_t*)task->state;
   char                        *xml = NULL;
   size_t                       len = 0;
   int                          http = 0;
   enum rom_library_http_status rc;

   if (!st)
   {
      task_set_flags(task, RETRO_TASK_FLG_FINISHED, true);
      return;
   }

   rc = rom_library_http_propfind(st->url, st->user, st->pass,
         &xml, &len, &http);

   if (rc == ROM_LIBRARY_HTTP_OK && xml)
   {
      if (rom_library_webdav_parse_entries(&st->result, xml, len))
         st->ok = true;
   }
   else
      RARCH_WARN("[ROMLib] entries PROPFIND failed rc=%d http=%d\n",
            (int)rc, http);

   free(xml);

   task_set_progress(task, 100);
   task_set_flags(task, RETRO_TASK_FLG_FINISHED, true);
}

static void rom_library_entries_callback(retro_task_t *task,
      void *task_data, void *user_data, const char *error)
{
   rom_library_entries_state_t *st = (rom_library_entries_state_t*)task->state;

   if (st)
   {
      rom_library_system_t *sys = rom_library_get_system(
            rom_library_get_global(), st->sys_index);
      if (sys)
      {
         /* On success install the parsed entries; on failure st->result is
          * empty, so this clears the system's stale entries too. */
         rom_library_move_entries(sys, &st->result);
         if (st->ok)
            RARCH_LOG("[ROMLib] system %u entries loaded: %u\n",
                  (unsigned)st->sys_index, (unsigned)sys->entries_count);
         else
            RARCH_WARN("[ROMLib] system %u entries fetch failed; cleared.\n",
                  (unsigned)st->sys_index);
      }
   }

   rom_library_entries_loading = false;
   rom_library_menu_notify_refresh();
}

static void rom_library_entries_cleanup(retro_task_t *task)
{
   rom_library_entries_state_t *st = (rom_library_entries_state_t*)task->state;
   size_t i;
   if (!st)
      return;
   /* Free any entries the callback did not move out. */
   for (i = 0; i < st->result.entries_count; i++)
   {
      free(st->result.entries[i].name);
      free(st->result.entries[i].href);
   }
   free(st->result.entries);
   free(st->url);
   free(st->user);
   free(st->pass);
   free(st);
   task->state = NULL;
}

bool rom_library_task_push_list_entries(rom_library_t *lib, size_t sys_index)
{
   retro_task_t                *task;
   rom_library_entries_state_t *st;
   rom_library_config_t        *cfg = rom_library_config_get();
   rom_library_system_t        *sys = rom_library_get_system(
         rom_library_get_global(), sys_index);
   char                         origin[2048];
   char                         folder_url[4096];
   const char                  *href;

   (void)lib;

   if (!sys || !sys->href)
      return false;

   if (rom_library_entries_loading)
      return true;

   rom_library_url_origin(origin, sizeof(origin),
         (cfg->url && *cfg->url) ? cfg->url : ROM_LIBRARY_DEFAULT_URL);

   href = sys->href;
   if (*href == '/')
      snprintf(folder_url, sizeof(folder_url), "%s%s", origin, href);
   else
      snprintf(folder_url, sizeof(folder_url), "%s/%s", origin, href);

   if (!(st = (rom_library_entries_state_t*)calloc(1, sizeof(*st))))
      return false;

   st->sys_index = sys_index;
   st->url       = strdup(folder_url);
   st->user      = (cfg->username && *cfg->username) ? strdup(cfg->username) : NULL;
   st->pass      = (cfg->password && *cfg->password) ? strdup(cfg->password) : NULL;

   if (!(task = task_init()))
   {
      free(st->url);
      free(st->user);
      free(st->pass);
      free(st);
      return false;
   }

   task->handler  = rom_library_entries_handler;
   task->callback = rom_library_entries_callback;
   task->cleanup  = rom_library_entries_cleanup;
   task->state    = st;
   task->title    = strdup("Fetching ROM list");
   task->progress = 0;

   rom_library_entries_loading = true;

   if (!task_queue_push(task))
   {
      rom_library_entries_loading = false;
      return false;
   }

   return true;
}

/*********************/
/* Download (ph. 4)  */
/*********************/

typedef struct
{
   char                        *url;
   char                        *user;
   char                        *pass;
   char                        *dest_path;   /* final path (client writes .part) */
   char                        *label;       /* filename, for OSD messages       */
   char                        *sys_name;    /* libretro system name (playlist)  */
   size_t                       sys_index;   /* catalogue coords (immutable after */
   size_t                       rom_index;   /* push): identify this download.    */
   int                          http_status;
   enum rom_library_http_status rc;
} rom_library_download_state_t;

static void rom_library_download_handler(retro_task_t *task);

/* Locates a queued/running rom-library download by its catalogue coordinates.
 * Runs under the task queue lock (threaded mode), so it only captures the task
 * pointer — the caller cancels outside the lock to avoid re-entrant locking. */
typedef struct
{
   size_t        sys_index;
   size_t        rom_index;
   retro_task_t *found;
} rom_library_download_finder_t;

static bool rom_library_download_finder(retro_task_t *task, void *user_data)
{
   rom_library_download_finder_t *f =
         (rom_library_download_finder_t*)user_data;
   rom_library_download_state_t  *st;

   if (task->handler != rom_library_download_handler)
      return false;

   st = (rom_library_download_state_t*)task->state;
   if (!st || st->sys_index != f->sys_index || st->rom_index != f->rom_index)
      return false;

   f->found = task;
   return true;
}

bool rom_library_task_download_is_active(size_t sys_index, size_t rom_index)
{
   rom_library_download_finder_t f;
   task_finder_data_t            fd;

   f.sys_index = sys_index;
   f.rom_index = rom_index;
   f.found     = NULL;
   fd.func     = rom_library_download_finder;
   fd.userdata = &f;

   return task_queue_find(&fd);
}

bool rom_library_task_cancel_download(size_t sys_index, size_t rom_index)
{
   rom_library_download_finder_t f;
   task_finder_data_t            fd;

   f.sys_index = sys_index;
   f.rom_index = rom_index;
   f.found     = NULL;
   fd.func     = rom_library_download_finder;
   fd.userdata = &f;

   if (task_queue_find(&fd) && f.found)
   {
      task_queue_cancel_task(f.found);
      return true;
   }
   return false;
}

/* Archive formats we decompress after download. Restricted to what the build
 * can actually extract (see Makefile: HAVE_ZLIB -> zip/apk, HAVE_7ZIP -> 7z).
 * .rar is intentionally absent: RetroArch ships no RAR extractor. Listing a
 * format the build lacks is harmless — task_push_decompress then returns NULL
 * and we fall back to scanning the archive directly. */
static bool rom_library_path_is_archive(const char *path)
{
   const char *ext = (path && *path) ? path_get_extension(path) : NULL;

   if (!ext || !*ext)
      return false;

   return string_is_equal_case_insensitive(ext, "zip")
       || string_is_equal_case_insensitive(ext, "7z")
       || string_is_equal_case_insensitive(ext, "apk");
}

/* Derives the extraction directory for an archive: same folder, sub-directory
 * named after the archive minus its extension.
 * e.g. <dl>/<sys>/Game (Europe).zip -> <dl>/<sys>/Game (Europe) */
static void rom_library_archive_extract_dir(
      const char *archive_path, char *out, size_t out_len)
{
   char dir[PATH_MAX_LENGTH];
   char name[PATH_MAX_LENGTH];

   fill_pathname_basedir(dir, archive_path, sizeof(dir));
   fill_pathname_base(name, archive_path, sizeof(name));
   path_remove_extension(name);
   fill_pathname_join_special(out, dir, name, out_len);
}

/* Builds the local destination path for entry: <downloads>/<system>/<filename>.
 * Returns false if no download directory is configured. Creates nothing. MUST
 * stay in sync with the dest path built in rom_library_task_push_download(). */
static bool rom_library_entry_dest_path(
      const rom_library_system_t *system,
      const rom_library_entry_t *entry,
      char *dest, size_t dest_len)
{
   settings_t *settings = config_get_ptr();
   const char *base_dl  =
         settings ? settings->paths.directory_core_assets : NULL;
   char        dir[PATH_MAX_LENGTH];

   if (!system || !system->name || !entry || !entry->name)
      return false;
   if (!base_dl || !*base_dl)
      return false;

   fill_pathname_join_special(dir, base_dl, system->name, sizeof(dir));
   fill_pathname_join_special(dest, dir, entry->name, dest_len);
   return true;
}

bool rom_library_task_entry_is_present(
      const rom_library_system_t *system, const rom_library_entry_t *entry)
{
   char dest[PATH_MAX_LENGTH];

   if (!rom_library_entry_dest_path(system, entry, dest, sizeof(dest)))
      return false;

   /* Case 1: the downloaded file itself is still on disk (non-archive ROM, or
    * an archive not yet decompressed). When the server advertised a size,
    * require an exact match: a partial leftover or an updated ROM has a
    * different size and must be (re)fetched; unknown size (-1) -> existence. */
   if (path_is_valid(dest))
      return !(entry->size >= 0 && path_get_size(dest) != entry->size);

   /* Case 2: the archive was decompressed and removed (see the post-download
    * pipeline) — only its extraction directory remains. Its content no longer
    * matches the server file size, so directory existence alone counts as
    * present. */
   if (rom_library_path_is_archive(entry->name))
   {
      char extract_dir[PATH_MAX_LENGTH];
      rom_library_archive_extract_dir(dest, extract_dir, sizeof(extract_dir));
      if (path_is_directory(extract_dir))
         return true;
   }

   return false;
}

/* Streaming progress + cancellation, called from the worker thread by the
 * standalone HTTP client. Returns false to abort (task was cancelled). */
static bool rom_library_download_progress(
      void *user_data, int64_t received, int64_t total)
{
   retro_task_t *task = (retro_task_t*)user_data;

   if (!task)
      return true;

   if (task_get_flags(task) & RETRO_TASK_FLG_CANCELLED)
      return false;

   if (total > 0)
      task_set_progress(task, (int8_t)((received * 100) / total));
   else
      task_set_progress(task, -1);

   return true;
}

/* Per-frame progress hook, invoked on the MAIN thread by the task queue while
 * the download runs (retro_task::progress_cb). We use it to periodically
 * refresh the ROM list so the in-flight "↓" markers update live. Throttled to
 * one refresh every ROM_LIBRARY_REFRESH_INTERVAL_US across all downloads. */
#define ROM_LIBRARY_REFRESH_INTERVAL_US (1 * 1000000)

static retro_time_t rom_library_last_refresh_us = 0;

static void rom_library_download_tick(retro_task_t *task)
{
   retro_time_t now = cpu_features_get_time_usec();

   (void)task;

   if (now - rom_library_last_refresh_us < ROM_LIBRARY_REFRESH_INTERVAL_US)
      return;

   rom_library_last_refresh_us = now;
   rom_library_menu_tick_refresh();
}

static void rom_library_download_handler(retro_task_t *task)
{
   rom_library_download_state_t *st =
         (rom_library_download_state_t*)task->state;

   if (!st)
   {
      task_set_flags(task, RETRO_TASK_FLG_FINISHED, true);
      return;
   }

   st->rc = rom_library_http_download(st->url, st->user, st->pass,
         st->dest_path, rom_library_download_progress, task,
         &st->http_status);

   task_set_progress(task, 100);
   task_set_flags(task, RETRO_TASK_FLG_FINISHED, true);
}

/****************************/
/* Post-download pipeline   */
/* (scan -> playlist -> thumbnail), CLAUDE.md §7. Runs entirely on existing */
/* upstream tasks; all orchestration stays inside rom_library.              */
/****************************/

/* True if child lies inside dir: dir is a path prefix followed by a separator,
 * compared case-insensitively with '/' and '\' treated as equal (playlist
 * paths and our extraction dir may differ in slash style / case on Windows). */
static bool rom_library_path_under_dir(const char *child, const char *dir)
{
   size_t i;

   if (!child || !dir || !*dir)
      return false;

   for (i = 0; dir[i]; i++)
   {
      char a = child[i];
      char b = dir[i];

      if (!a)
         return false;
      if (a == '\\') a = '/';
      if (b == '\\') b = '/';
      if (tolower((unsigned char)a) != tolower((unsigned char)b))
         return false;
   }

   return child[i] == '/' || child[i] == '\\';
}

/* Searches an already-loaded playlist for the entry created by our scan and
 * returns its index. Two match modes:
 *   - by_dir=false: match by ARCHIVE/file name (the downloaded file was scanned
 *     as-is). "match" is a basename; the "#member" suffix of archive entries is
 *     ignored so we compare the container name (game.zip), not its inner member.
 *   - by_dir=true: the download was decompressed and the extraction directory
 *     was scanned; "match" is that directory, and any entry whose path lies
 *     inside it is the freshly imported ROM. */
static bool rom_library_playlist_find(
      playlist_t *pl, const char *match, bool by_dir, size_t *out_idx)
{
   size_t i, n;

   if (!pl || !match || !*match)
      return false;

   n = playlist_size(pl);
   for (i = 0; i < n; i++)
   {
      const struct playlist_entry *e = NULL;
      const char                  *base;
      char                         cmp[PATH_MAX_LENGTH];
      char                        *hash;

      playlist_get_index(pl, i, &e);
      if (!e || !e->path)
         continue;

      if (by_dir)
      {
         if (!rom_library_path_under_dir(e->path, match))
            continue;
         if (out_idx)
            *out_idx = i;
         return true;
      }

      /* Archive entries are stored as "<...>/game.zip#member.sfc". Use the
       * nocompression basename (cuts only at the last slash) then trim the
       * "#member" ourselves, so we compare the ARCHIVE file name (game.zip) to
       * the downloaded file. path_basename() would cut at '#' and hand back the
       * inner member (member.sfc), which never matches the .zip we fetched. */
      base = path_basename_nocompression(e->path);
      if (!base || !*base)
         continue;

      strlcpy(cmp, base, sizeof(cmp));
      if ((hash = strchr(cmp, '#')))
         *hash = '\0';

      if (string_is_equal_case_insensitive(cmp, match))
      {
         if (out_idx)
            *out_idx = i;
         return true;
      }
   }
   return false;
}

/* Loads the playlist described by cfg once and reports both its entry count
 * (*out_size) and whether our specific scanned entry is present (return value,
 * with *out_idx). The watcher uses the count to detect scan completion robustly
 * (the scan appended something) even if the precise entry match happens to fail,
 * so the single-download lock is never held on a matching edge case. */
static bool rom_library_playlist_probe(
      const playlist_config_t *cfg, const char *match, bool by_dir,
      size_t *out_size, size_t *out_idx)
{
   playlist_t *pl;
   bool        found;

   if (out_size)
      *out_size = 0;

   if (!(pl = playlist_init(cfg)))
      return false;

   if (out_size)
      *out_size = playlist_size(pl);

   found = rom_library_playlist_find(pl, match, by_dir, out_idx);
   playlist_free(pl);
   return found;
}

/* Watcher task: the manual-content-scan runs asynchronously and does not plumb
 * a completion callback back to us, so we poll the target playlist until the
 * freshly downloaded ROM appears (the scan matched its CRC to a .rdb and wrote
 * the entry), then fire a per-entry thumbnail download for it. Poll is
 * throttled and bounded so a slow scan (large-file CRC) or a no-match ROM does
 * not spin forever. */
#define ROM_LIBRARY_THUMB_POLL_US     (1   * 1000000)
#define ROM_LIBRARY_THUMB_DEADLINE_US (180 * 1000000)
/* Heartbeat: log the playlist state while waiting, so a scan that never lands an
 * entry can be told apart from a scan that is merely slow. */
#define ROM_LIBRARY_THUMB_TRACE_US    (5   * 1000000)

typedef struct
{
   char             *system;        /* libretro system name = playlist name  */
   char             *match;         /* file basename, or extraction directory */
   playlist_config_t pl_config;     /* points at <playlist_dir>/<system>.lpl  */
   retro_time_t      start_us;
   retro_time_t      last_check_us;
   retro_time_t      last_trace_us;
   size_t            idx;
   size_t            baseline_count; /* playlist size before the scan          */
   bool              by_dir;        /* match is a directory (see find())      */
   bool              found;         /* our specific entry was located          */
   bool              grew;          /* playlist gained entries (scan is done)  */
} rom_library_thumb_watch_state_t;

static void rom_library_thumb_watch_handler(retro_task_t *task)
{
   rom_library_thumb_watch_state_t *st =
         (rom_library_thumb_watch_state_t*)task->state;
   retro_time_t now;

   if (!st || (task_get_flags(task) & RETRO_TASK_FLG_CANCELLED))
   {
      task_set_flags(task, RETRO_TASK_FLG_FINISHED, true);
      return;
   }

   now = cpu_features_get_time_usec();
   if (st->start_us == 0)
      st->start_us = now;

   /* Throttle: only touch the disk once per poll interval. */
   if (st->last_check_us != 0
         && (now - st->last_check_us) < ROM_LIBRARY_THUMB_POLL_US)
      return;
   st->last_check_us = now;

   {
      size_t size = 0;
      size_t idx  = 0;
      bool   hit  = rom_library_playlist_probe(&st->pl_config, st->match,
            st->by_dir, &size, &idx);

      /* Heartbeat while the scan runs (diagnostics). */
      if ((now - st->last_trace_us) >= ROM_LIBRARY_THUMB_TRACE_US)
      {
         st->last_trace_us = now;
         RARCH_LOG("[ROMLib] waiting for scan: playlist %s has %u entries "
               "(baseline %u), elapsed %u s\n",
               st->system, (unsigned)size, (unsigned)st->baseline_count,
               (unsigned)((now - st->start_us) / 1000000));
      }

      if (hit)
      {
         /* Precise hit: our entry is in the playlist -> scan done, know idx. */
         st->found = true;
         st->idx   = idx;
         task_set_progress(task, 100);
         task_set_flags(task, RETRO_TASK_FLG_FINISHED, true);
         return;
      }

      /* Robust completion: the scan appended at least one entry even though we
       * could not match ours exactly. The pipeline is finished -> stop here so
       * the single-download lock is released promptly (never stuck for the full
       * deadline on a matching edge case). Thumbnail is best-effort. */
      if (size > st->baseline_count)
      {
         st->grew = true;
         task_set_progress(task, 100);
         task_set_flags(task, RETRO_TASK_FLG_FINISHED, true);
         return;
      }
   }

   if ((now - st->start_us) >= ROM_LIBRARY_THUMB_DEADLINE_US)
   {
      RARCH_WARN("[ROMLib] thumbnail: entry \"%s\" not found in playlist "
            "(scan slow or produced no entry); giving up.\n", st->match);
      task_set_flags(task, RETRO_TASK_FLG_FINISHED, true);
   }
}

static void rom_library_thumb_watch_callback(retro_task_t *task,
      void *task_data, void *user_data, const char *error)
{
   rom_library_thumb_watch_state_t *st =
         (rom_library_thumb_watch_state_t*)task->state;
   playlist_t *pl;
   size_t      idx = 0;

   /* Only act when the scan actually produced entries (found = our entry, grew =
    * some entry). On a bare deadline there is nothing to do; the lock is still
    * released by the cleanup. */
   if (!st || (!st->found && !st->grew))
      return;

   /* Reload and re-locate on the main thread: the loaded playlist_t must be
    * created here to hand to the (main-thread) thumbnail task. */
   if (!(pl = playlist_init(&st->pl_config)))
      return;

   if (rom_library_playlist_find(pl, st->match, st->by_dir, &idx))
   {
      RARCH_LOG("[ROMLib] scan matched \"%s\" (playlist %s, idx %u); "
            "fetching thumbnail.\n",
            st->match, st->system, (unsigned)idx);
      task_push_pl_entry_thumbnail_download(st->system, pl,
            (unsigned)idx, false, false);
   }
   else
   {
      /* Scan added entries but our precise match failed: skip the thumbnail
       * (best-effort) but log the paths so the matcher can be diagnosed. The
       * important part — releasing the download lock — happens in cleanup. */
      size_t n = playlist_size(pl);
      RARCH_WARN("[ROMLib] scan finished but entry \"%s\" (by_dir=%d) not "
            "matched in playlist %s (%u entries); thumbnail skipped.\n",
            st->match, (int)st->by_dir, st->system, (unsigned)n);
      if (n > 0)
      {
         const struct playlist_entry *e = NULL;
         playlist_get_index(pl, n - 1, &e);
         if (e && e->path)
            RARCH_WARN("[ROMLib]   last playlist path: %s\n", e->path);
      }
   }

   playlist_free(pl);

   /* Nudge the menu so a currently-open list picks the new entry/marker up. */
   rom_library_menu_tick_refresh();
}

static void rom_library_thumb_watch_cleanup(retro_task_t *task)
{
   rom_library_thumb_watch_state_t *st =
         (rom_library_thumb_watch_state_t*)task->state;

   /* End of the post-download pipeline (entry registered or watcher gave up):
    * release the single-download lock so the next ROM can be fetched. */
   rom_library_download_busy = false;

   if (!st)
      return;
   free(st->system);
   free(st->match);
   free(st);
   task->state = NULL;
}

/* content_path is either the downloaded file (scanned as-is) or, when the
 * download was an archive we decompressed, its extraction directory. */
static bool rom_library_task_push_thumbnail_watch(
      const char *system, const char *content_path)
{
   retro_task_t                    *task;
   rom_library_thumb_watch_state_t *st;
   settings_t                      *settings = config_get_ptr();
   const char                      *pldir    =
         settings ? settings->paths.directory_playlist : NULL;
   bool                             by_dir;
   const char                      *base;
   char                             pl_file[PATH_MAX_LENGTH];
   char                             pl_path[PATH_MAX_LENGTH];

   if (!system || !*system || !content_path || !pldir || !*pldir)
      return false;

   by_dir = path_is_directory(content_path);

   if (!(st = (rom_library_thumb_watch_state_t*)calloc(1, sizeof(*st))))
      return false;

   snprintf(pl_file, sizeof(pl_file), "%s.lpl", system);
   fill_pathname_join_special(pl_path, pldir, pl_file, sizeof(pl_path));

   st->system                        = strdup(system);
   st->by_dir                        = by_dir;
   if (by_dir)
      st->match                      = strdup(content_path);
   else
   {
      base = path_basename(content_path);
      st->match                      = strdup((base && *base) ? base : content_path);
   }
   st->pl_config.capacity            = COLLECTION_SIZE;
   st->pl_config.old_format          = settings->bools.playlist_use_old_format;
   st->pl_config.compress            = settings->bools.playlist_compression;
   st->pl_config.fuzzy_archive_match = settings->bools.playlist_fuzzy_archive_match;
   st->pl_config.autofix_paths       = false;
   playlist_config_set_path(&st->pl_config, pl_path);

   /* Snapshot the playlist size before the scan lands, so the watcher can tell
    * "the scan appended an entry" (size grew) from "still nothing yet". */
   {
      playlist_t *pl0 = playlist_init(&st->pl_config);
      st->baseline_count = pl0 ? playlist_size(pl0) : 0;
      if (pl0)
         playlist_free(pl0);
   }

   RARCH_LOG("[ROMLib] thumbnail watcher: match=\"%s\" by_dir=%d "
         "baseline=%u (playlist %s)\n",
         st->match, (int)by_dir, (unsigned)st->baseline_count, system);

   if (!(task = task_init()))
   {
      free(st->system);
      free(st->match);
      free(st);
      return false;
   }

   task->handler  = rom_library_thumb_watch_handler;
   task->callback = rom_library_thumb_watch_callback;
   task->cleanup  = rom_library_thumb_watch_cleanup;
   task->state    = st;
   task->title    = strdup("Waiting for ROM scan");
   task->progress = -1;
   task->flags   |= RETRO_TASK_FLG_MUTE;   /* silent: the scan task shows OSD */

   if (!task_queue_push(task))
      return false;

   return true;
}

/* Sidecar / metadata files an archive may ship next to the ROM: never content,
 * and in a LOOSE scan every listed file that reaches the playlist becomes an
 * entry, so keep them out of the extension filter below. */
static bool rom_library_ext_is_sidecar(const char *ext)
{
   static const char *skip[] = {
      "txt", "nfo", "diz", "md", "html", "htm", "xml", "log", "ini", "dat",
      "sfv", "md5", "sha1", "jpg", "jpeg", "png", "gif", "bmp", "pdf",
      "sbi", "cu2", "sub", "ccd", NULL
   };
   size_t i;

   for (i = 0; skip[i]; i++)
      if (string_is_equal_case_insensitive(ext, skip[i]))
         return true;
   return false;
}

/* Builds a '|'-separated extension list covering every content file found under
 * dir. Returns false when dir holds nothing scannable.
 *
 * Why this exists: when the scan's file_exts is left empty,
 * database_info_dir_init() does NOT list everything — it substitutes the
 * extension list of the *installed cores* (database_info.c). A system whose core
 * is not installed therefore yields an EMPTY content list, and the scan ends
 * silently without writing a playlist (no entry, no error). Feeding it the
 * extensions we just extracted makes the listing independent of which cores the
 * user happens to have. Multi-track disc images stay correct: .bin files listed
 * here are pruned by task_database_cue_prune() once the .cue is scanned. */
static bool rom_library_collect_content_exts(
      const char *dir, char *out, size_t out_len)
{
   struct string_list *files = dir_list_new(dir, NULL, false, false, false,
         true /* recursive */);
   struct string_list *exts  = string_list_new();
   size_t i;
   size_t _len               = 0;

   out[0] = '\0';

   if (files && exts)
   {
      for (i = 0; i < files->size; i++)
      {
         union string_list_elem_attr attr;
         const char *ext = path_get_extension(files->elems[i].data);

         attr.i = 0;

         if (!ext || !*ext || rom_library_ext_is_sidecar(ext))
            continue;
         if (string_list_find_elem(exts, ext))
            continue;
         string_list_append(exts, ext, attr);
      }

      for (i = 0; i < exts->size; i++)
      {
         if (_len > 0 && _len < out_len - 1)
            out[_len++] = '|';
         _len += strlcpy(out + _len, exts->elems[i].data, out_len - _len);
      }
   }

   if (files)
      string_list_free(files);
   if (exts)
      string_list_free(exts);

   return (out[0] != '\0');
}

/* Kicks off the post-download pipeline for a freshly downloaded ROM:
 *   1. LOOSE scan -> playlist entry (+ .rdb metadata match when the content is
 *      recognised, otherwise the ROM is still added by file name). rom_path is
 *      either the downloaded file (scanned as a single file) or, when the
 *      download was decompressed, its extraction directory (scanned as a dir),
 *   2. do_menu_refresh so an open list updates in place,
 *   3. per-entry thumbnail once the scan has landed the entry (watcher).
 * Called from the download success callback (main thread). Returns true when the
 * thumbnail watcher was pushed and now owns the single-download lock (it releases
 * it on completion); false on any early exit, so the caller releases the lock. */
static bool rom_library_pipeline_start(
      const char *system, const char *rom_path)
{
   settings_t *settings = config_get_ptr();
   bool        pushed;
   bool        saved_scan_without_core_match = false;
   char       *file_exts_custom;
   bool       *search_archives;
   bool       *search_recursively;
   bool       *overwrite_playlist;
   bool       *validate_entries;
   bool       *omit_db_ref;

   if (!system || !*system || !rom_path || !*rom_path)
      return false;

   /* LOOSE detection (CLAUDE.md §7): try to match the ROM against the target
    * system's database (real metadata + CRC), but ALWAYS add it to that
    * system's playlist even when nothing matches in the .rdb. The target system
    * is known from the download folder name (== libretro system name), so we
    * drive a CUSTOM scan of the single file with:
    *   - db_usage     = LOOSE            -> unmatched content is still added
    *                                        (task_database.c: NO_DB_MATCH +
    *                                        LOOSE -> MANUAL_SCAN_ITERATE_CONTENT
    *                                        appends the file to the playlist),
    *   - db_selection = SPECIFIC(system) -> match only against <system>.rdb and,
    *                                        crucially, stamp the *unmatched*
    *                                        entries with db_name = <system> so
    *                                        their thumbnails resolve under
    *                                        thumbnails/<system>/... (the menu
    *                                        derives the thumbnail folder from the
    *                                        entry's db_name, see gfx_thumbnail.c
    *                                        gfx_thumbnail_set_content_playlist),
    *   - system name  = DATABASE(system) -> every result lands in a single
    *                                        determined playlist: <system>.lpl.
    * The AUTOMATIC scan method cannot be used here: it hard-resets db_usage back
    * to STRICT in manual_content_scan_get_task_config() (so unmatched ROMs would
    * be dropped). */
   manual_content_scan_set_menu_scan_method(
         MANUAL_CONTENT_SCAN_METHOD_CUSTOM);

   if (!manual_content_scan_set_menu_content_dir(rom_path))
   {
      RARCH_WARN("[ROMLib] post-download scan: invalid content path %s\n",
            rom_path);
      return false;
   }

   manual_content_scan_set_menu_scan_use_db(MANUAL_CONTENT_SCAN_USE_DB_LOOSE);
   manual_content_scan_set_menu_scan_db_select(
         MANUAL_CONTENT_SCAN_SELECT_DB_SPECIFIC, system);
   manual_content_scan_set_menu_system_name(
         MANUAL_CONTENT_SCAN_SYSTEM_NAME_DATABASE, system);
   manual_content_scan_set_menu_core_name(
         MANUAL_CONTENT_SCAN_CORE_DETECT, NULL);

   /* Normalise the remaining shared scan_settings. This static state is also
    * driven by the interactive Manual Scan menu, so a prior visit there could
    * have left non-default values that would break our loose scan (the AUTOMATIC
    * method used to reset these for us; CUSTOM does not). */
   if ((file_exts_custom = manual_content_scan_get_file_exts_custom_ptr()))
   {
      /* Directory scan: pin the filter to the extensions actually extracted.
       * Leaving it empty would let database_info_dir_init() fall back to the
       * installed cores' extensions and list NOTHING for a system with no core
       * installed. A single file needs no filter (it is scanned as-is). */
      file_exts_custom[0] = '\0';
      if (path_is_directory(rom_path))
      {
         char exts[PATH_MAX_LENGTH];

         if (rom_library_collect_content_exts(rom_path, exts, sizeof(exts)))
         {
            strlcpy(file_exts_custom, exts,
                  manual_content_scan_get_file_exts_custom_size());
            RARCH_LOG("[ROMLib] scan file extensions: %s\n", file_exts_custom);
         }
         else
         {
            RARCH_WARN("[ROMLib] no scannable content in %s\n", rom_path);
            return false;
         }
      }
   }
   if ((search_archives = manual_content_scan_get_search_archives_ptr()))
      *search_archives = true;            /* look inside the downloaded .zip    */
   if ((search_recursively = manual_content_scan_get_search_recursively_ptr()))
      *search_recursively = true;         /* recurse when scanning an extracted */
                                          /* directory (ignored for a single    */
                                          /* file: get_task_config forces it off)*/
   if ((overwrite_playlist = manual_content_scan_get_overwrite_playlist_ptr()))
      *overwrite_playlist = false;        /* append, never wipe the playlist    */
   if ((validate_entries = manual_content_scan_get_validate_entries_ptr()))
      *validate_entries = false;          /* do not prune existing entries      */
   if ((omit_db_ref = manual_content_scan_get_omit_db_ref_ptr()))
      *omit_db_ref = false;               /* keep db_name (needed by thumbnails) */

   /* Force "Scan Without Core Match" ON for this scan so a freshly downloaded
    * ROM lands in the playlist even when no core for its system is installed
    * (mirrors Import Content > Scan Without Core Match). task_push_manual_
    * content_scan reads settings->bools.scan_without_core_match SYNCHRONOUSLY
    * (captured into the scan handle's flags at push time), so overriding it
    * just around the push and restoring straight after is safe and leaves the
    * user's persisted preference untouched. */
   if (settings)
   {
      saved_scan_without_core_match = settings->bools.scan_without_core_match;
      settings->bools.scan_without_core_match = true;
   }

   RARCH_LOG("[ROMLib] scan push: content=%s (dir=%d) system=%s "
         "loose/specific, recursive=1, archives=1, without_core_match=1\n",
         rom_path, (int)path_is_directory(rom_path), system);

   /* do_menu_refresh = true -> refresh the current menu list when the scan
    * finishes (CLAUDE.md §7.2, "refresh playlist in place"). */
   pushed = task_push_manual_content_scan(true);

   if (settings)
      settings->bools.scan_without_core_match = saved_scan_without_core_match;

   if (!pushed)
   {
      RARCH_WARN("[ROMLib] post-download scan push failed (concurrent scan "
            "on the same playlist?).\n");
      return false;
   }

   /* Hand the single-download lock to the watcher; it releases it when the
    * playlist registration is observed (or it times out). */
   return rom_library_task_push_thumbnail_watch(system, rom_path);
}

/****************************/
/* Decompression (post-DL)  */
/* Extracts a downloaded archive into a sibling directory named after the ROM  */
/* (minus extension), removes the archive, then runs the post-download         */
/* pipeline on the extracted directory. Keeps the single-download lock held    */
/* end-to-end so no second download/decompression starts meanwhile.            */
/****************************/

typedef struct
{
   char *system;         /* libretro system name (playlist target)  */
   char *archive_path;   /* downloaded archive; removed on success   */
   char *extract_dir;    /* extraction target directory              */
} rom_library_decompress_state_t;

static void rom_library_decompress_free_state(
      rom_library_decompress_state_t *st)
{
   if (!st)
      return;
   free(st->system);
   free(st->archive_path);
   free(st->extract_dir);
   free(st);
}

/* Runs on the main thread when the extraction task finishes (error != NULL on
 * failure). It owns the single-download lock and either hands it to the
 * pipeline watcher or releases it. */
static void rom_library_decompress_callback(retro_task_t *task,
      void *task_data, void *user_data, const char *error)
{
   rom_library_decompress_state_t *st =
         (rom_library_decompress_state_t*)user_data;
   bool pipeline_owns_lock = false;

   (void)task;
   (void)task_data;

   if (!st)
   {
      rom_library_download_busy = false;
      return;
   }

   if (error)
   {
      /* Extraction failed: keep the archive and fall back to scanning it
       * directly (search_archives reads inside it), so the ROM still lands in
       * the playlist rather than being lost. */
      RARCH_WARN("[ROMLib] decompress failed (%s); scanning archive %s.\n",
            error, st->archive_path);
      pipeline_owns_lock =
            rom_library_pipeline_start(st->system, st->archive_path);
   }
   else
   {
      /* Success: drop the archive (workflow: the extracted files replace it),
       * then scan the extraction directory. */
      RARCH_LOG("[ROMLib] decompressed %s -> %s; removing archive.\n",
            st->archive_path, st->extract_dir);
      filestream_delete(st->archive_path);
      pipeline_owns_lock =
            rom_library_pipeline_start(st->system, st->extract_dir);
   }

   if (!pipeline_owns_lock)
      rom_library_download_busy = false;

   rom_library_decompress_free_state(st);
   rom_library_menu_tick_refresh();
}

/* Pushes an extraction of archive_path. Returns true when the decompress task
 * was queued and now owns the single-download lock (its callback releases or
 * hands it on); false if the archive is unsupported or the task could not be
 * pushed, so the caller falls back to scanning the archive directly. */
static bool rom_library_decompress_start(
      const char *system, const char *archive_path)
{
   rom_library_decompress_state_t *st;
   char  extract_dir[PATH_MAX_LENGTH];
   void *t;

   rom_library_archive_extract_dir(archive_path, extract_dir,
         sizeof(extract_dir));

   /* Pre-create the target (the extractor also mkdir's per file, but this keeps
    * a clean, present directory even for an empty/odd archive). */
   if (!path_is_directory(extract_dir) && !path_mkdir(extract_dir))
   {
      RARCH_WARN("[ROMLib] cannot create extraction dir: %s\n", extract_dir);
      return false;
   }

   if (!(st = (rom_library_decompress_state_t*)calloc(1, sizeof(*st))))
      return false;
   st->system       = strdup(system);
   st->archive_path = strdup(archive_path);
   st->extract_dir  = strdup(extract_dir);

   /* valid_ext=NULL -> extract every member. mute=false -> show the
    * "Extracting: ..." OSD. task_push_decompress returns NULL for a format the
    * build cannot handle (e.g. .rar), which we treat as "not decompressable". */
   t = task_push_decompress(archive_path, extract_dir,
         NULL, NULL, NULL,
         rom_library_decompress_callback, st, NULL, false);

   if (!t)
   {
      RARCH_WARN("[ROMLib] archive not decompressable, scanning as-is: %s\n",
            archive_path);
      rom_library_decompress_free_state(st);
      return false;
   }

   return true;
}

static void rom_library_download_callback(retro_task_t *task,
      void *task_data, void *user_data, const char *error)
{
   rom_library_download_state_t *st =
         (rom_library_download_state_t*)task->state;
   char        msg[256];
   size_t      len;
   bool        pipeline_owns_lock = false;
   const char *name = (st && st->label) ? st->label : "ROM";

   if (st && st->rc == ROM_LIBRARY_HTTP_OK)
   {
      len = snprintf(msg, sizeof(msg), "Downloaded: %s", name);
      RARCH_LOG("[ROMLib] download OK: %s -> %s\n", name,
            st->dest_path ? st->dest_path : "?");
      /* Workflow: if the download is a known archive, decompress it first
       * (into <ROM name>/, then delete the archive); the decompress task then
       * runs the post-download pipeline. Otherwise, or if extraction cannot be
       * started, scan the downloaded file directly (phase 5 behaviour). Either
       * way the single-download lock is handed onward and released downstream. */
      if (rom_library_path_is_archive(st->dest_path)
            && rom_library_decompress_start(st->sys_name, st->dest_path))
         pipeline_owns_lock = true;
      else
         pipeline_owns_lock = rom_library_pipeline_start(st->sys_name,
               st->dest_path);
   }
   else if (st && st->rc == ROM_LIBRARY_HTTP_ERR_CANCELLED)
   {
      len = snprintf(msg, sizeof(msg), "Download cancelled: %s", name);
      RARCH_WARN("[ROMLib] download cancelled: %s\n", name);
   }
   else
   {
      len = snprintf(msg, sizeof(msg), "Download failed: %s", name);
      RARCH_WARN("[ROMLib] download failed rc=%d http=%d: %s\n",
            st ? (int)st->rc : -1, st ? st->http_status : 0, name);
   }

   runloop_msg_queue_push(msg, len, 1, 180, true, NULL,
         MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);

   /* Release the single-download lock unless the post-download pipeline took it
    * over (it then clears it when the playlist registration completes). */
   if (!pipeline_owns_lock)
      rom_library_download_busy = false;

   /* The task is already off the running list here (it was popped from the
    * finished queue before this callback), so is_active() now reports false
    * for it: refresh the ROM list to clear this ROM's "↓" marker. Crucially
    * this also covers the "last download ends" case, where no further per-frame
    * tick would fire to remove the marker. */
   rom_library_menu_tick_refresh();
}

static void rom_library_download_free_state(rom_library_download_state_t *st)
{
   if (!st)
      return;
   free(st->url);
   free(st->user);
   free(st->pass);
   free(st->dest_path);
   free(st->label);
   free(st->sys_name);
   free(st);
}

static void rom_library_download_cleanup(retro_task_t *task)
{
   rom_library_download_free_state(
         (rom_library_download_state_t*)task->state);
   task->state = NULL;
}

bool rom_library_task_push_download(
      rom_library_system_t *system, size_t sys_index,
      const rom_library_entry_t *entry, size_t rom_index)
{
   retro_task_t                 *task;
   rom_library_download_state_t *st;
   rom_library_config_t         *cfg      = rom_library_config_get();
   settings_t                   *settings = config_get_ptr();
   const char                   *base_dl  =
         settings ? settings->paths.directory_core_assets : NULL;
   char                          origin[2048];
   char                          url[4096];
   char                          dir[PATH_MAX_LENGTH];
   char                          dest[PATH_MAX_LENGTH];
   char                          title[256];
   const char                   *href;

   if (!system || !system->name || !entry || !entry->href || !entry->name)
      return false;

   if (!base_dl || !*base_dl)
   {
      const char *emsg = "ROM Library: no download directory set";
      RARCH_ERR("[ROMLib] no download directory configured "
            "(Settings > Directory > File Browser downloads).\n");
      runloop_msg_queue_push(emsg, strlen(emsg), 1, 180, true,
            NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_ERROR);
      return false;
   }

   /* Download URL = scheme://host of the base URL + the entry href (kept
    * URL-encoded exactly as PROPFIND returned it). */
   rom_library_url_origin(origin, sizeof(origin),
         (cfg->url && *cfg->url) ? cfg->url : ROM_LIBRARY_DEFAULT_URL);

   href = entry->href;
   if (*href == '/')
      snprintf(url, sizeof(url), "%s%s", origin, href);
   else
      snprintf(url, sizeof(url), "%s/%s", origin, href);

   /* Destination = <downloads>/<libretro system name>/<filename>. The system
    * folder name is the exact libretro name so the post-download scan matches
    * the right .rdb and thumbnails (CLAUDE.md §4/§9). */
   fill_pathname_join_special(dir, base_dl, system->name, sizeof(dir));
   if (!path_is_directory(dir) && !path_mkdir(dir))
   {
      RARCH_ERR("[ROMLib] cannot create destination dir: %s\n", dir);
      return false;
   }
   fill_pathname_join_special(dest, dir, entry->name, sizeof(dest));

   if (!(st = (rom_library_download_state_t*)calloc(1, sizeof(*st))))
      return false;

   st->url       = strdup(url);
   st->user      = (cfg->username && *cfg->username)
         ? strdup(cfg->username) : NULL;
   st->pass      = (cfg->password && *cfg->password)
         ? strdup(cfg->password) : NULL;
   st->dest_path = strdup(dest);
   st->label     = strdup(entry->name);
   st->sys_name  = strdup(system->name);
   st->sys_index = sys_index;
   st->rom_index = rom_index;
   st->rc        = ROM_LIBRARY_HTTP_ERR_PARAM;

   if (!(task = task_init()))
   {
      rom_library_download_free_state(st);
      return false;
   }

   snprintf(title, sizeof(title), "Downloading %s", entry->name);

   task->handler     = rom_library_download_handler;
   task->callback    = rom_library_download_callback;
   task->cleanup     = rom_library_download_cleanup;
   task->progress_cb = rom_library_download_tick;
   task->state       = st;
   task->title       = strdup(title);
   task->progress    = 0;

   if (!task_queue_push(task))
      return false;

   /* Take the single-download lock; held until the post-download pipeline ends
    * (handed to the thumbnail watcher, released in its cleanup). */
   rom_library_download_busy = true;

   return true;
}
