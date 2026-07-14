/* RetroArch — Online ROM Library (WebDAV)
 *
 * rom_library_unzip.c — Standalone streaming ZIP extractor (see the header for
 * why libretro-common's zip backend cannot be used).
 *
 * Layout parsed here (all little-endian):
 *
 *   [local header + data]...  [central directory]  [ZIP64 EOCD]  [ZIP64 loc]  [EOCD]
 *
 * The central directory is the authority: it carries the sizes, the CRC and the
 * offset of every member's local header. Its 32-bit fields saturate at
 * 0xFFFFFFFF, at which point the real 64-bit value lives in the member's ZIP64
 * extra field (id 0x0001) — that is precisely the case upstream misses, and the
 * case every >4 GiB ISO hits.
 *
 * ⚠ I/O does NOT go through RetroArch's VFS (filestream_*), on purpose. On
 * Windows/MinGW the build defines neither ATLEAST_VC2005 nor HAVE_64BIT_OFFSETS
 * (it is defined nowhere in the tree), so retro_vfs_file_seek_internal() ends up
 * in its `fseek(fp, (long)offset, whence)` fallback and retro_vfs_file_tell_impl()
 * in `ftell()`. long is 32-bit on Windows: every offset past 2 GiB is truncated
 * and tell() returns -1. Our archives are routinely 3+ GiB and their central
 * directory sits at the very end, so the VFS cannot even find it. We therefore
 * do our own 64-bit stdio (fopen_utf8 + _fseeki64/fseeko) and keep the VFS only
 * for path-level operations (delete/rename), which are offset-free and fine.
 *
 * Memory budget is flat: one input chunk, one output chunk, plus the central
 * directory itself (a few KiB for our archives). Nothing scales with member
 * size, so a 4.4 GiB PS2 image extracts in ~384 KiB of RAM.
 */

/* Must precede every include: makes off_t (and fseeko/ftello) 64-bit on 32-bit
 * POSIX hosts such as the Raspberry Pi. */
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <retro_common_api.h>
#include <retro_miscellaneous.h>
#include <boolean.h>

#include <compat/fopen_utf8.h>
#include <string/stdstring.h>
#include <streams/file_stream.h>
#include <file/file_path.h>
#include <encodings/crc32.h>

#ifdef HAVE_ZLIB
#include <zlib.h>
#endif

#include "verbosity.h"

#include "rom_library.h"
#include "rom_library_unzip.h"

/* Signatures. */
#define RL_ZIP_SIG_LFH          0x04034b50u  /* local file header            */
#define RL_ZIP_SIG_CDH          0x02014b50u  /* central directory header     */
#define RL_ZIP_SIG_EOCD         0x06054b50u  /* end of central directory     */
#define RL_ZIP_SIG_EOCD64       0x06064b50u  /* ZIP64 EOCD record            */
#define RL_ZIP_SIG_EOCD64_LOC   0x07064b50u  /* ZIP64 EOCD locator           */

/* Compression methods we support. Anything else is rejected rather than
 * silently producing garbage. */
#define RL_ZIP_METHOD_STORED    0
#define RL_ZIP_METHOD_DEFLATE   8

/* Extra-field id holding the 64-bit sizes/offset. */
#define RL_ZIP_EXTRA_ID_ZIP64   0x0001

/* Sentinels meaning "the real value is in the ZIP64 extra field". */
#define RL_ZIP_U32_MAX          0xFFFFFFFFu
#define RL_ZIP_U16_MAX          0xFFFFu

/* Fixed header sizes. */
#define RL_ZIP_LFH_SIZE         30
#define RL_ZIP_CDH_SIZE         46
#define RL_ZIP_EOCD_SIZE        22
#define RL_ZIP_EOCD64_LOC_SIZE  20
#define RL_ZIP_EOCD64_MIN_SIZE  56

/* Streaming buffers: the whole point of this module. */
#define RL_UNZIP_IN_BUF         (128 * 1024)
#define RL_UNZIP_OUT_BUF        (256 * 1024)

