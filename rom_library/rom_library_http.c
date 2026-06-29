/* RetroArch — Online ROM Library (WebDAV)
 *
 * rom_library_http.c — Standalone streaming HTTP(S) client.
 *
 * Built directly on the public net_socket + net_socket_ssl APIs (NOT
 * net_http.c) so it can stream multi-GB downloads to disk with a flat,
 * sub-1MB memory budget and 64-bit counters (CLAUDE.md §6):
 *   connect (socket_init/next) -> TLS handshake (ssl_socket_connect) ->
 *   send request line + Host + Authorization: Basic + Connection: close ->
 *   parse status line + headers -> stream body to "<dest>.part" with a fixed
 *   ~256 KiB reusable buffer, int64 counters, disk-space pre-check, atomic
 *   rename, cancel-safe.
 *
 * The same core also serves the WebDAV PROPFIND (small buffered response;
 * parsing lives in rom_library_webdav.c).
 *
 * Everything here is synchronous/blocking-style; the task layer
 * (rom_library_task.c) drives it off the main thread.
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <stdio.h>

#include <retro_common_api.h>
#include <retro_miscellaneous.h>
#include <boolean.h>
#include <libretro.h>

#include <encodings/base64.h>
#include <string/stdstring.h>
#include <streams/file_stream.h>
#include <file/file_path.h>
#include <net/net_compat.h>
#include <net/net_socket.h>
#include <net/net_socket_ssl.h>

#include "verbosity.h"

#include "rom_library_http.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__) || defined(__APPLE__) || defined(__unix__)
#include <sys/statvfs.h>
#define ROM_LIBRARY_HAVE_STATVFS 1
#endif

/* Fixed reception buffer: the whole point of this client. Reused for every
 * chunk so the memory budget stays flat regardless of file size. */
#define RL_HTTP_RECV_BUF      (256 * 1024)
/* Upper bound on the response-header block. WebDAV/HTTP headers are tiny;
 * anything bigger is a malformed/hostile response. */
#define RL_HTTP_HDR_MAX       (64 * 1024)
/* Per-read inactivity timeout (ms). The server is ours and on a LAN/WAN we
 * control; 30 s is generous and still bounds a dead connection. */
#define RL_HTTP_TIMEOUT_MS    30000
#define RL_HTTP_USER_AGENT    "RetroArch-ROMLibrary/1.0"
/* Safety cap for buffered (PROPFIND) responses: 32 MiB. The real responses
 * are <~600 KB (CLAUDE.md §5); this only guards against a runaway server. */
#define RL_HTTP_BUF_MAX       (32u * 1024u * 1024u)

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

/* Case-insensitive prefix test. Returns 0 when s starts with prefix. */
static int rl_ci_prefix(const char *s, const char *prefix)
{
   while (*prefix)
   {
      int a = tolower((unsigned char)*s++);
      int b = tolower((unsigned char)*prefix++);
      if (a != b)
         return 1;
   }
   return 0;
}

/* Locate needle (length nlen) inside hay (length hlen). */
static const uint8_t *rl_memfind(const uint8_t *hay, size_t hlen,
      const char *needle, size_t nlen)
{
   size_t i;
   if (nlen == 0 || hlen < nlen)
      return NULL;
   for (i = 0; i + nlen <= hlen; i++)
   {
      if (hay[i] == (uint8_t)needle[0]
            && memcmp(hay + i, needle, nlen) == 0)
         return hay + i;
   }
   return NULL;
}

/* Free disk space (bytes) for the volume holding dir, or -1 if unknown. */
static int64_t rl_disk_free(const char *dir)
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

/* Builds "Authorization: Basic <base64(user:pass)>\r\n" (malloc'd), or NULL
 * when no credentials are supplied. Mirrors webdav.c's builder. */
