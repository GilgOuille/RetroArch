/* RetroArch — Online ROM Library (WebDAV)
 *
 * rom_library_config.c — Settings load/save into rom_library.cfg.
 *
 * The three settings (URL, username, password) live in their own
 * rom_library.cfg next to retroarch.cfg, so that the upstream configuration.c
 * / menu_setting.c stay untouched (CLAUDE.md §2.B, §6). The password is stored
 * in clear text (like Cloud Sync) — the user is warned in the UI.
 */

#include <stdlib.h>
#include <string.h>

#include <retro_common_api.h>
#include <retro_miscellaneous.h>
#include <string/stdstring.h>
#include <file/file_path.h>
#include <file/config_file.h>

#include "../paths.h"
#include "../file_path_special.h"
#include "../verbosity.h"

#include "rom_library_config.h"

#define ROM_LIBRARY_CFG_FILENAME "rom_library.cfg"
#define ROM_LIBRARY_CFG_KEY_URL  "rom_library_url"
#define ROM_LIBRARY_CFG_KEY_USER "rom_library_username"
#define ROM_LIBRARY_CFG_KEY_PASS "rom_library_password"

static rom_library_config_t rom_library_config_state;
static bool                 rom_library_config_inited = false;

static void rom_library_config_set_field(char **field, const char *value)
{
   if (!field)
      return;

   free(*field);
   *field = (value && *value) ? strdup(value) : NULL;
}

static void rom_library_config_apply_defaults(void)
{
   rom_library_config_set_field(&rom_library_config_state.url,
         ROM_LIBRARY_DEFAULT_URL);
   rom_library_config_set_field(&rom_library_config_state.username, NULL);
   rom_library_config_set_field(&rom_library_config_state.password, NULL);
}

/* Builds the absolute path to rom_library.cfg next to retroarch.cfg. Falls
 * back to the application-data directory when no main config path is known
 * yet. */
static void rom_library_config_path(char *s, size_t len)
{
   const char *cfg = path_get(RARCH_PATH_CONFIG);
   char         dir[PATH_MAX_LENGTH];

   dir[0] = '\0';

   if (cfg && *cfg)
      fill_pathname_basedir(dir, cfg, sizeof(dir));
   else
      fill_pathname_application_data(dir, sizeof(dir));

   fill_pathname_join(s, dir, ROM_LIBRARY_CFG_FILENAME, len);
}

/* Reads rom_library.cfg into the (already-initialised) state. A missing file
 * leaves the defaults in place. */
static void rom_library_config_read_file(void)
{
   char           path[PATH_MAX_LENGTH];
   config_file_t *conf;
   char          *val = NULL;

   rom_library_config_path(path, sizeof(path));

   if (!(conf = config_file_new_from_path_to_string(path)))
   {
      RARCH_LOG("[ROMLib] no %s yet, using defaults\n",
            ROM_LIBRARY_CFG_FILENAME);
      return;
   }

   if (config_get_string(conf, ROM_LIBRARY_CFG_KEY_URL, &val))
   {
      rom_library_config_set_field(&rom_library_config_state.url,
            (val && *val) ? val : ROM_LIBRARY_DEFAULT_URL);
      free(val);
      val = NULL;
   }
   if (config_get_string(conf, ROM_LIBRARY_CFG_KEY_USER, &val))
   {
      rom_library_config_set_field(&rom_library_config_state.username, val);
      free(val);
      val = NULL;
   }
   if (config_get_string(conf, ROM_LIBRARY_CFG_KEY_PASS, &val))
   {
      rom_library_config_set_field(&rom_library_config_state.password, val);
      free(val);
      val = NULL;
   }

   config_file_free(conf);
   RARCH_LOG("[ROMLib] loaded %s\n", path);
}

rom_library_config_t *rom_library_config_get(void)
{
   if (!rom_library_config_inited)
   {
      rom_library_config_apply_defaults();
      rom_library_config_inited = true;
      rom_library_config_read_file();
   }
   return &rom_library_config_state;
}

bool rom_library_config_load(void)
{
   (void)rom_library_config_get(); /* ensures defaults + first read */
   if (rom_library_config_inited)
      rom_library_config_read_file();
   return true;
}

bool rom_library_config_save(void)
{
   char           path[PATH_MAX_LENGTH];
   config_file_t *conf;
   bool           ok;

   (void)rom_library_config_get();
   rom_library_config_path(path, sizeof(path));

   if (!(conf = config_file_new_from_path_to_string(path)))
      conf = config_file_new_alloc();
   if (!conf)
      return false;

   config_set_string(conf, ROM_LIBRARY_CFG_KEY_URL,
         rom_library_config_state.url ? rom_library_config_state.url : "");
   config_set_string(conf, ROM_LIBRARY_CFG_KEY_USER,
         rom_library_config_state.username
            ? rom_library_config_state.username : "");
   config_set_string(conf, ROM_LIBRARY_CFG_KEY_PASS,
         rom_library_config_state.password
            ? rom_library_config_state.password : "");

   ok = config_file_write(conf, path, true);
   config_file_free(conf);

   if (ok)
      RARCH_LOG("[ROMLib] saved %s\n", path);
   else
      RARCH_ERR("[ROMLib] failed to write %s\n", path);

   return ok;
}

void rom_library_config_set_url(const char *url)
{
   (void)rom_library_config_get();
   rom_library_config_set_field(&rom_library_config_state.url,
         (url && *url) ? url : ROM_LIBRARY_DEFAULT_URL);
}

void rom_library_config_set_username(const char *username)
{
   (void)rom_library_config_get();
   rom_library_config_set_field(&rom_library_config_state.username, username);
}

void rom_library_config_set_password(const char *password)
{
   (void)rom_library_config_get();
   rom_library_config_set_field(&rom_library_config_state.password, password);
}

void rom_library_config_deinit(void)
{
   if (!rom_library_config_inited)
      return;

   free(rom_library_config_state.url);
   free(rom_library_config_state.username);
   free(rom_library_config_state.password);
   memset(&rom_library_config_state, 0, sizeof(rom_library_config_state));
   rom_library_config_inited = false;
}