/* The EOCD sits within the last 64 KiB + 22 bytes (max comment length). */
#define RL_UNZIP_TAIL_MAX       (65536 + RL_ZIP_EOCD_SIZE)

/* Sanity cap on the central directory we buffer. Our archives hold a single
 * member (a few dozen bytes of directory); 64 MiB is a runaway guard, not a
 * real limit. */
#define RL_UNZIP_CD_MAX         (64 * 1024 * 1024)

/* General-purpose bit flag 0: the member is encrypted. */
#define RL_ZIP_FLAG_ENCRYPTED   0x0001

/* ------------------------------------------------------------------------- */
/* 64-bit stdio (see the file header for why the VFS is unusable here)       */
/* ------------------------------------------------------------------------- */

static FILE *rl_fopen(const char *path, const char *mode)
{
   return (FILE*)fopen_utf8(path, mode);
}

static bool rl_fseek64(FILE *fp, int64_t offset, int whence)
{
#if defined(_WIN32)
   return _fseeki64(fp, offset, whence) == 0;
#else
   return fseeko(fp, (off_t)offset, whence) == 0;
#endif
}

static int64_t rl_ftell64(FILE *fp)
{
#if defined(_WIN32)
   return (int64_t)_ftelli64(fp);
#else
   return (int64_t)ftello(fp);
#endif
}

/* ------------------------------------------------------------------------- */
/* Little-endian readers                                                     */
/* ------------------------------------------------------------------------- */

static uint32_t rl_rd16(const uint8_t *p)
{
   return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t rl_rd32(const uint8_t *p)
{
   return  (uint32_t)p[0]
        | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16)
        | ((uint32_t)p[3] << 24);
}

static uint64_t rl_rd64(const uint8_t *p)
{
   return (uint64_t)rl_rd32(p) | ((uint64_t)rl_rd32(p + 4) << 32);
}

/* Positioned read of exactly len bytes. */
static bool rl_read_at(FILE *fp, int64_t offset, void *buf, size_t len)
{
   if (!rl_fseek64(fp, offset, SEEK_SET))
      return false;
   return fread(buf, 1, len, fp) == len;
}

/* ------------------------------------------------------------------------- */
/* Member metadata (one central-directory entry, ZIP64 already resolved)     */
/* ------------------------------------------------------------------------- */

typedef struct
{
   int64_t  csize;        /* compressed size                                 */
   int64_t  usize;        /* uncompressed size                               */
   int64_t  lfh_offset;   /* offset of the local file header                 */
   uint32_t crc;          /* expected CRC32 of the uncompressed data         */
   unsigned method;       /* compression method                              */
   unsigned flags;        /* general-purpose bit flags                       */
   bool     is_dir;       /* directory entry (no payload)                    */
   char     name[PATH_MAX_LENGTH];
} rl_zip_member_t;

/* Reads the ZIP64 extra field, which lists only the fields that saturated, in
 * a fixed order: uncompressed size, compressed size, local header offset, disk
 * number. Skipping a present field would shift every following one, so each is
 * consumed strictly in order and only when its 32-bit counterpart was
 * 0xFFFFFFFF. */
static bool rl_zip64_extra(rl_zip_member_t *m, const uint8_t *extra,
      size_t extra_len, bool need_usize, bool need_csize, bool need_offset,
      bool need_disk)
{
   size_t pos = 0;

   while (pos + 4 <= extra_len)
   {
      uint32_t       id    = rl_rd16(extra + pos);
      uint32_t       size  = rl_rd16(extra + pos + 2);
      const uint8_t *data  = extra + pos + 4;
      size_t         avail;

      if (pos + 4 + size > extra_len)
         return false;               /* declared size runs past the field */

      if (id != RL_ZIP_EXTRA_ID_ZIP64)
      {
         pos += 4 + size;
         continue;
      }

      avail = size;

      if (need_usize)
      {
         if (avail < 8)
            return false;
         m->usize = (int64_t)rl_rd64(data);
         data    += 8;
         avail   -= 8;
      }
      if (need_csize)
      {
         if (avail < 8)
            return false;
         m->csize = (int64_t)rl_rd64(data);
         data    += 8;
         avail   -= 8;
      }
      if (need_offset)
      {
         if (avail < 8)
            return false;
         m->lfh_offset = (int64_t)rl_rd64(data);
         data         += 8;
         avail        -= 8;
      }
      /* The disk number is the last field; we are single-volume only, so it is
       * validated for completeness and ignored. */
      if (need_disk && avail < 4)
         return false;

      return true;
   }

   /* A saturated field with no ZIP64 extra to resolve it is malformed. */
   return false;
}