static char *rl_http_basic_auth(const char *user, const char *pass)
{
   char    userpass[768];
   char   *b64;
   char   *result = NULL;
   int     flen   = 0;
   size_t  _len   = 0;
   size_t  total;

   if ((!user || !*user) && (!pass || !*pass))
      return NULL;

   if (user && *user)
      _len += strlcpy(userpass + _len, user, sizeof(userpass) - _len);
   userpass[_len++] = ':';
   if (pass && *pass)
      _len += strlcpy(userpass + _len, pass, sizeof(userpass) - _len);
   userpass[_len]   = '\0';

   if (!(b64 = base64(userpass, (int)_len, &flen)))
      return NULL;

   total  = STRLEN_CONST("Authorization: Basic ") + strlen(b64)
          + STRLEN_CONST("\r\n") + 1;
   if ((result = (char*)malloc(total)))
      snprintf(result, total, "Authorization: Basic %s\r\n", b64);

   free(b64);
   return result;
}

/* Splits an http(s) URL into scheme/host/port/path. Returns false on a URL we
 * cannot parse. path always begins with '/'. */
static bool rl_http_url_split(const char *url,
      bool *ssl, char *host, size_t host_len,
      unsigned *port, char *path, size_t path_len)
{
   const char *p;
   const char *host_start;
   const char *host_end;
   const char *colon;
   const char *slash;

   if (!url || !host || !port || !path)
      return false;

   if (!rl_ci_prefix(url, "https://"))
   {
      *ssl  = true;
      *port = 443;
      p     = url + STRLEN_CONST("https://");
   }
   else if (!rl_ci_prefix(url, "http://"))
   {
      *ssl  = false;
      *port = 80;
      p     = url + STRLEN_CONST("http://");
   }
   else
      return false;

   host_start = p;
   slash      = strchr(p, '/');
   host_end   = slash ? slash : (p + strlen(p));

   /* Optional :port lives between host_start and host_end. */
   for (colon = host_start; colon < host_end; colon++)
      if (*colon == ':')
         break;

   if (colon < host_end)
   {
      size_t hl = (size_t)(colon - host_start);
      if (hl == 0 || hl >= host_len)
         return false;
      memcpy(host, host_start, hl);
      host[hl] = '\0';
      *port    = (unsigned)strtoul(colon + 1, NULL, 10);
      if (*port == 0)
         return false;
   }
   else
   {
      size_t hl = (size_t)(host_end - host_start);
      if (hl == 0 || hl >= host_len)
         return false;
      memcpy(host, host_start, hl);
      host[hl] = '\0';
   }

   if (slash)
      strlcpy(path, slash, path_len);
   else
      strlcpy(path, "/", path_len);

   return true;
}

/* Parses "HTTP/1.x NNN ..." -> NNN, or -1. */
static int rl_http_parse_status(const char *hdr)
{
   const char *sp = strchr(hdr, ' ');
   if (!sp)
      return -1;
   return (int)strtol(sp + 1, NULL, 10);
}

/* Finds the Content-Length header value, or -1 if absent. */
static int64_t rl_http_parse_content_length(const char *hdr)
{
   const char *p = hdr;
   while (p && *p)
   {
      if (!rl_ci_prefix(p, "Content-Length:"))
      {
         p += STRLEN_CONST("Content-Length:");
         while (*p == ' ' || *p == '\t')
            p++;
         return (int64_t)strtoll(p, NULL, 10);
      }
      if ((p = strchr(p, '\n')))
         p++;
   }
   return -1;
}

/* True if the response uses "Transfer-Encoding: chunked". rclone serves the
 * (dynamically generated) PROPFIND body chunked; static file GETs come with a
 * Content-Length instead (CLAUDE.md §6). */
static bool rl_http_is_chunked(const char *hdr)
{
   const char *p = hdr;
   while (p && *p)
   {
      if (!rl_ci_prefix(p, "Transfer-Encoding:"))
      {
         const char *eol = strchr(p, '\n');
         p += STRLEN_CONST("Transfer-Encoding:");
         while (*p && p != eol)
         {
            if (!rl_ci_prefix(p, "chunked"))
               return true;
            p++;
         }
         return false;
      }
      if ((p = strchr(p, '\n')))
         p++;
   }
   return false;
}

/* ------------------------------------------------------------------------- */
/* Connection wrapper (SSL and plain share one type)                         */
/* ------------------------------------------------------------------------- */

