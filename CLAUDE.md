# RetroArch — Bibliothèque de ROMs en ligne (WebDAV)

> Fork RetroArch. **Fonctionnalité terminée, validée en usage réel** — ce doc est un contexte de *maintenance* : décisions, contraintes et pièges **non déductibles du code**. La source fait foi.
> Boucle de travail : coder → builder (§4, **toujours `both`**) → **validation IHM par l'utilisateur** (Claude ne juge pas un rendu graphique) → **l'utilisateur commite lui-même** (§6).

## 1. Objectif

Depuis l'IHM RetroArch, exploiter un catalogue de ROMs sur un serveur privé **WebDAV** (`rclone serve webdav`, HTTPS + Basic auth, URL/identifiants configurables) : naviguer (1 dossier = 1 système au nom libretro exact) → télécharger en streaming avec barre de progression (fichiers multi-Go) → décompresser → **playlist + métadonnées + pochette, sans redémarrer**.

Ensuite **RetroArch natif prend le relais** (lancement via `core_path=DETECT`, suppression d'entrée, re-scan manuel) : **rien de plus à coder**, ne pas réimplémenter.

Tout repose sur la convention **No-Intro/Redump/libretro**, déjà respectée par le serveur (dossiers = systèmes → `db_name` ; fichiers = entrées `.rdb` → libellé playlist et nom de vignette). **Ne jamais altérer les noms** : métadonnées et vignettes décrocheraient.

## 2. Isolation & rebase (PRIORITÉ)

Le fork doit suivre `master` upstream sans calvaire de merge : **tout le code dans `rom_library/`**, la glue dans les fichiers partagés réduite à des hooks d'une ligne balisés `/* === ROM_LIBRARY BEGIN/END === */`.

- **Fichiers upstream à ne PAS toucher** : `net_http.c`, `net_socket*.c`, `configuration.c`, `menu_setting.c` — d'où le client HTTP maison et une config séparée (§3).
- **10 fichiers partagés touchés**, tous listés avec leur contenu exact dans **`rom_library/INTEGRATION.md` = source de vérité des hooks**. Après un rebase, un conflit sur l'un d'eux se résout en réappliquant cette checklist. **La maintenir à jour à chaque nouveau hook.**
- **Le hook build vit dans `Makefile.common`**, jamais dans un makefile de plateforme : la build officielle (`./configure && make`, cf. §4) passe par `Makefile` → `Makefile.common` et ignore `Makefile.win`. Les makefiles de plateforme qui incluent `Makefile.common` récupèrent nos objets tout seuls — **ne jamais les y redéclarer** (doublon au link).
- **`griffin/griffin.c` n'est PAS touché** (voie *unity* console, hors cible).
- Les entrées de menu par système / par ROM sont **générées au runtime** → jamais un enum par jeu.

```
rom_library/
├── rom_library.c/.h          # modèle catalogue (singleton rom_library_get_global()) + état
├── rom_library_webdav.c/.h   # PROPFIND : parsing rxml
├── rom_library_http.c/.h     # client HTTPS autonome (net_socket + net_socket_ssl) : GET streaming + PROPFIND
├── rom_library_config.c/.h   # rom_library.cfg (url/username/password)
├── rom_library_task.c/.h     # tâches async : listing, download, pipeline post-DL, watcher vignette
├── rom_library_menu.c/.h     # displaylists + callbacks (nav, OSK)
└── INTEGRATION.md
```

## 3. Contraintes techniques structurantes

- **Client HTTP maison obligatoire** : `net_http.c` bufferise tout le corps en RAM, cape à 256 Mio (`NET_HTTP_MAX_CONTENT_LENGTH`) et compte en `size_t` (déborde à 4 Go en 32 bits) → inutilisable pour des ROMs multi-Go sur RPi. `rom_library_http.c` streame vers disque : **compteurs `int64_t` uniquement**, buffer de réception fixe, `.part` → rename atomique, précheck d'espace disque, annulation coopérative. Validé octet-parfait jusqu'à **7,5 Gio** (ZIP64).
- **Réglages dans `rom_library.cfg`** (pas `retroarch.cfg`) : `rom_library_url`, `rom_library_username`, `rom_library_password`. Saisie OSK ; mot de passe masqué à l'affichage mais **stocké en clair** (comme Cloud Sync).
- **Auth Basic** sur chaque requête (PROPFIND + GET), base64 via `encodings/base64.h` (modèle : `network/cloud_sync/webdav.c`). HTTPS fourni par `HAVE_BUILTINMBEDTLS` (§4).
- **Tout passe par la `task_queue`**, jamais le thread principal. Callbacks sur thread worker → copier les chaînes avant push, **jamais de pointeur catalogue cross-thread**.

## 4. Build

**Une seule chaîne : `./build_dist.sh`** (MSYS2 + `./configure`). Lire son en-tête. Elle reproduit la chaîne officielle du buildbot et de la version Steam : le système **`qb`** (`./configure`) **autodétecte les libs installées et active seul tous les `HAVE_*`** — Vulkan + D3D10/11/12 (⇒ les cores *hw* démarrent), CHD + FLAC (⇒ `.chd` lisibles), ffmpeg, SDL2, CD-ROM, slang/glslang, freetype, LANGEXTRA, et `RPNG/RJPEG/RBMP/RTGA`, `STB_VORBIS`, `ONLINE_UPDATER`+`UPDATE_*` déjà `yes` par défaut (`qb/config.params.sh`). **Aucun flag à forcer à la main.**

```bash
MSYS=/c/Users/didie/scoop/apps/msys2/current
"$MSYS/usr/bin/bash.exe" -c './build_dist.sh'
"$MSYS/usr/bin/bash.exe" -c './build_dist.sh --clean'   # après un build interrompu
```
**C'est un script de RELEASE** : il compile, empaquette, puis **purge toute trace** (`dist/`, `obj-unix/`, `retroarch.exe`, `config.mk`, `config.h`). Son unique livrable est l'archive `RetroArch-<version>-g<commit>-win64-portable.zip` à la racine (gitignorée par `*.zip`), qui contient un dossier `RetroArch/` = `retroarch.exe` + ~113 DLL. Windows est **portable par nature** (tous les dossiers par défaut dérivent de celui de l'exe, `platform_win32.c`) : dézipper où l'on veut suffit, aucun fichier marqueur. Assets/cores/info/databases **non empaquetés** : on les prend via l'*Online Updater*.

> **Boucle de dev ≠ release** : `build_dist.sh` ne laisse aucun `dist/retroarch.exe` à lancer et repart donc de zéro à chaque fois. Pour coder/valider l'IHM, un **`build_dev.sh`** (même chaîne `qb`, mais incrémental et **sans purge**) est à créer à la prochaine feature. En attendant : dézipper l'archive pour tester.
>
> Prérequis supplémentaire : le paquet MSYS2 **`zip`** (`pacman -S zip` ; `tar` seul ne produit pas de `.zip`). Le script le vérifie **avant** de compiler.

> Historique : un `Makefile.dev` (wrapper de `Makefile.win`, MinGW/CLion) a existé comme boucle de dev rapide. **Supprimé** : chemin dégradé (ni Vulkan, ni ffmpeg, ni CHD ; chaque `HAVE_*` à forcer à la main, features compilées en no-op silencieux) pour un seul avantage désormais caduc — son binaire console, cf. *Debug* ci-dessous. Ne pas le ressusciter.

**Prérequis (une fois)** — MSYS2 (`scoop install msys2`), puis dans son shell :
```bash
pacman -Syuu && pacman -S --needed base-devel git zip mingw-w64-x86_64-toolchain \
  mingw-w64-x86_64-{pkgconf,zlib,libpng,freetype,SDL2,ffmpeg,x264,flac,libusb,openssl,vulkan-headers,vulkan-loader}
```

**⚠️ 2 pièges d'environnement**, tous deux à erreur trompeuse (`build_dist.sh` les neutralise déjà) :
- **`C:\Windows\System32` doit rester dans le `PATH`** : `windres` lance son préprocesseur via `popen()` → `cmd.exe`. Sans lui : `windres: can't popen ... : No error` (qui ne mentionne ni `cmd.exe` ni le PATH).
- **`TMP`/`TEMP` hérités de Windows peuvent pointer sur `C:\WINDOWS`**, où gcc ne peut pas écrire → `Cannot create temporary file … Permission denied`, que `./configure` ne rapporte que par un laconique **« Cannot proceed without a working C compiler »** (alors que `gcc --version` marche).

> **Méthodo — leçon coûteuse** : une feature **flagrante et transversale** qui ne marche pas (aucune vignette, aucune icône, aucun son, core *hw* qui refuse de démarrer) = **suspecter le build AVANT le code** (un `HAVE_*` manquant compile en no-op muet). C'est précisément pour ça qu'il n'y a plus qu'une chaîne, exhaustive.

**Prérequis runtime au 1er lancement** : *Online Updater* → Update Assets + Update Databases + Update Core Info Files ; puis *Settings → Drivers → Menu → `ozone`*. Le scan ne matche que si `.rdb` **et** `.info` sont présents (le core du système, lui, n'est pas requis — §5).

**Debug** — pas besoin d'un binaire console dédié : `retroarch.exe -v` (celui de l'archive dézippée) **écrit ses logs dans le terminal appelant** (le mode verbeux déclenche `AttachConsole(ATTACH_PARENT_PROCESS)`, `frontend/drivers/platform_win32.c`). Sans `-v`, `RARCH_LOG/WARN/ERR` sont **muets** → un log vide ne prouve rien. Pour des symboles : `make DEBUG=1` (`-O0 -g`, objets dans `obj-unix/debug/`, séparés du release).
**Debug du scan** : pas de `[Scanner] Scanning of directory finished` = le scan n'atteint jamais `MANUAL_SCAN_END`. Pour les traces internes : recompiler le **seul** objet `tasks/task_database.o` avec `-DDEBUG` + `frontend_log_level = "0"` dans `retroarch.cfg` (puis **restaurer**).

## 5. Pièges à ne pas redécouvrir

**PROPFIND / rxml** (structure réelle de rclone, namespace `D:`)
- `Depth: 1` (jamais `infinity`). Encoder l'URL **segment par segment** (`net_http_urlencode_full`), sans toucher aux `/`.
- Un `<response>` peut avoir **plusieurs `<propstat>`** : sur un **dossier**, `getcontentlength` arrive dans un propstat en **404** (vide) → ne jamais le lire comme une taille. Dossier ⇔ `<resourcetype>` contient `<collection/>`.
- `href` est URL-encodé → **le garder tel quel** pour l'URL de download ; `displayname` est déjà décodé → libellé.
- La 1re `<response>` est la collection demandée elle-même → l'ignorer.

**Menu**
- Pages poussées en liste générique différée (`generic_action_ok_displaylist_push(…, ACTION_OK_DL_GENERIC)`) : le **label** choisit la page (`rom_library_menu_deferred_push`), le **path** porte l'index (relu via `menu->deferred_path`). Aucun enum `ACTION_OK_DL_*` ajouté.
- Nos lignes sont en `FILE_TYPE_DOWNLOAD_URL` (pour afficher l'`alt`), or ce type binde Back sur `action_cancel_core_content` (qui flush vers *Add Content*) : d'où le hook `menu_cbs_cancel.c` qui re-binde `action_cancel_pop_default`.
- Le fetch réseau est déclenché **dans l'action OK**, jamais dans le build de displaylist (sinon boucle). Échec réseau/XML ⇒ cache vidé (jamais de contenu périmé).
- Rafraîchissement live des marqueurs (« ↓ » en cours / « ✓ » présent) via `retro_task::progress_cb` (thread principal, throttlé 1 s) + un refresh final ; ne rafraîchit que si le haut de pile est la page ROMs.
- **Annuler un download = re-valider OK sur la ROM** (pas de task manager dans ce build). Localiser la tâche par `(sys_index, rom_index)` via `task_queue_find`, puis **annuler HORS du finder** : `find` tient `running_lock` que `cancel` reprendrait → **deadlock**.

**Pipeline post-download** (`rom_library_download_callback` succès → extraction → scan → playlist → vignette)
- **Verrou unique `rom_library_download_busy`** : pris au push du download, **transmis** download → décompression → watcher, relâché dans le `cleanup` du watcher. **Toute branche qui n'enchaîne pas doit rendre le verrou**, sinon plus aucun download n'est possible.
- **Décompression d'abord** : `task_push_decompress` vers un dossier frère `<ROM sans extension>/`, puis suppression de l'archive. Formats du build : **zip / apk / 7z** (`.rar` n'existe nulle part dans RetroArch). Format inconnu ⇒ `task_push_decompress` renvoie **NULL** → sert de test « ce n'est pas une archive » → repli : scanner le fichier tel quel.
- **⚠️ PIÈGE MAJEUR — `file_exts` vide ≠ « tout lister »** : `database_info_dir_init` substitue alors `core_info_list->all_ext` (extensions des **cores installés**) ⇒ système sans core = **liste vide** ⇒ le scan sort sans playlist, **sans erreur ni log**. Fix : `rom_library_collect_content_exts` liste le dossier extrait, déduplique les extensions, écarte les sidecars (`txt|nfo|jpg|sbi|sub|ccd|…`) et les écrit dans `file_exts_custom`. Orthogonal à *Scan Without Core Match* (qui n'agit qu'**après** le listing).
- **Scan en mode LOOSE** avec `db_selection=SPECIFIC(<système>)` : l'entrée est ajoutée **même sans match `.rdb`**, avec `db_name=<système>` — **crucial**, car le menu en dérive le dossier des vignettes. `AUTOMATIC` force `db_usage=STRICT` ⇒ inutilisable ; en `CUSTOM`, les champs du `scan_settings` partagé **ne sont pas réinitialisés** → tout normaliser explicitement avant le push.
- **`scan_without_core_match` est lu synchroniquement au push** → on le force à ON juste autour du push, puis on le restaure (sans toucher upstream ni la préférence utilisateur). Il lève le verrou *core installé* ; le LOOSE lève le verrou *match `.rdb`*.
- **Vignette** : `task_push_manual_content_scan` **ne propage aucun callback de fin** → tâche « watcher » maison qui sonde la playlist (1 s, plafond 180 s) puis pousse `task_push_pl_entry_thumbnail_download`. La playlist n'est écrite qu'**à la toute fin** du scan (`MANUAL_SCAN_END`) : un sondage vide n'est pas un échec. Le watcher sort sur `found` **ou** `grew` (playlist plus longue que le baseline).
- **Matching d'entrée playlist** : `path_basename_nocompression` puis trimmer `#` — `path_basename` coupe au `#` et renvoie le membre interne d'archive, il ne matcherait jamais. Le scan ciblant un **dossier**, le watcher matche en mode `by_dir` (préfixe insensible à la casse et aux `/` vs `\`).
- **Déjà téléchargé (« ✓ »)** : fichier présent (et taille = taille serveur si connue) **OU** son dossier d'extraction existe (l'archive ayant été supprimée).

## 6. Conventions

- **Git : l'utilisateur commite lui-même.** Claude ne lance **jamais** `git commit`, `git push`, `git tag`, ni rien qui réécrive l'historique — même après une validation IHM réussie. Il laisse le working tree propre, résume les fichiers modifiés, et propose un message de commit si on le lui demande.
- **C89/C99**, pas de C++ ; déclarations en tête de bloc ; en-têtes en `RETRO_BEGIN/END_DECLS` ; logs `RARCH_LOG/WARN/ERR` ; respecter le formatage voisin (pas de reformatage massif). Chaînes UI dans `intl/msg_hash_us.h` (EN = référence).
- Chemins Windows : `\` doublé en JSON (`.lpl`).
- **GPLv3** (publier les sources de toute distribution binaire) ; les ROMs sont du ressort de l'utilisateur.

## 7. Références

- RetroArch https://github.com/libretro/RetroArch · libretro-database https://github.com/libretro/libretro-database · thumbnails https://github.com/libretro-thumbnails
- Docs : ROMs/Playlists/Thumbnails https://docs.libretro.com/guides/roms-playlists-thumbnails/ · rclone webdav https://rclone.org/commands/rclone_serve_webdav/
- Modèles internes lus (à ne pas modifier) : `tasks/task_core_updater.c` + `core_updater_list.c` (liste distante → menu → download), `network/cloud_sync/webdav.c` (Basic auth), `tasks/task_database.c` + `manual_content_scan.c` (scan), `tasks/task_pl_thumbnail_download.c` (vignettes).