/* Parses the central-directory entry at cd[pos]. On success *next receives the
 * offset of the following entry. */
static bool rl_parse_cd_entry(const uint8_t *cd, size_t cd_len, size_t pos,
      rl_zip_member_t *m, size_t *next)
{
   uint32_t csize32, usize32, offset32, disk16;
   size_t   namelen, extralen, commentlen, total;
   bool     need_usize, need_csize, need_offset, need_disk;
   char     last;

   if (pos + RL_ZIP_CDH_SIZE > cd_len)
      return false;
   if (rl_rd32(cd + pos) != RL_ZIP_SIG_CDH)
      return false;

   m->flags   = rl_rd16(cd + pos + 8);
   m->method  = rl_rd16(cd + pos + 10);
   m->crc     = rl_rd32(cd + pos + 16);
   csize32    = rl_rd32(cd + pos + 20);
   usize32    = rl_rd32(cd + pos + 24);
   namelen    = rl_rd16(cd + pos + 28);
   extralen   = rl_rd16(cd + pos + 30);
   commentlen = rl_rd16(cd + pos + 32);
   disk16     = rl_rd16(cd + pos + 34);
   offset32   = rl_rd32(cd + pos + 42);

   total = RL_ZIP_CDH_SIZE + namelen + extralen + commentlen;
   if (pos + total > cd_len)
      return false;
   if (namelen == 0 || namelen >= PATH_MAX_LENGTH)
      return false;

   memcpy(m->name, cd + pos + RL_ZIP_CDH_SIZE, namelen);
   m->name[namelen] = '\0';

   m->csize      = (int64_t)csize32;
   m->usize      = (int64_t)usize32;
   m->lfh_offset = (int64_t)offset32;

   need_usize  = (usize32  == RL_ZIP_U32_MAX);
   need_csize  = (csize32  == RL_ZIP_U32_MAX);
   need_offset = (offset32 == RL_ZIP_U32_MAX);
   need_disk   = (disk16   == RL_ZIP_U16_MAX);

   /* THE fix: >4 GiB members saturate these fields and the true values live in
    * the extra field. Upstream stops at the 32-bit read above. */
   if (need_usize || need_csize || need_offset || need_disk)
   {
      if (!rl_zip64_extra(m, cd + pos + RL_ZIP_CDH_SIZE + namelen, extralen,
               need_usize, need_csize, need_offset, need_disk))
         return false;
   }

   if (m->csize < 0 || m->usize < 0 || m->lfh_offset < 0)
      return false;

   last      = m->name[namelen - 1];
   m->is_dir = (last == '/' || last == '\\');

   *next = pos + total;
   return true;
}

/* ------------------------------------------------------------------------- */
/* Central directory location (EOCD, then ZIP64 EOCD when saturated)         */
/* ------------------------------------------------------------------------- */

typedef struct
{
   int64_t cd_offset;
   int64_t cd_size;
} rl_zip_cd_t;

