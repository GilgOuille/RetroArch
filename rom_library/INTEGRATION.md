# INTEGRATION.md — hooks de la Bibliothèque de ROMs en ligne

> **Source de vérité** des points de contact entre le code de la feature (`rom_library/`) et les fichiers **upstream partagés**. Après un rebase sur `master`, un conflit sur l'un d'eux se résout en **réappliquant exactement** cette checklist. À mettre à jour à chaque nouveau hook.
>
> Règle d'or (CLAUDE.md §2) : **hooks minimaux**, encadrés de `/* === ROM_LIBRARY BEGIN === */` … `/* === ROM_LIBRARY END === */`, qui ne font qu'appeler dans `rom_library/`. Tout le reste vit sous `rom_library/`.
>
> **10 fichiers touchés** : 1 build, 4 chaînes/enums, 5 menu. Tous sont appliqués.

---

## 1. Build — `Makefile.common`

Bloc autonome en **fin de fichier** :

```make
# === ROM_LIBRARY BEGIN ===
ifeq ($(HAVE_MENU), 1)
ifeq ($(HAVE_NETWORKING), 1)
OBJ += rom_library/rom_library.o \
       rom_library/rom_library_config.o \
       rom_library/rom_library_http.o \
       rom_library/rom_library_webdav.o \
       rom_library/rom_library_task.o \
       rom_library/rom_library_menu.o
endif
endif
# === ROM_LIBRARY END ===
```

**Pourquoi ici et pas ailleurs** : la build du fork (`./build_dist.sh` = `./configure && make`, la chaîne officielle, cf. `.github/workflows/`) passe par `Makefile` → `Makefile.common` et **ignore totalement `Makefile.win`**. Un hook posé dans un makefile de plateforme rendrait la feature absente de toute build exhaustive (MSYS2, MXE, buildbot). En fin de fichier, un conflit de rebase est au pire localisé et se résout en ré-ajoutant le bloc verbatim.

