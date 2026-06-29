#!/bin/sh
# =============================================================================
#  build_dist.sh — build de DISTRIBUTION Windows x64 « exhaustive » -> .zip
#
#  C'est LA build du fork — l'unique (validée 2026-07-12 : téléchargement WebDAV
#  -> playlist -> pochette -> jeu lancé avec swanstation). Elle reproduit la
#  chaîne OFFICIELLE de RetroArch (celle du buildbot et de la version Steam) :
#  le système `qb` (./configure) autodétecte les bibliothèques installées et
#  active seul TOUS les HAVE_* (Vulkan, D3D10/11/12, CHD, ffmpeg, SDL2...).
#
#  NB : elle passe par Makefile -> Makefile.common et IGNORE totalement
#  Makefile.win. C'est pourquoi les objets rom_library/ sont déclarés dans
#  Makefile.common (cf. rom_library/INTEGRATION.md §1).
#
#  Debug : `make DEBUG=1` (objets dans obj-unix/debug, -O0 -g). Et les logs
#  s'affichent dans le terminal appelant via `retroarch.exe -v` (une fois
#  l'archive dézippée) : le mode verbeux fait un AttachConsole
#  (frontend/drivers/platform_win32.c).
#
#  Prérequis (une seule fois) : MSYS2 + les paquets MINGW64 + le paquet `zip`
#  (pacman -S zip ; tar seul ne produit pas de .zip). Voir CLAUDE.md §4.
#
#  Usage — depuis n'importe quel shell Windows/Git-bash, à la racine du repo :
#
#    MSYS=/c/Users/didie/scoop/apps/msys2/current
#    "$MSYS/usr/bin/bash.exe" -c './build_dist.sh'
#    "$MSYS/usr/bin/bash.exe" -c './build_dist.sh --clean'
#
#  --clean ne sert qu'à repartir de zéro après un build INTERROMPU (un run qui
#  va au bout purge déjà tout). JOBS=8 (défaut).
#
#  Résultat : UNE archive à la racine du repo —
#      RetroArch-<version>-g<commit>-win64-portable.zip
#  qui contient un dossier RetroArch/ (retroarch.exe + ses DLL). Windows est
#  portable par nature : tous les dossiers par défaut sont dérivés de celui de
#  l'exe (frontend/drivers/platform_win32.c) — dézipper où l'on veut suffit.
#  Les assets, cores, info et databases se téléchargent depuis l'IHM (Online
#  Updater) — inutile de les empaqueter.
#
#  ⚠️ C'est un script de RELEASE : une fois l'archive écrite, il PURGE toute
#  trace du build (dist/, obj-unix/, retroarch.exe, config.mk, config.h). Donc
#  chaque exécution repart forcément de zéro, et il ne reste aucun
#  dist/retroarch.exe à lancer — pour la boucle de dev (build incrémental que
#  l'on garde pour tester l'IHM), ce sera un build_dev.sh séparé.
# =============================================================================
set -eu

CLEAN=0
[ "${1:-}" = "--clean" ] && CLEAN=1
JOBS="${JOBS:-8}"

# --- Environnement MINGW64 ---------------------------------------------------
# ⚠️ PIÈGE : garder C:\Windows\System32 dans le PATH. windres lance son
# préprocesseur via popen() -> cmd.exe ; sans System32 il échoue sur un
# « can't popen ... : No error » parfaitement incompréhensible (l'erreur ne
# mentionne ni cmd.exe ni le PATH).
# ⚠️ PIÈGE : TMP/TEMP hérités de Windows peuvent pointer sur C:\WINDOWS, où gcc
# n'a pas le droit d'écrire -> « Cannot create temporary file ... Permission
# denied », que ./configure ne rapporte que par « no working C compiler ».
export MSYSTEM=MINGW64
export PATH="/mingw64/bin:/usr/bin:/c/Windows/System32:/c/Windows"
export TMP=/tmp TEMP=/tmp TMPDIR=/tmp

if [ ! -x /mingw64/bin/gcc.exe ]; then
	echo "ERREUR : toolchain MINGW64 introuvable. Lancer ce script avec le bash" >&2
	echo "         de MSYS2 (pas Git-bash) — cf. l'en-tête du script." >&2
	exit 1