static bool rl_find_central_dir(FILE *fp, int64_t fsize, rl_zip_cd_t *out)
{
   uint8_t *tail;
   int64_t  tail_len = MIN(fsize, (int64_t)RL_UNZIP_TAIL_MAX);
   int64_t  tail_pos;
   int64_t  i;
   int64_t  eocd     = -1;
   uint32_t cd_size32, cd_offset32;
   bool     ok       = false;

   if (tail_len < RL_ZIP_EOCD_SIZE)
      return false;

   tail_pos = fsize - tail_len;

   if (!(tail = (uint8_t*)malloc((size_t)tail_len)))
      return false;
   if (!rl_read_at(fp, tail_pos, tail, (size_t)tail_len))
      goto done;

   /* Scan backwards for the EOCD signature, confirming it with the trailing
    * comment length so a signature appearing inside the comment (or inside the
    * compressed data) cannot be mistaken for the real record. */
   for (i = tail_len - RL_ZIP_EOCD_SIZE; i >= 0; i--)
   {
      if (rl_rd32(tail + i) != RL_ZIP_SIG_EOCD)
         continue;
      if (tail_pos + i + RL_ZIP_EOCD_SIZE + (int64_t)rl_rd16(tail + i + 20)
            == fsize)
      {
         eocd = i;
         break;
      }
   }

   if (eocd < 0)
      goto done;

   cd_size32   = rl_rd32(tail + eocd + 12);
   cd_offset32 = rl_rd32(tail + eocd + 16);

   out->cd_size   = (int64_t)cd_size32;
   out->cd_offset = (int64_t)cd_offset32;

   /* Saturated: the real values are in the ZIP64 EOCD record, found via the
    * locator that sits immediately before the EOCD. An archive whose payload
    * exceeds 4 GiB lands here even when no single member does. */
   if (cd_size32 == RL_ZIP_U32_MAX || cd_offset32 == RL_ZIP_U32_MAX)
   {
      uint8_t loc[RL_ZIP_EOCD64_LOC_SIZE];
      uint8_t rec[RL_ZIP_EOCD64_MIN_SIZE];
      int64_t loc_off = tail_pos + eocd - RL_ZIP_EOCD64_LOC_SIZE;
      int64_t rec_off;

      if (loc_off < 0)
         goto done;
      if (!rl_read_at(fp, loc_off, loc, sizeof(loc)))
         goto done;
      if (rl_rd32(loc) != RL_ZIP_SIG_EOCD64_LOC)
         goto done;

      rec_off = (int64_t)rl_rd64(loc + 8);
      if (rec_off < 0 || rec_off + RL_ZIP_EOCD64_MIN_SIZE > fsize)
         goto done;
      if (!rl_read_at(fp, rec_off, rec, sizeof(rec)))
         goto done;
      if (rl_rd32(rec) != RL_ZIP_SIG_EOCD64)
         goto done;

      out->cd_size   = (int64_t)rl_rd64(rec + 40);
      out->cd_offset = (int64_t)rl_rd64(rec + 48);
   }

   if (   out->cd_size   < 0
       || out->cd_offset < 0
       || out->cd_size   > (int64_t)RL_UNZIP_CD_MAX
       || out->cd_offset + out->cd_size > fsize)
      goto done;

   ok = true;

done:
   free(tail);
   return ok;
}

/* ------------------------------------------------------------------------- */
/* Member extraction                                                          */
/* ------------------------------------------------------------------------- */

/* Rejects member names that would escape dest_dir (absolute paths, drive
 * letters, "..") and joins the sanitised name onto dest_dir. A ZIP is untrusted
 * input even from our own server. */
static bool rl_member_dest(char *out, size_t out_len,
      const char *dest_dir, const char *name)
{
   const char *p = name;

   if (!name || !*name)
      return false;
   if (*name == '/' || *name == '\\')
      return false;
   if (strchr(name, ':'))
      return false;

   /* Reject any ".." path segment. */
   while (*p)
   {
      const char *sep = p;
      while (*sep && *sep != '/' && *sep != '\\')
         sep++;
      if ((sep - p) == 2 && p[0] == '.' && p[1] == '.')
         return false;
      if (!*sep)
         break;
      p = sep + 1;
   }

   fill_pathname_join_special(out, dest_dir, name, out_len);
   return true;
}

/* Shared state for one extraction run, so the per-member helpers stay short. */
typedef struct
{
   FILE                         *zf;
   const char                   *dest_dir;
   uint8_t                      *inbuf;
   uint8_t                      *outbuf;
   int64_t                       consumed;   /* compressed bytes read so far */
   int64_t                       total;      /* sum of every member's csize  */
   rom_library_unzip_progress_cb cb;
   void                         *cb_data;
} rl_unzip_ctx_t;