typedef struct
{
   bool  ssl;
   int   fd;
   void *ssl_ctx;
} rl_conn_t;

static bool rl_conn_open(const char *host, unsigned port, bool ssl,
      rl_conn_t *conn)
{
   struct addrinfo *addr_head = NULL;
   struct addrinfo *addr_iter = NULL;
   int              fd;

   conn->ssl     = ssl;
   conn->fd      = -1;
   conn->ssl_ctx = NULL;

   fd = socket_init((void**)&addr_iter, (uint16_t)port, host,
         SOCKET_TYPE_STREAM, 0);
   if (fd < 0 || !addr_iter)
   {
      if (addr_iter)
         freeaddrinfo_retro(addr_iter);
      return false;
   }
   addr_head = addr_iter;

   for (; fd >= 0; fd = socket_next((void**)&addr_iter))
   {
      if (ssl)
      {
         void *ctx = ssl_socket_init(fd, host);
         if (!ctx)
         {
            socket_close(fd);
            continue;
         }
         /* timeout_enable=true (5s connect), nonblock=true: matches the
          * proven net_http.c path. */
         if (ssl_socket_connect(ctx, addr_iter, true, true) < 0)
         {
            ssl_socket_close(ctx); /* closes fd */
            ssl_socket_free(ctx);
            continue;
         }
         conn->fd      = fd;
         conn->ssl_ctx = ctx;
         freeaddrinfo_retro(addr_head);
         return true;
      }

      if (socket_connect_with_timeout(fd, addr_iter, RL_HTTP_TIMEOUT_MS))
      {
         conn->fd = fd;
         freeaddrinfo_retro(addr_head);
         return true;
      }
      socket_close(fd);
   }

   freeaddrinfo_retro(addr_head);
   return false;
}

static bool rl_conn_send(rl_conn_t *conn, const void *data, size_t len)
{
   if (conn->ssl)
      return ssl_socket_send_all_blocking(conn->ssl_ctx, data, len, true);
   return socket_send_all_blocking(conn->fd, data, len, true);
}

static ssize_t rl_conn_recv(rl_conn_t *conn, void *buf, size_t len, bool *err)
{
   if (conn->ssl)
      return ssl_socket_receive_all_nonblocking(conn->ssl_ctx, err, buf, len);
   return socket_receive_all_nonblocking(conn->fd, err, buf, len);
}

/* Blocks until the socket is readable. Returns false on timeout/error. */
static bool rl_conn_wait_readable(rl_conn_t *conn, int timeout_ms)
{
   bool rd = true, wr = false;
   if (!socket_wait(conn->fd, &rd, &wr, timeout_ms))
      return false;
   return rd;
}

static void rl_conn_close(rl_conn_t *conn)
{
   if (conn->ssl)
   {
      if (conn->ssl_ctx)
      {
         ssl_socket_close(conn->ssl_ctx);
         ssl_socket_free(conn->ssl_ctx);
      }
   }
   else if (conn->fd >= 0)
      socket_close(conn->fd);

   conn->fd      = -1;
   conn->ssl_ctx = NULL;
}

/* ------------------------------------------------------------------------- */
/* Generic perform: connect -> request -> headers -> body sink               */
/* ------------------------------------------------------------------------- */

/* on_headers: inspect status/total once headers are parsed. Return
 * ROM_LIBRARY_HTTP_OK to proceed, or an error code to abort.
 * on_body: consume a body chunk. Return false to signal a write failure. */
typedef enum rom_library_http_status (*rl_on_headers_fn)(
      void *ctx, int status, int64_t total);
typedef bool (*rl_on_body_fn)(void *ctx, const uint8_t *data, size_t len);

/* ------------------------------------------------------------------------- */
/* Chunked transfer-encoding decoder (incremental state machine)             */
/* ------------------------------------------------------------------------- */