fi

# Vérifié AVANT de compiler : inutile de découvrir l'absence de zip au bout
# d'un quart d'heure de make.
if ! command -v zip >/dev/null 2>&1; then
	echo "ERREUR : zip introuvable. Dans le shell MSYS2 : pacman -S zip" >&2
	exit 1
fi

# --- Configure ---------------------------------------------------------------
# --disable-qt : le « Desktop Menu » Qt n'est pas utilisé et alourdirait
# lourdement le paquet (Qt6 + ses plugins). C'est aussi ce que fait la build
# Steam officielle. Tout le reste est laissé en autodétection.
if [ "$CLEAN" = 1 ]; then
	echo ">>> clean : obj-unix/ config.mk config.h dist/ retroarch.exe"
	rm -rf obj-unix config.mk config.h dist retroarch.exe
fi

if [ ! -f config.mk ]; then
	echo ">>> ./configure --disable-qt"
	./configure --disable-qt
fi

# --- Compilation -------------------------------------------------------------
echo ">>> make -j${JOBS}"
make -j"$JOBS"

# --- Paquet portable ---------------------------------------------------------
# Fermeture TRANSITIVE des dépendances : une DLL de /mingw64 en tire d'autres
# (ffmpeg -> libass -> harfbuzz -> ...). On boucle jusqu'à point fixe ; un
# simple `ldd retroarch.exe` en oublierait la moitié.
echo ">>> collecte des DLL -> dist/"
rm -rf dist
mkdir -p dist
cp retroarch.exe dist/

prev=0
while : ; do
	for f in dist/*.exe dist/*.dll; do
		[ -e "$f" ] && ldd "$f" 2>/dev/null
	done | grep -i '/mingw64/bin' | awk '{print $3}' | sort -u > /tmp/ra_deps.txt

	while read -r dll; do
		[ -f "$dll" ] && cp -n "$dll" dist/ 2>/dev/null || true
	done < /tmp/ra_deps.txt

	n=$(ls dist/*.dll 2>/dev/null | wc -l)
	[ "$n" = "$prev" ] && break
	prev=$n
done
rm -f /tmp/ra_deps.txt

echo ">>> dist/ : $(ls dist/*.dll | wc -l) DLL, $(du -sh dist | cut -f1)"
./dist/retroarch.exe --version

# --- Archive portable --------------------------------------------------------
# Le zip doit contenir un dossier RetroArch/ à sa racine (et non le contenu en
# vrac) : on renomme dist/ plutôt que de le copier — il est purgé juste après.
# Le paquet `zip` vient de MSYS2 (pacman -S zip) ; tar seul ne fait pas de .zip.
VERSION=$(sed -n 's/^#define PACKAGE_VERSION "\(.*\)"/\1/p' version.all)
COMMIT=$(git rev-parse --short HEAD 2>/dev/null || echo nogit)
PKGDIR=RetroArch
ARCHIVE="RetroArch-${VERSION}-g${COMMIT}-win64-portable.zip"

echo ">>> archive -> ${ARCHIVE}"
rm -rf "$PKGDIR" "$ARCHIVE"
mv dist "$PKGDIR"
zip -qr9 "$ARCHIVE" "$PKGDIR"
rm -rf "$PKGDIR"

# --- Purge -------------------------------------------------------------------
# L'archive est le seul livrable : on ne laisse aucune trace du build derrière.
echo ">>> purge : obj-unix/ config.mk config.h config.log retroarch.exe dist/"
rm -rf obj-unix config.mk config.h config.log retroarch.exe dist

echo
echo "=============================================================="
echo " Archive portable : ${ARCHIVE} ($(du -h "$ARCHIVE" | cut -f1))"
echo " Dézipper n'importe où, puis lancer RetroArch/retroarch.exe."
echo "=============================================================="
echo " Au 1er lancement : Online Updater -> Update Assets + Update"
echo " Databases + Update Core Info Files, puis Settings -> Drivers"
echo " -> Menu -> ozone."