/* Reports progress and honours cancellation. Returns false to abort. */
static bool rl_unzip_tick(rl_unzip_ctx_t *ctx)
{
   if (!ctx->cb)
      return true;
   return ctx->cb(ctx->cb_data, ctx->consumed, ctx->total);
}

/* Locates a member's payload: the central directory's name/extra lengths are
 * allowed to differ from the local header's, so the data offset must be derived
 * from the LOCAL header (a classic source of off-by-N corruption). */
static bool rl_member_data_offset(rl_unzip_ctx_t *ctx,
      const rl_zip_member_t *m, int64_t *out)
{
   uint8_t lfh[RL_ZIP_LFH_SIZE];

   if (!rl_read_at(ctx->zf, m->lfh_offset, lfh, sizeof(lfh)))
      return false;
   if (rl_rd32(lfh) != RL_ZIP_SIG_LFH)
      return false;

   *out = m->lfh_offset + RL_ZIP_LFH_SIZE
        + (int64_t)rl_rd16(lfh + 26)   /* file name length   */
        + (int64_t)rl_rd16(lfh + 28);  /* extra field length */

   return true;
}

/* Copies a STORED member straight through, chunk by chunk. */
static enum rom_library_unzip_status rl_extract_stored(rl_unzip_ctx_t *ctx,
      const rl_zip_member_t *m, FILE *out, uint32_t *crc, int64_t *written)
{
   int64_t remaining = m->csize;

   while (remaining > 0)
   {
      size_t want = (size_t)MIN(remaining, (int64_t)RL_UNZIP_OUT_BUF);
      size_t got  = fread(ctx->outbuf, 1, want, ctx->zf);

      if (got == 0)
         return ROM_LIBRARY_UNZIP_ERR_IO;
      if (fwrite(ctx->outbuf, 1, got, out) != got)
         return ROM_LIBRARY_UNZIP_ERR_IO;

      *crc           = encoding_crc32(*crc, ctx->outbuf, got);
      *written      += (int64_t)got;
      remaining     -= (int64_t)got;
      ctx->consumed += (int64_t)got;

      if (!rl_unzip_tick(ctx))
         return ROM_LIBRARY_UNZIP_ERR_CANCELLED;
   }

   return ROM_LIBRARY_UNZIP_OK;
}

#ifdef HAVE_ZLIB
/* Inflates a DEFLATE member, writing each output chunk to disk as it is
 * produced. Nothing here scales with member size. */
static enum rom_library_unzip_status rl_extract_deflate(rl_unzip_ctx_t *ctx,
      const rl_zip_member_t *m, FILE *out, uint32_t *crc, int64_t *written)
{
   z_stream zs;
   int64_t  remaining = m->csize;
   enum rom_library_unzip_status rc = ROM_LIBRARY_UNZIP_OK;

   memset(&zs, 0, sizeof(zs));

   /* Raw inflate: a ZIP member carries no zlib wrapper. */
   if (inflateInit2(&zs, -MAX_WBITS) != Z_OK)
      return ROM_LIBRARY_UNZIP_ERR_MEM;

   for (;;)
   {
      int     ret;
      int64_t produced;

      if (zs.avail_in == 0 && remaining > 0)
      {
         size_t want = (size_t)MIN(remaining, (int64_t)RL_UNZIP_IN_BUF);
         size_t got  = fread(ctx->inbuf, 1, want, ctx->zf);

         if (got == 0)
         {
            rc = ROM_LIBRARY_UNZIP_ERR_IO;
            break;
         }

         zs.next_in     = ctx->inbuf;
         zs.avail_in    = (uInt)got;
         remaining     -= (int64_t)got;
         ctx->consumed += (int64_t)got;

         if (!rl_unzip_tick(ctx))
         {
            rc = ROM_LIBRARY_UNZIP_ERR_CANCELLED;
            break;
         }
      }

      zs.next_out  = ctx->outbuf;
      zs.avail_out = RL_UNZIP_OUT_BUF;

      ret = inflate(&zs, Z_NO_FLUSH);
      if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR)
      {
         rc = ROM_LIBRARY_UNZIP_ERR_FORMAT;
         break;
      }

      produced = (int64_t)RL_UNZIP_OUT_BUF - (int64_t)zs.avail_out;
      if (produced > 0)
      {
         if (fwrite(ctx->outbuf, 1, (size_t)produced, out) != (size_t)produced)
         {
            rc = ROM_LIBRARY_UNZIP_ERR_IO;
            break;
         }
         *crc      = encoding_crc32(*crc, ctx->outbuf, (size_t)produced);
         *written += produced;
      }

      if (ret == Z_STREAM_END)
         break;

      /* No input left and no output produced: the stream ended early. */
      if (produced == 0 && zs.avail_in == 0 && remaining <= 0)
      {
         rc = ROM_LIBRARY_UNZIP_ERR_FORMAT;
         break;
      }
   }

   inflateEnd(&zs);
   return rc;
}
#endif

