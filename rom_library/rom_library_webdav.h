/* RetroArch — Online ROM Library (WebDAV)
 *
 * rom_library_webdav.h — Listing via PROPFIND + rxml parsing (CLAUDE.md §5).
 *
 * The network fetch is delegated to rom_library_http.c; this module owns the
 * WebDAV/XML semantics: multiple <D:propstat> per <D:response>, the 404 on a
 * directory's getcontentlength, collection-vs-file via <D:resourcetype>,
 * skipping the self entry, and href encoding/decoding.
 */

#ifndef __ROM_LIBRARY_WEBDAV_H
#define __ROM_LIBRARY_WEBDAV_H

#include <stddef.h>

#include <retro_common_api.h>
#include <boolean.h>

#include "rom_library.h"

RETRO_BEGIN_DECLS

/* Parses a PROPFIND 207 Multi-Status XML body listing the TOP-LEVEL
 * collection into the catalogue as systems (folders only). The first
 * <D:response> (the requested collection itself) is skipped. Returns false on
 * parse failure. */
bool rom_library_webdav_parse_systems(
      rom_library_t *lib, const char *xml, size_t len);

/* Parses a PROPFIND 207 body listing one system folder, filling that system's
 * entries (files only, with size from getcontentlength when present). Returns
 * false on parse failure. */
bool rom_library_webdav_parse_entries(
      rom_library_system_t *system, const char *xml, size_t len);

RETRO_END_DECLS

#endif /* __ROM_LIBRARY_WEBDAV_H */
