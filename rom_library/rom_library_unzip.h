/* RetroArch — Online ROM Library (WebDAV)
 *
 * rom_library_unzip.h — Standalone streaming ZIP extractor.
 *
 * Exists for the same reason as rom_library_http.c (CLAUDE.md §3): the upstream
 * component cannot handle our files. libretro-common's zip backend
 * (file/archive_file_zlib.c) reads the central directory's sizes as uint32_t
 * and ignores the ZIP64 extra field, so any member larger than 4 GiB is read as
 * 0xFFFFFFFF and fails; it also mallocs the whole decompressed member in RAM
 * before writing it out, which no multi-GB ROM can afford (a PS2 DVD image is
 * ~4.4 GiB). Both are fatal for us and neither is fixable without reworking the
 * shared archive_file backend API (uint32_t csize/size, three backends, several
 * callers) — exactly the kind of transversal upstream diff §2 forbids.
 *
 * This extractor instead:
 *   - parses ZIP64 (EOCD locator/record + the 0x0001 extra field),
 *   - counts in int64_t throughout,
 *   - inflates in fixed chunks written straight to disk, so the memory budget
 *     is flat (~384 KiB) regardless of member size,
 *   - verifies CRC32, writes "<file>.part" and renames atomically,
 *   - supports cooperative cancellation and progress reporting.
 *
 * Blocking/synchronous, meant to run on a task worker thread
 * (rom_library_task.c drives it off the main thread).
 */

#ifndef __ROM_LIBRARY_UNZIP_H
#define __ROM_LIBRARY_UNZIP_H

#include <stdint.h>
#include <stddef.h>

#include <retro_common_api.h>
#include <boolean.h>

RETRO_BEGIN_DECLS

/* Progress/cancellation callback. Called as compressed bytes are consumed;
 * done/total are compressed-stream byte counts (total > 0). Return true to
 * CONTINUE, false to ABORT the extraction. */
typedef bool (*rom_library_unzip_progress_cb)(
      void *user_data, int64_t done, int64_t total);

enum rom_library_unzip_status
{
   ROM_LIBRARY_UNZIP_OK = 0,
   ROM_LIBRARY_UNZIP_ERR_PARAM,
   ROM_LIBRARY_UNZIP_ERR_NOT_ZIP,   /* Not a ZIP: caller should fall back.    */
   ROM_LIBRARY_UNZIP_ERR_FORMAT,    /* Malformed, encrypted, or unsupported
                                     * compression method.                    */
   ROM_LIBRARY_UNZIP_ERR_IO,        /* Read/write failure.                    */
   ROM_LIBRARY_UNZIP_ERR_MEM,
   ROM_LIBRARY_UNZIP_ERR_NOSPACE,   /* Pre-check: not enough free disk space. */
   ROM_LIBRARY_UNZIP_ERR_CRC,       /* CRC32 mismatch on an extracted member. */
   ROM_LIBRARY_UNZIP_ERR_CANCELLED
};

/* Cheap signature test on the first bytes of path. Lets the caller route ZIP
 * archives here and everything else (7z, ...) to task_push_decompress, without
 * paying for a full parse. */
bool rom_library_unzip_is_zip(const char *path);

/* Extracts every member of archive_path into dest_dir (created as needed,
 * including member sub-directories). Members are written to "<file>.part" and
 * renamed on success, so a cancelled or failed run leaves no half-written file
 * that would look like a valid ROM. */
enum rom_library_unzip_status rom_library_unzip_extract(
      const char *archive_path,
      const char *dest_dir,
      rom_library_unzip_progress_cb progress_cb,
      void *progress_user_data);

/* Short human-readable message for a status code (for OSD / logs). */
const char *rom_library_unzip_strerror(enum rom_library_unzip_status status);

RETRO_END_DECLS

#endif /* __ROM_LIBRARY_UNZIP_H */
