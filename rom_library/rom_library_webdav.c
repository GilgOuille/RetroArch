/* RetroArch — Online ROM Library (WebDAV)
 *
 * rom_library_webdav.c — PROPFIND response parsing with rxml.
 *
 * Walks the 207 Multi-Status tree produced by `rclone serve webdav` and fills
 * the catalogue (CLAUDE.md §5). The transport (the PROPFIND request + body
 * buffering, already NUL-terminated) lives in rom_library_http.c; this module
 * only owns the XML/WebDAV semantics:
 *   - element names carry a namespace prefix ("D:response"); we match on the
 *     LOCAL name so a redundant inline xmlns never trips us up;
 *   - a <D:response> may hold several <D:propstat>, and on a directory the
 *     getcontentlength lands in a separate propstat with status 404 (empty) —
 *     we only read getcontentlength when it actually carries a value;
 *   - directory vs file is decided by <D:resourcetype> containing
 *     <D:collection/>;
 *   - the first <D:response> is the requested collection itself and is skipped;
 *   - <D:href> stays URL-encoded (used to build the GET URL); <D:displayname>
 *     is already decoded and used as the label.
 */

#include <stdlib.h>
#include <string.h>

#include <retro_common_api.h>
#include <formats/rxml.h>
#include <verbosity.h>

#include "rom_library_webdav.h"

/* ---- rxml helpers (namespace-agnostic) ---------------------------------- */

/* ASCII case-insensitive compare; rclone uses lowercase local names but we
 * stay tolerant. Self-contained to keep the headless test link minimal. */
static int rl_ci_equal(const char *a, const char *b)
{
   if (!a || !b)
      return 0;
   for (; *a && *b; a++, b++)
   {
      char ca = *a, cb = *b;
      if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
      if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
      if (ca != cb)
         return 0;
   }
   return *a == '\0' && *b == '\0';
}

/* Local part of a qualified XML name ("D:response" -> "response"). */
static const char *rl_local_name(const char *name)
{
   const char *colon;
   if (!name)
      return "";
   colon = strchr(name, ':');
   return colon ? colon + 1 : name;
}

static int rl_node_is(const rxml_node_t *node, const char *local)
{
   return node && rl_ci_equal(rl_local_name(node->name), local);
}

/* Depth-first search for the first descendant matching local name. */
static rxml_node_t *rl_find(rxml_node_t *node, const char *local)
{
   rxml_node_t *c, *r;
   if (!node)
      return NULL;
   for (c = node->children; c; c = c->next)
   {
      if (rl_node_is(c, local))
         return c;
      if ((r = rl_find(c, local)))
         return r;
   }
   return NULL;
}

/* Like rl_find but only matches a node that actually carries (non-empty) data.
 * This is what lets a folder's empty 404 getcontentlength be skipped while a
 * file's real value (possibly in a later propstat) is still found. */
static const char *rl_find_data(rxml_node_t *node, const char *local)
{
   rxml_node_t *c;
   const char  *r;
   if (!node)
      return NULL;
   for (c = node->children; c; c = c->next)
   {
      if (rl_node_is(c, local) && c->data && c->data[0])
         return c->data;
      if ((r = rl_find_data(c, local)))
         return r;
   }
   return NULL;
}

/* ---- per-response extraction -------------------------------------------- */

/* Decoded label for a response: prefer <D:displayname>; fall back to the last
 * path segment of the (URL-encoded) href so we never emit an empty name.
 * Returns a pointer to the chosen label, which may be the response's own
 * displayname data (no copy) or the caller-provided buf (for the href tail).
 * Returns NULL when neither is available. */