enum rl_chunk_state
{
   CK_SIZE = 0,    /* reading hex chunk-size digits           */
   CK_SIZE_EXT,    /* skipping a ";chunk-extension" to CR     */
   CK_SIZE_LF,     /* expecting LF after the size line CR     */
   CK_DATA,        /* copying chunk payload                   */
   CK_DATA_CR,     /* expecting CR after a chunk's payload    */
   CK_DATA_LF,     /* expecting LF after that CR              */
   CK_TRAILER_CR,  /* at start of a line in the trailer block */
   CK_TRAILER_SKIP,/* skipping a non-empty trailer line       */
   CK_TRAILER_LF   /* expecting LF of the final empty line    */
};

typedef struct
{
   enum rl_chunk_state state;
   int64_t             remaining; /* bytes left in the current chunk      */
   bool                done;      /* terminating 0-size chunk seen        */
   bool                error;     /* malformed framing                    */
} rl_chunk_t;

/* Feeds raw bytes through the decoder, emitting decoded payload to on_body.
 * Returns false on a sink failure or malformed framing. */
static bool rl_chunk_feed(rl_chunk_t *ck, const uint8_t *in, size_t inlen,
      rl_on_body_fn on_body, void *ctx, int64_t *emitted)
{
   size_t i = 0;

   while (i < inlen && !ck->done)
   {
      uint8_t c = in[i];
      switch (ck->state)
      {
         case CK_SIZE:
            if (c >= '0' && c <= '9')
               { ck->remaining = ck->remaining * 16 + (c - '0');      i++; }
            else if (c >= 'a' && c <= 'f')
               { ck->remaining = ck->remaining * 16 + (c - 'a' + 10); i++; }
            else if (c >= 'A' && c <= 'F')
               { ck->remaining = ck->remaining * 16 + (c - 'A' + 10); i++; }
            else if (c == '\r') { ck->state = CK_SIZE_LF;  i++; }
            else if (c == ';')  { ck->state = CK_SIZE_EXT; i++; }
            else { ck->error = true; return false; }
            break;
         case CK_SIZE_EXT:
            if (c == '\r')
               ck->state = CK_SIZE_LF;
            i++;
            break;
         case CK_SIZE_LF:
            if (c != '\n') { ck->error = true; return false; }
            i++;
            ck->state = (ck->remaining == 0) ? CK_TRAILER_CR : CK_DATA;
            break;
         case CK_DATA:
            {
               size_t avail = inlen - i;
               size_t take  = (ck->remaining < (int64_t)avail)
                            ? (size_t)ck->remaining : avail;
               if (take)
               {
                  if (!on_body(ctx, in + i, take))
                     return false;
                  *emitted      += (int64_t)take;
                  i             += take;
                  ck->remaining -= (int64_t)take;
               }
               if (ck->remaining == 0)
                  ck->state = CK_DATA_CR;
            }
            break;
         case CK_DATA_CR:
            if (c != '\r') { ck->error = true; return false; }
            i++; ck->state = CK_DATA_LF;
            break;
         case CK_DATA_LF:
            if (c != '\n') { ck->error = true; return false; }
            i++; ck->state = CK_SIZE; ck->remaining = 0;
            break;
         case CK_TRAILER_CR:
            if (c == '\r') { ck->state = CK_TRAILER_LF; i++; }
            else             ck->state = CK_TRAILER_SKIP; /* trailer header */
            break;
         case CK_TRAILER_SKIP:
            if (c == '\n')
               ck->state = CK_TRAILER_CR;
            i++;
            break;
         case CK_TRAILER_LF:
            if (c != '\n') { ck->error = true; return false; }
            i++; ck->done = true;
            break;
         default:
            ck->error = true;
            return false;
      }
   }

   return true;
}

