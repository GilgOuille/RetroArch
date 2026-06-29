/* RetroArch — Online ROM Library (WebDAV)
 *
 * rom_library_http.h — Standalone HTTP(S) client built directly on
 * net_socket + net_socket_ssl (NOT net_http.c), so it can stream multi-GB
 * downloads to disk with a flat, sub-1MB memory budget and 64-bit counters
 * (CLAUDE.md §6). Also issues the WebDAV PROPFIND request (parsing lives in
 * rom_library_webdav.c).
 *
 * Everything here is synchronous/blocking-style on a worker thread; the task
 * layer (rom_library_task.c) drives it off the main thread.
 */

#ifndef __ROM_LIBRARY_HTTP_H
#define __ROM_LIBRARY_HTTP_H

#include <stdint.h>
#include <stddef.h>

#include <retro_common_api.h>
#include <boolean.h>

RETRO_BEGIN_DECLS

/* Progress/cancellation callback for streaming download.
 * Called periodically with bytes received so far and the total content length
 * (-1 if unknown). Return true to CONTINUE, false to ABORT the transfer. */
typedef bool (*rom_library_http_progress_cb)(
      void *user_data, int64_t received, int64_t total);

/* Result codes for the standalone client. */
enum rom_library_http_status
{
   ROM_LIBRARY_HTTP_OK = 0,
   ROM_LIBRARY_HTTP_ERR_PARAM,
   ROM_LIBRARY_HTTP_ERR_CONNECT,
   ROM_LIBRARY_HTTP_ERR_TLS,
   ROM_LIBRARY_HTTP_ERR_HTTP,      /* Non-2xx status (see http_status).      */
   ROM_LIBRARY_HTTP_ERR_IO,        /* Disk write / read failure.             */
   ROM_LIBRARY_HTTP_ERR_NOSPACE,   /* Pre-check: not enough free disk space. */
   ROM_LIBRARY_HTTP_ERR_CANCELLED  /* Aborted via progress callback.         */
};

/* Streams a GET of url into dest_path. Writes to "<dest_path>.part" and renames
 * atomically on success. url_user/url_pass build the Basic auth header (may be
 * NULL/empty for none). On success out_http_status (optional) receives the HTTP
 * status code. */
enum rom_library_http_status rom_library_http_download(
      const char *url,
      const char *url_user,
      const char *url_pass,
      const char *dest_path,
      rom_library_http_progress_cb progress_cb,
      void *progress_user_data,
      int *out_http_status);

/* Issues a WebDAV PROPFIND (Depth: 1) against url and buffers the XML body
 * into *out_data (malloc'd, caller frees) with length *out_len. The response
 * is small (<~600 KB) so buffering is fine here (CLAUDE.md §5). */
enum rom_library_http_status rom_library_http_propfind(
      const char *url,
      const char *url_user,
      const char *url_pass,
      char **out_data,
      size_t *out_len,
      int *out_http_status);

RETRO_END_DECLS

#endif /* __ROM_LIBRARY_HTTP_H */