static const char *rl_response_label(
      rxml_node_t *response, const char *href, char *buf, size_t buf_len)
{
   const char *dname = rl_find_data(response, "displayname");
   const char *slash;
   size_t      len;
   size_t      i;
   size_t      n;

   if (dname && dname[0])
      return dname;

   if (!href || !href[0] || buf_len == 0)
      return NULL;

   /* href for a folder ends with '/', so trim a trailing slash first. */
   len   = strlen(href);
   if (len > 0 && href[len - 1] == '/')
      len--;

   slash = href;
   for (i = 0; i < len; i++)
      if (href[i] == '/')
         slash = href + i + 1;

   n = (size_t)((href + len) - slash);
   if (n >= buf_len)
      n = buf_len - 1;
   memcpy(buf, slash, n);
   buf[n] = '\0';
   return buf;
}

/* ---- public parsers ----------------------------------------------------- */

bool rom_library_webdav_parse_systems(
      rom_library_t *lib, const char *xml, size_t len)
{
   rxml_document_t *doc;
   rxml_node_t     *root;
   rxml_node_t     *response;
   bool             skipped_self = false;
   size_t           added        = 0;

   (void)len; /* xml is NUL-terminated by the http layer. */

   if (!lib || !xml)
      return false;

   doc = rxml_load_document_string(xml);
   if (!doc)
   {
      RARCH_ERR("[ROMLib] PROPFIND systems: XML parse failed.\n");
      return false;
   }

   root = rxml_root_node(doc);
   if (!root)
   {
      rxml_free_document(doc);
      return false;
   }

   for (response = root->children; response; response = response->next)
   {
      const char *href;
      const char *label;
      char        labelbuf[512];

      if (!rl_node_is(response, "response"))
         continue;

      /* The first <D:response> is the requested collection itself. */
      if (!skipped_self)
      {
         skipped_self = true;
         continue;
      }

      /* Systems are folders only. */
      if (!rl_find(response, "collection"))
         continue;

      href  = rl_find_data(response, "href");
      label = rl_response_label(response, href, labelbuf, sizeof(labelbuf));
      if (!label)
         continue;

      if (rom_library_add_system(lib, label, href))
         added++;
   }

   rxml_free_document(doc);
   RARCH_LOG("[ROMLib] PROPFIND systems: %u system(s) parsed.\n",
         (unsigned)added);
   return true;
}

bool rom_library_webdav_parse_entries(
      rom_library_system_t *system, const char *xml, size_t len)
{
   rxml_document_t *doc;
   rxml_node_t     *root;
   rxml_node_t     *response;
   bool             skipped_self = false;
   size_t           added        = 0;

   (void)len;

   if (!system || !xml)
      return false;

   doc = rxml_load_document_string(xml);
   if (!doc)
   {
      RARCH_ERR("[ROMLib] PROPFIND entries: XML parse failed.\n");
      return false;
   }

   root = rxml_root_node(doc);
   if (!root)
   {
      rxml_free_document(doc);
      return false;
   }

   for (response = root->children; response; response = response->next)
   {
      const char *href;
      const char *label;
      const char *size_str;
      char        labelbuf[512];
      int64_t     size = -1;

      if (!rl_node_is(response, "response"))
         continue;

      /* The first <D:response> is the system folder itself. */
      if (!skipped_self)
      {
         skipped_self = true;
         continue;
      }

      /* Entries are files only; skip any nested collection. */
      if (rl_find(response, "collection"))
         continue;

      href  = rl_find_data(response, "href");
      label = rl_response_label(response, href, labelbuf, sizeof(labelbuf));
      if (!label)
         continue;

      /* Only honoured when present and non-empty (the 404 propstat copy is
       * empty and is therefore ignored — CLAUDE.md §5). */
      size_str = rl_find_data(response, "getcontentlength");
      if (size_str)
         size = (int64_t)strtoll(size_str, NULL, 10);

      if (rom_library_add_entry(system, label, href, size))
         added++;
   }

   rxml_free_document(doc);
   RARCH_LOG("[ROMLib] PROPFIND entries: %u file(s) parsed for '%s'.\n",
         (unsigned)added, system->name ? system->name : "?");
   return true;
}