**Un simple `OBJ +=` suffit** : `Makefile` inclut `Makefile.common`, puis calcule `RARCH_OBJ := $(addprefix $(OBJDIR)/,$(OBJ))` et `-include` les `.d`. Compilation, link et suivi des en-têtes sont automatiques. Un éventuel makefile de plateforme qui inclurait `Makefile.common` (c'est le cas de `Makefile.win`) récupère donc nos objets tout seul : ⚠️ ne jamais les y redéclarer, le même `.o` passerait deux fois au linker.

**Gardes** : le bloc est conditionné à `HAVE_MENU` + `HAVE_NETWORKING` ; les hooks C (§3) portent **la même garde `#ifdef HAVE_NETWORKING`** à l'intérieur de leurs régions balisées (`HAVE_MENU` est implicite : ces fichiers vivent sous `menu/`). Sans elle, une build `--disable-networking` compilerait des appels vers des objets non compilés → erreur de link obscure.

**`griffin/griffin.c` — non modifié** : les builds *unity* console (`HAVE_GRIFFIN`) ne sont pas la cible. Si un jour c'était nécessaire : ajouter les `#include "../rom_library/*.c"` après le bloc `MANUAL CONTENT SCAN`, **et** s'assurer que ces objets ne sont pas aussi compilés séparément (doublons de symboles).

---

## 2. Chaînes & enums menu — 4 fichiers

Les **7 entrées** sont déclarées via la macro `MENU_LABEL(X)` (qui crée `MENU_ENUM_LABEL_X`, `MENU_ENUM_SUBLABEL_X` **et** `MENU_ENUM_LABEL_VALUE_X`) ; libellé (`*_STR`) et valeur d'affichage passent par le mécanisme de hash standard :

| Fichier | Contenu balisé | Emplacement |
|---|---|---|
| `msg_hash.h` | 7 × `MENU_LABEL(ROM_LIBRARY_*)` | juste **avant `MSG_LAST`** |
| `msg_hash_lbl_str.h` | 7 × `#define …_STR "…"` | avant le `#endif` de garde |
| `intl/msg_hash_lbl.h` | 7 × `MSG_HASH(LABEL, LABEL_STR)` | **fin de fichier** |
| `intl/msg_hash_us.h` | 7 × `MSG_HASH(VALUE, "Texte")` | **fin de fichier** |

Les 7 labels : `ROM_LIBRARY_LIST` (point d'entrée + page systèmes), `ROM_LIBRARY_SETTINGS` (page réglages), `DEFERRED_ROM_LIBRARY_ENTRIES` (ligne système → page ROMs), `ROM_LIBRARY_ENTRY` (ligne ROM), `ROM_LIBRARY_SETTING_URL` / `_USERNAME` / `_PASSWORD` (lignes réglage, OSK).

---

## 3. Glue menu — 5 fichiers

### `menu/menu_displaylist.h`
3 valeurs ajoutées au `enum menu_displaylist_ctl_state` (`DISPLAYLIST_ROM_LIBRARY_SYSTEMS` / `_ENTRIES` / `_SETTINGS`), avant `DISPLAYLIST_PENDING_CLEAR`.

### `menu/menu_displaylist.c` — 3 régions
1. `#include "../rom_library/rom_library_menu.h"` (avec les autres includes).
2. Dans le `switch (type)` de `menu_displaylist_ctl()` : un `case` triple (`…_SYSTEMS/_ENTRIES/_SETTINGS`) → `menu_entries_clear(info->list)` + `rom_library_menu_displaylist(info, type)` + `MD_FLAG_NEED_REFRESH | MD_FLAG_NEED_PUSH`.
3. Dans `case DISPLAYLIST_MAIN_MENU` (après le bloc `ONLINE_UPDATER`, sous `HAVE_NETWORKING`) : `count += rom_library_menu_append_main_entry(info->list)`, gardé par `!kiosk_mode_enable`. → c'est la ligne « Online ROM Library » du Menu principal, à côté d'« Online Updater ».

### `menu/cbs/menu_cbs_ok.c` — 2 régions
L'`#include`, puis dans `menu_cbs_init_bind_ok()` (juste après le bind par défaut) : si `rom_library_menu_enum_is_ours(cbs->enum_idx)` → `BIND_ACTION_OK(cbs, rom_library_menu_action_ok)` et `return 0`. Match par `enum_idx` : toutes nos lignes cliquables portent un enum réel.

### `menu/cbs/menu_cbs_cancel.c` — 2 régions
L'`#include`, puis dans `menu_cbs_init_bind_cancel()` (juste après le bind par défaut) : si `rom_library_menu_enum_is_ours(cbs->enum_idx)` → **re-bind** `action_cancel_pop_default` et `return 0`.
**Pourquoi** : nos lignes utilisent `FILE_TYPE_DOWNLOAD_URL` pour afficher leur `alt`, mais `compare_type` lie **aussi** ce type à `action_cancel_core_content`, qui **flush** la pile vers *Add Content* (→ retour au menu principal au lieu d'un cran en arrière). Le hook rétablit le Back standard pour nos entrées.

### `menu/cbs/menu_cbs_deferred_push.c` — 2 régions
L'`#include`, puis dans `menu_cbs_init_bind_deferred_push()` (juste après le bind par défaut) : si `rom_library_menu_label_is_deferred(label)` → `BIND_ACTION_DEFERRED_PUSH(cbs, rom_library_menu_deferred_push)` et `return 0`.
Match par **chaîne de label** (et non `enum_idx`) : le conteneur poussé en `DISPLAYLIST_GENERIC` a `enum_idx == MSG_UNKNOWN` mais conserve son label.

### Navigation — sans nouveaux `ACTION_OK_DL_*`
Chaque page navigable est poussée comme **liste générique différée** (`generic_action_ok_displaylist_push(..., ACTION_OK_DL_GENERIC)`, API publique de `menu_cbs.h`) : le **label** de la ligne sélectionne la page que `rom_library_menu_deferred_push()` construit (via `menu_displaylist_ctl` + `menu_displaylist_process`), et le **path** transporte l'index du système (relu via `menu->deferred_path`). **Aucun** ajout à l'enum `ACTION_OK_DL_*` ni aux tables `ok_list` / `deferred_push_bind_list`.

---

## 4. Fichiers upstream explicitement NON modifiés

`net_http.c`, `net_socket*.c`, `configuration.c`, `menu_setting.c` (CLAUDE.md §2) : toute la logique réseau/réglages est réécrite sous `rom_library/` sur les API publiques. Côté build, `griffin/griffin.c` (§1). Seule exception assumée : `Makefile.common`, seul moyen d'exister dans la build officielle.