/* Extracts one member to "<dest>.part", then renames it into place. */
static enum rom_library_unzip_status rl_extract_member(rl_unzip_ctx_t *ctx,
      const rl_zip_member_t *m)
{
   char     dest[PATH_MAX_LENGTH];
   char     part[PATH_MAX_LENGTH];
   char     basedir[PATH_MAX_LENGTH];
   FILE    *out      = NULL;
   uint32_t crc      = 0;
   int64_t  written  = 0;
   int64_t  data_off = 0;
   enum rom_library_unzip_status rc;

   if (m->flags & RL_ZIP_FLAG_ENCRYPTED)
   {
      RARCH_ERR("[ROMLib] encrypted ZIP member, cannot extract: %s\n", m->name);
      return ROM_LIBRARY_UNZIP_ERR_FORMAT;
   }

   if (!rl_member_dest(dest, sizeof(dest), ctx->dest_dir, m->name))
   {
      RARCH_ERR("[ROMLib] unsafe ZIP member name, refusing: %s\n", m->name);
      return ROM_LIBRARY_UNZIP_ERR_FORMAT;
   }

   /* Directory entry: just materialise it. */
   if (m->is_dir)
   {
      if (!path_is_directory(dest) && !path_mkdir(dest))
         return ROM_LIBRARY_UNZIP_ERR_IO;
      return ROM_LIBRARY_UNZIP_OK;
   }

   /* Members may sit in sub-directories that have no directory entry of their
    * own; path_mkdir creates the whole chain. */
   fill_pathname_basedir(basedir, dest, sizeof(basedir));
   if (!string_is_empty(basedir) && !path_is_directory(basedir)
         && !path_mkdir(basedir))
      return ROM_LIBRARY_UNZIP_ERR_IO;

   if (!rl_member_data_offset(ctx, m, &data_off))
      return ROM_LIBRARY_UNZIP_ERR_FORMAT;
   if (!rl_fseek64(ctx->zf, data_off, SEEK_SET))
      return ROM_LIBRARY_UNZIP_ERR_IO;

   snprintf(part, sizeof(part), "%s.part", dest);
   if (!(out = rl_fopen(part, "wb")))
   {
      RARCH_ERR("[ROMLib] cannot open for writing: %s\n", part);
      return ROM_LIBRARY_UNZIP_ERR_IO;
   }

   switch (m->method)
   {
      case RL_ZIP_METHOD_STORED:
         rc = rl_extract_stored(ctx, m, out, &crc, &written);
         break;
      case RL_ZIP_METHOD_DEFLATE:
#ifdef HAVE_ZLIB
         rc = rl_extract_deflate(ctx, m, out, &crc, &written);
         break;
#else
         RARCH_ERR("[ROMLib] built without zlib: cannot inflate %s\n", m->name);
         rc = ROM_LIBRARY_UNZIP_ERR_FORMAT;
         break;
#endif
      default:
         RARCH_ERR("[ROMLib] unsupported ZIP compression method %u for %s\n",
               m->method, m->name);
         rc = ROM_LIBRARY_UNZIP_ERR_FORMAT;
         break;
   }

   if (fclose(out) != 0 && rc == ROM_LIBRARY_UNZIP_OK)
      rc = ROM_LIBRARY_UNZIP_ERR_IO;   /* a failed flush would truncate it */

   if (rc == ROM_LIBRARY_UNZIP_OK && written != m->usize)
   {
      RARCH_ERR("[ROMLib] size mismatch on %s: got %lld, expected %lld.\n",
            m->name, (long long)written, (long long)m->usize);
      rc = ROM_LIBRARY_UNZIP_ERR_FORMAT;
   }

   /* The CRC is the end-to-end proof that the 64-bit path is byte-exact. */
   if (rc == ROM_LIBRARY_UNZIP_OK && crc != m->crc)
   {
      RARCH_ERR("[ROMLib] CRC mismatch on %s: got %08X, expected %08X.\n",
            m->name, crc, m->crc);
      rc = ROM_LIBRARY_UNZIP_ERR_CRC;
   }

   if (rc != ROM_LIBRARY_UNZIP_OK)
   {
      filestream_delete(part);
      return rc;
   }

   /* Atomic publish (Windows rename() will not overwrite). */
   filestream_delete(dest);
   if (filestream_rename(part, dest) != 0)
   {
      RARCH_ERR("[ROMLib] rename failed: %s -> %s\n", part, dest);
      filestream_delete(part);
      return ROM_LIBRARY_UNZIP_ERR_IO;
   }

   RARCH_LOG("[ROMLib] extracted %s (%lld bytes).\n",
         m->name, (long long)written);

   return ROM_LIBRARY_UNZIP_OK;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                 */
/* ------------------------------------------------------------------------- */

bool rom_library_unzip_is_zip(const char *path)
{
   FILE    *fp;
   uint8_t  sig[4];
   bool     ok = false;

   if (string_is_empty(path))
      return false;

   if (!(fp = rl_fopen(path, "rb")))
      return false;

   if (fread(sig, 1, sizeof(sig), fp) == sizeof(sig))
   {
      uint32_t s = rl_rd32(sig);
      /* A local header (normal archive) or a bare EOCD (empty archive). */
      ok = (s == RL_ZIP_SIG_LFH || s == RL_ZIP_SIG_EOCD);
   }

   fclose(fp);
   return ok;
}

enum rom_library_unzip_status rom_library_unzip_extract(
      const char *archive_path,
      const char *dest_dir,
      rom_library_unzip_progress_cb progress_cb,
      void *progress_user_data)
{
   rl_unzip_ctx_t  ctx;
   rl_zip_cd_t     cd_loc;
   rl_zip_member_t m;
   uint8_t        *cd    = NULL;
   int64_t         fsize;
   int64_t         need  = 0;
   int64_t         freeb;
   size_t          pos;
   unsigned        files = 0;
   enum rom_library_unzip_status rc = ROM_LIBRARY_UNZIP_OK;

   if (string_is_empty(archive_path) || string_is_empty(dest_dir))
      return ROM_LIBRARY_UNZIP_ERR_PARAM;

   memset(&ctx, 0, sizeof(ctx));
   ctx.dest_dir = dest_dir;
   ctx.cb       = progress_cb;
   ctx.cb_data  = progress_user_data;

   if (!(ctx.zf = rl_fopen(archive_path, "rb")))
      return ROM_LIBRARY_UNZIP_ERR_IO;

   if (!rl_fseek64(ctx.zf, 0, SEEK_END))
   {
      rc = ROM_LIBRARY_UNZIP_ERR_IO;
      goto done;
   }
   if ((fsize = rl_ftell64(ctx.zf)) < 0)
   {
      rc = ROM_LIBRARY_UNZIP_ERR_IO;
      goto done;
   }

   if (!rl_find_central_dir(ctx.zf, fsize, &cd_loc))
   {
      rc = ROM_LIBRARY_UNZIP_ERR_NOT_ZIP;
      goto done;
   }

   if (!(cd = (uint8_t*)malloc((size_t)cd_loc.cd_size)))
   {
      rc = ROM_LIBRARY_UNZIP_ERR_MEM;
      goto done;
   }
   if (!rl_read_at(ctx.zf, cd_loc.cd_offset, cd, (size_t)cd_loc.cd_size))
   {
      rc = ROM_LIBRARY_UNZIP_ERR_IO;
      goto done;
   }

   /* Pass 1: validate every entry and total up the work. Doing this before
    * writing anything means a malformed archive or a full disk fails cleanly,
    * with nothing half-extracted for the scanner to pick up. */
   pos = 0;
   while (pos + RL_ZIP_CDH_SIZE <= (size_t)cd_loc.cd_size)
   {
      size_t next;

      if (rl_rd32(cd + pos) != RL_ZIP_SIG_CDH)
         break;
      if (!rl_parse_cd_entry(cd, (size_t)cd_loc.cd_size, pos, &m, &next))
      {
         RARCH_ERR("[ROMLib] malformed ZIP central directory: %s\n",
               archive_path);
         rc = ROM_LIBRARY_UNZIP_ERR_FORMAT;
         goto done;
      }

      if (!m.is_dir)
      {
         need      += m.usize;
         ctx.total += m.csize;
         files++;
      }
      pos = next;
   }

   if (files == 0)
   {
      RARCH_WARN("[ROMLib] ZIP holds no files: %s\n", archive_path);
      rc = ROM_LIBRARY_UNZIP_ERR_FORMAT;
      goto done;
   }

   freeb = rom_library_disk_free(dest_dir);
   if (freeb >= 0 && freeb < need)
   {
      RARCH_ERR("[ROMLib] not enough disk space to extract: "
            "need %lld, have %lld.\n", (long long)need, (long long)freeb);
      rc = ROM_LIBRARY_UNZIP_ERR_NOSPACE;
      goto done;
   }

   if (   !(ctx.inbuf  = (uint8_t*)malloc(RL_UNZIP_IN_BUF))
       || !(ctx.outbuf = (uint8_t*)malloc(RL_UNZIP_OUT_BUF)))
   {
      rc = ROM_LIBRARY_UNZIP_ERR_MEM;
      goto done;
   }

   RARCH_LOG("[ROMLib] extracting %s: %u file(s), %lld bytes -> %s\n",
         archive_path, files, (long long)need, dest_dir);

   /* Pass 2: extract. */
   pos = 0;
   while (pos + RL_ZIP_CDH_SIZE <= (size_t)cd_loc.cd_size)
   {
      size_t next;

      if (rl_rd32(cd + pos) != RL_ZIP_SIG_CDH)
         break;
      if (!rl_parse_cd_entry(cd, (size_t)cd_loc.cd_size, pos, &m, &next))
      {
         rc = ROM_LIBRARY_UNZIP_ERR_FORMAT;
         goto done;
      }

      if ((rc = rl_extract_member(&ctx, &m)) != ROM_LIBRARY_UNZIP_OK)
         goto done;

      pos = next;
   }

done:
   free(cd);
   free(ctx.inbuf);
   free(ctx.outbuf);
   if (ctx.zf)
      fclose(ctx.zf);
   return rc;
}

const char *rom_library_unzip_strerror(enum rom_library_unzip_status status)
{
   switch (status)
   {
      case ROM_LIBRARY_UNZIP_OK:
         return "OK";
      case ROM_LIBRARY_UNZIP_ERR_PARAM:
         return "invalid parameters";
      case ROM_LIBRARY_UNZIP_ERR_NOT_ZIP:
         return "not a ZIP archive";
      case ROM_LIBRARY_UNZIP_ERR_FORMAT:
         return "unsupported or malformed archive";
      case ROM_LIBRARY_UNZIP_ERR_IO:
         return "read/write error";
      case ROM_LIBRARY_UNZIP_ERR_MEM:
         return "out of memory";
      case ROM_LIBRARY_UNZIP_ERR_NOSPACE:
         return "not enough disk space";
      case ROM_LIBRARY_UNZIP_ERR_CRC:
         return "CRC mismatch";
      case ROM_LIBRARY_UNZIP_ERR_CANCELLED:
         return "cancelled";
   }
   return "unknown error";
}
