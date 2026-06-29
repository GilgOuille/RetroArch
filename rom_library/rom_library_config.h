/* RetroArch — Online ROM Library (WebDAV)
 *
 * rom_library_config.h — The three feature settings, stored in their own
 * rom_library.cfg (NOT retroarch.cfg) so that configuration.c / menu_setting.c
 * stay untouched (CLAUDE.md §2.B, §6).
 *
 * Settings: server URL, username, password. The password is stored in clear
 * text (like Cloud Sync) — the user must be warned in the UI.
 */

#ifndef __ROM_LIBRARY_CONFIG_H
#define __ROM_LIBRARY_CONFIG_H

#include <retro_common_api.h>
#include <boolean.h>

RETRO_BEGIN_DECLS

#define ROM_LIBRARY_DEFAULT_URL "https://roms.gilgaserver.com/"

typedef struct
{
   char *url;       /* Base WebDAV URL, trailing '/' included. */
   char *username;  /* HTTP Basic username.                    */
   char *password;  /* HTTP Basic password (stored in clear).  */
} rom_library_config_t;

/* Returns the process-global config singleton, loading rom_library.cfg from
 * the RetroArch config directory on first access. Never returns NULL. */
rom_library_config_t *rom_library_config_get(void);

/* Loads (or reloads) rom_library.cfg into the singleton. Missing file is not
 * an error — defaults are applied. Returns false only on hard failure. */
bool rom_library_config_load(void);

/* Writes the current singleton back to rom_library.cfg. */
bool rom_library_config_save(void);

/* Setters take ownership semantics of a copy: the string is duplicated. */
void rom_library_config_set_url(const char *url);
void rom_library_config_set_username(const char *username);
void rom_library_config_set_password(const char *password);

/* Frees the singleton (shutdown). */
void rom_library_config_deinit(void);

RETRO_END_DECLS

#endif /* __ROM_LIBRARY_CONFIG_H */