static enum rom_library_http_status rl_http_perform(
      const char *method,
      const char *url,
      const char *user,
      const char *pass,
      const char *extra_headers, /* may be NULL; each line ends with \r\n */
      const char *req_body,      /* may be NULL */
      size_t req_body_len,
      rl_on_headers_fn on_headers,
      rl_on_body_fn on_body,
      void *ctx,
      rom_library_http_progress_cb progress_cb,
      void *progress_user_data,
      int *out_http_status,
      int64_t *out_total)
{
   enum rom_library_http_status rc = ROM_LIBRARY_HTTP_ERR_HTTP;
   rl_conn_t conn;
   bool      ssl  = false;
   unsigned  port = 0;
   char      host[256];
   char      path[PATH_MAX_LENGTH];
   char      hostport[300];
   char     *auth = NULL;
   char     *req  = NULL;
   size_t    req_cap;
   size_t    req_len = 0;
   uint8_t  *headbuf = NULL;
   uint8_t  *recvbuf = NULL;
   size_t    headlen = 0;
   size_t    hdr_end = 0;
   bool      header_done = false;
   int        status = -1;
   int64_t    total  = -1;
   int64_t    received = 0;
   bool       chunked  = false;
   rl_chunk_t ck;

   conn.ssl     = false;
   conn.fd      = -1;
   conn.ssl_ctx = NULL;
   memset(&ck, 0, sizeof(ck));
   ck.state = CK_SIZE;

   if (out_total)
      *out_total = -1;

   if (!rl_http_url_split(url, &ssl, host, sizeof(host),
            &port, path, sizeof(path)))
      return ROM_LIBRARY_HTTP_ERR_PARAM;

   auth = rl_http_basic_auth(user, pass);

   if (!(recvbuf = (uint8_t*)malloc(RL_HTTP_RECV_BUF))
         || !(headbuf = (uint8_t*)malloc(RL_HTTP_HDR_MAX)))
   {
      rc = ROM_LIBRARY_HTTP_ERR_PARAM;
      goto done;
   }

   /* --- connect ---------------------------------------------------------- */
   RARCH_LOG("[ROMLib] %s %s://%s:%u%s\n", method, ssl ? "https" : "http",
         host, port, path);

   if (!rl_conn_open(host, port, ssl, &conn))
   {
      RARCH_ERR("[ROMLib] Connection to %s:%u failed.\n", host, port);
      rc = ROM_LIBRARY_HTTP_ERR_CONNECT;
      goto done;
   }

   /* --- build + send request -------------------------------------------- */
   if (((ssl && port == 443) || (!ssl && port == 80)))
      strlcpy(hostport, host, sizeof(hostport));
   else
      snprintf(hostport, sizeof(hostport), "%s:%u", host, port);

   req_cap = strlen(method) + strlen(path) + strlen(hostport)
           + (auth ? strlen(auth) : 0)
           + (extra_headers ? strlen(extra_headers) : 0)
           + 256;
   if (!(req = (char*)malloc(req_cap)))
   {
      rc = ROM_LIBRARY_HTTP_ERR_PARAM;
      goto done;
   }

   req_len += snprintf(req + req_len, req_cap - req_len,
         "%s %s HTTP/1.1\r\n", method, path);
   req_len += snprintf(req + req_len, req_cap - req_len,
         "Host: %s\r\n", hostport);
   req_len += snprintf(req + req_len, req_cap - req_len,
         "User-Agent: %s\r\n", RL_HTTP_USER_AGENT);
   req_len += snprintf(req + req_len, req_cap - req_len, "Accept: */*\r\n");
   if (auth)
      req_len += snprintf(req + req_len, req_cap - req_len, "%s", auth);
   if (extra_headers)
      req_len += snprintf(req + req_len, req_cap - req_len, "%s", extra_headers);
   if (req_body)
      req_len += snprintf(req + req_len, req_cap - req_len,
            "Content-Length: %u\r\n", (unsigned)req_body_len);
   req_len += snprintf(req + req_len, req_cap - req_len,
         "Connection: close\r\n\r\n");

   if (!rl_conn_send(&conn, req, req_len))
   {
      rc = ROM_LIBRARY_HTTP_ERR_IO;
      goto done;
   }
   if (req_body && req_body_len
         && !rl_conn_send(&conn, req_body, req_body_len))
   {
      rc = ROM_LIBRARY_HTTP_ERR_IO;
      goto done;
   }

   /* --- receive + parse headers ----------------------------------------- */
   while (!header_done)
   {
      bool    neterr = false;
      ssize_t n      = rl_conn_recv(&conn, recvbuf, RL_HTTP_RECV_BUF, &neterr);

      if (n > 0)
      {
         const uint8_t *term;
         if (headlen + (size_t)n > RL_HTTP_HDR_MAX)
         {
            rc = ROM_LIBRARY_HTTP_ERR_HTTP;
            goto done;
         }
         memcpy(headbuf + headlen, recvbuf, (size_t)n);
         headlen += (size_t)n;

         if ((term = rl_memfind(headbuf, headlen, "\r\n\r\n", 4)))
         {
            hdr_end     = (size_t)(term - headbuf) + 4;
            header_done = true;
         }
      }
      else if (n == 0)
      {
         if (!rl_conn_wait_readable(&conn, RL_HTTP_TIMEOUT_MS))
         {
            rc = ROM_LIBRARY_HTTP_ERR_IO;
            goto done;
         }
      }
      else /* n < 0: connection closed before headers completed */
      {
         rc = ROM_LIBRARY_HTTP_ERR_IO;
         goto done;
      }
   }

   /* NUL-terminate the header region (overwrites the first \r of the
    * terminator; body bytes after hdr_end are untouched). */
   headbuf[hdr_end - 4] = '\0';
   status  = rl_http_parse_status((char*)headbuf);
   total   = rl_http_parse_content_length((char*)headbuf);
   chunked = rl_http_is_chunked((char*)headbuf);
   if (chunked)
      total = -1; /* length is implied by the chunk framing */

   RARCH_LOG("[ROMLib] HTTP %d, Content-Length %lld%s\n",
         status, (long long)total, chunked ? " (chunked)" : "");

   if (out_http_status)
      *out_http_status = status;
   if (out_total)
      *out_total = total;

   if ((rc = on_headers(ctx, status, total)) != ROM_LIBRARY_HTTP_OK)
      goto done;

   /* --- leftover body bytes already in headbuf -------------------------- */
   if (headlen > hdr_end)
   {
      const uint8_t *p    = headbuf + hdr_end;
      size_t         plen = headlen - hdr_end;

      if (chunked)
      {
         int64_t emitted = 0;
         if (!rl_chunk_feed(&ck, p, plen, on_body, ctx, &emitted))
         {
            rc = ROM_LIBRARY_HTTP_ERR_IO;
            goto done;
         }
         received += emitted;
      }
      else
      {
         if (!on_body(ctx, p, plen))
         {
            rc = ROM_LIBRARY_HTTP_ERR_IO;
            goto done;
         }
         received += (int64_t)plen;
      }

      if (progress_cb
            && !progress_cb(progress_user_data, received, total))
      {
         rc = ROM_LIBRARY_HTTP_ERR_CANCELLED;
         goto done;
      }
   }

   /* --- stream the remaining body --------------------------------------- */
   while (chunked ? !ck.done : (total < 0 || received < total))
   {
      bool    neterr = false;
      size_t  want   = RL_HTTP_RECV_BUF;
      ssize_t n;

      if (!chunked && total >= 0)
      {
         int64_t remaining = total - received;
         if ((int64_t)want > remaining)
            want = (size_t)remaining;
      }

      n = rl_conn_recv(&conn, recvbuf, want, &neterr);

      if (n > 0)
      {
         if (chunked)
         {
            int64_t emitted = 0;
            if (!rl_chunk_feed(&ck, recvbuf, (size_t)n, on_body, ctx,
                     &emitted))
            {
               rc = ROM_LIBRARY_HTTP_ERR_IO;
               goto done;
            }
            received += emitted;
         }
         else
         {
            if (!on_body(ctx, recvbuf, (size_t)n))
            {
               rc = ROM_LIBRARY_HTTP_ERR_IO;
               goto done;
            }
            received += n;
         }

         if (progress_cb
               && !progress_cb(progress_user_data, received, total))
         {
            rc = ROM_LIBRARY_HTTP_ERR_CANCELLED;
            goto done;
         }
      }
      else if (n == 0)
      {
         if (!rl_conn_wait_readable(&conn, RL_HTTP_TIMEOUT_MS))
         {
            rc = ROM_LIBRARY_HTTP_ERR_IO;
            goto done;
         }
      }
      else /* n < 0 */
      {
         if (!chunked && total < 0)
            break; /* EOF with unknown length => body complete */
         rc = ROM_LIBRARY_HTTP_ERR_IO; /* premature close */
         goto done;
      }
   }

   if (chunked && ck.error)
   {
      rc = ROM_LIBRARY_HTTP_ERR_IO;
      goto done;
   }

   rc = ROM_LIBRARY_HTTP_OK;

done:
   rl_conn_close(&conn);
   free(req);
   free(auth);
   free(headbuf);
   free(recvbuf);
   return rc;
}

/* ------------------------------------------------------------------------- */
/* Download sink: stream to "<dest>.part", atomic rename on success          */
/* ------------------------------------------------------------------------- */

typedef struct
{
   const char *dest_path;
   char        part_path[PATH_MAX_LENGTH];
   RFILE      *fp;
   int64_t     total;
} rl_dl_ctx_t;

static enum rom_library_http_status rl_dl_on_headers(
      void *c, int status, int64_t total)
{
   rl_dl_ctx_t *d = (rl_dl_ctx_t*)c;
   char         basedir[PATH_MAX_LENGTH];
   int64_t      freeb;

   if (status != 200 && status != 206)
      return ROM_LIBRARY_HTTP_ERR_HTTP;
   /* A real download needs a length: it drives the disk pre-check and the
    * progress bar (CLAUDE.md §6). The server always sends one. */
   if (total < 0)
      return ROM_LIBRARY_HTTP_ERR_HTTP;

   d->total = total;

   fill_pathname_basedir(basedir, d->dest_path, sizeof(basedir));
   if (!string_is_empty(basedir) && !path_is_directory(basedir))
      path_mkdir(basedir);

   freeb = rl_disk_free(basedir);
   if (freeb >= 0 && freeb < total)
   {
      RARCH_ERR("[ROMLib] Not enough disk space: need %lld, have %lld.\n",
            (long long)total, (long long)freeb);
      return ROM_LIBRARY_HTTP_ERR_NOSPACE;
   }

   snprintf(d->part_path, sizeof(d->part_path), "%s.part", d->dest_path);
   d->fp = filestream_open(d->part_path,
         RETRO_VFS_FILE_ACCESS_WRITE,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (!d->fp)
   {
      RARCH_ERR("[ROMLib] Cannot open for writing: %s\n", d->part_path);
      return ROM_LIBRARY_HTTP_ERR_IO;
   }

   return ROM_LIBRARY_HTTP_OK;
}

static bool rl_dl_on_body(void *c, const uint8_t *data, size_t len)
{
   rl_dl_ctx_t *d = (rl_dl_ctx_t*)c;
   if (!d->fp)
      return false;
   if (filestream_write(d->fp, data, (int64_t)len) != (int64_t)len)
      return false;
   return true;
}

enum rom_library_http_status rom_library_http_download(
      const char *url,
      const char *url_user,
      const char *url_pass,
      const char *dest_path,
      rom_library_http_progress_cb progress_cb,
      void *progress_user_data,
      int *out_http_status)
{
   rl_dl_ctx_t                  ctx;
   enum rom_library_http_status rc;
   int                          http_status = 0;
   int64_t                      total       = 0;

   if (out_http_status)
      *out_http_status = 0;
   if (!url || !*url || !dest_path || !*dest_path)
      return ROM_LIBRARY_HTTP_ERR_PARAM;

   memset(&ctx, 0, sizeof(ctx));
   ctx.dest_path = dest_path;

   rc = rl_http_perform("GET", url, url_user, url_pass, NULL, NULL, 0,
         rl_dl_on_headers, rl_dl_on_body, &ctx,
         progress_cb, progress_user_data, &http_status, &total);

   if (out_http_status)
      *out_http_status = http_status;

   if (ctx.fp)
   {
      filestream_close(ctx.fp);
      ctx.fp = NULL;
   }

   if (rc == ROM_LIBRARY_HTTP_OK)
   {
      /* Atomic publish: drop any stale dest first (Windows rename() will not
       * overwrite), then rename "<dest>.part" -> "<dest>". */
      filestream_delete(dest_path);
      if (filestream_rename(ctx.part_path, dest_path) != 0)
      {
         RARCH_ERR("[ROMLib] Rename failed: %s -> %s\n",
               ctx.part_path, dest_path);
         filestream_delete(ctx.part_path);
         return ROM_LIBRARY_HTTP_ERR_IO;
      }
      RARCH_LOG("[ROMLib] Download complete: %s (%lld bytes)\n",
            dest_path, (long long)total);
      return ROM_LIBRARY_HTTP_OK;
   }

   /* Failure/cancel: drop the partial file. */
   if (ctx.part_path[0])
      filestream_delete(ctx.part_path);
   return rc;
}

/* ------------------------------------------------------------------------- */
/* Buffered sink: PROPFIND (small XML response)                              */
/* ------------------------------------------------------------------------- */

typedef struct
{
   char  *data;
   size_t len;
   size_t cap;
} rl_buf_ctx_t;

static enum rom_library_http_status rl_buf_on_headers(
      void *c, int status, int64_t total)
{
   (void)c;
   (void)total;
   if (status / 100 != 2)
      return ROM_LIBRARY_HTTP_ERR_HTTP;
   return ROM_LIBRARY_HTTP_OK;
}

static bool rl_buf_on_body(void *c, const uint8_t *data, size_t len)
{
   rl_buf_ctx_t *b = (rl_buf_ctx_t*)c;

   if (b->len + len + 1 > b->cap)
   {
      size_t ncap = b->cap ? b->cap : 16384;
      char  *nd;
      while (ncap < b->len + len + 1)
         ncap *= 2;
      if (ncap > RL_HTTP_BUF_MAX)
         return false;
      if (!(nd = (char*)realloc(b->data, ncap)))
         return false;
      b->data = nd;
      b->cap  = ncap;
   }

   memcpy(b->data + b->len, data, len);
   b->len += len;
   return true;
}

enum rom_library_http_status rom_library_http_propfind(
      const char *url,
      const char *url_user,
      const char *url_pass,
      char **out_data,
      size_t *out_len,
      int *out_http_status)
{
   rl_buf_ctx_t                 ctx;
   enum rom_library_http_status rc;
   int                          http_status = 0;
   int64_t                      total       = 0;
   static const char propfind_body[] =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
      "<D:propfind xmlns:D=\"DAV:\">"
      "<D:prop>"
      "<D:resourcetype/>"
      "<D:getcontentlength/>"
      "<D:displayname/>"
      "</D:prop>"
      "</D:propfind>";
   static const char extra_headers[] =
      "Depth: 1\r\n"
      "Content-Type: application/xml\r\n";

   if (out_data)
      *out_data = NULL;
   if (out_len)
      *out_len = 0;
   if (out_http_status)
      *out_http_status = 0;
   if (!url || !*url)
      return ROM_LIBRARY_HTTP_ERR_PARAM;

   memset(&ctx, 0, sizeof(ctx));

   rc = rl_http_perform("PROPFIND", url, url_user, url_pass,
         extra_headers, propfind_body, sizeof(propfind_body) - 1,
         rl_buf_on_headers, rl_buf_on_body, &ctx,
         NULL, NULL, &http_status, &total);

   if (out_http_status)
      *out_http_status = http_status;

   if (rc != ROM_LIBRARY_HTTP_OK)
   {
      free(ctx.data);
      return rc;
   }

   /* Guarantee a NUL-terminated buffer for the rxml parser (phase 2c). */
   if (!ctx.data)
   {
      if (!(ctx.data = (char*)malloc(1)))
         return ROM_LIBRARY_HTTP_ERR_PARAM;
      ctx.cap = 1;
   }
   ctx.data[ctx.len] = '\0';

   if (out_data)
      *out_data = ctx.data;
   else
      free(ctx.data);
   if (out_len)
      *out_len = ctx.len;

   RARCH_LOG("[ROMLib] PROPFIND ok, %u bytes.\n", (unsigned)ctx.len);
   return ROM_LIBRARY_HTTP_OK;
}
