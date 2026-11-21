(function () {
    const translations = {
        "en": {
            "app": {
                "title": "show23-pc launcher"
            },
            "window": {
                "minimize": "Minimize",
                "maximize": "Maximize",
                "restore": "Restore",
                "close": "Close"
            },
            "launcherUpdate": {
                "available": "Update available: show23-pc launcher {{version}}",
                "downloading": "Downloading show23-pc launcher {{version}}… {{percent}}%",
                "ready": "show23-pc launcher {{version}} is ready. It installs when the launcher restarts.",
                "failed": "The update failed: {{error}}",
                "upToDate": "show23-pc launcher is up to date.",
                "checking": "Checking for updates…",
                "checkFailed": "Could not check for updates: {{error}}",
                "devBuild": "Development build. Automatic installation is disabled.",
                "offline": "Offline mode: no update checks.",
                "update": "Update",
                "download": "Download",
                "restartNow": "Restart now",
                "tryAgain": "Try again",
                "releaseNotes": "Release notes",
                "later": "Later",
                "check": "Check for updates",
                "confirmTitle": "Update show23-pc launcher",
                "confirmBody": "Version {{version}} is available (you have {{current}}).<br><br>The launcher downloads it now ({{size}}), checks it against the release’s SHA-256 and installs it the next time it starts. Your settings, games, mods and builds are not touched.",
                "confirmManual": "Version {{version}} is available (you have {{current}}).<br><br>This launcher keeps its data in AppData rather than next to the exe, so it can’t update itself. Download the new portable zip from the release page.",
                "notNow": "Not now",
                "busyTitle": "Not yet",
                "restarting": "Restarting to install the update…",
                "updated": "show23-pc launcher was updated to {{version}}.",
                "availableCached": "Update available: show23-pc launcher {{version}} (the last beta found; GitHub could not be reached: {{error}})",
                "upToDateCached": "show23-pc launcher is up to date (the last beta found; GitHub could not be reached: {{error}}).",
                "aheadOfStable": "You are on the beta {{version}}, newer than the latest release ({{latest}})."
            },
            "updateChannel": {
                "section": "Updates",
                "label": "Update channel",
                "help": "Beta gets test versions early; they may have bugs. You can switch back to Stable any time.",
                "stable": "Stable",
                "beta": "Beta",
                "badge": "Beta",
                "footer": "Update channel: {{channel}}",
                "itemLauncher": "show23-pc launcher",
                "devBuild": "{{version}} (development build)",
                "betaAvailable": "{{version}} available",
                "checking": "Checking the channel…",
                "offline": "Offline mode: the channel is not checked.",
                "fromCache": "GitHub could not be reached ({{error}}). Using the last beta found ({{version}}).",
                "aheadNote": "You are on a beta ({{current}}), newer than the latest release ({{stable}}). It stays until you switch back.",
                "nowBeta": "Update channel: Beta. Test versions install like updates.",
                "nowStable": "Update channel: Stable.",
                "switchBackTitle": "You are on a beta",
                "switchBackBody": "You're on a beta ({{current}}). Switch back to {{stable}}?",
                "switchBackDetail": "Switch back reinstalls the stable versions; your settings, games, mods and builds are kept. Keep the beta changes nothing until a newer release comes out.",
                "switchBackYes": "Switch back",
                "keepBeta": "Keep the beta",
                "switchBackButton": "Switch back to Stable",
                "fromCacheRepo": "{{name}}: GitHub could not be reached ({{error}}). Using the last result ({{version}}).",
                "checkFailedRepo": "{{name}}: could not check ({{error}}), and there is no earlier result to use. Nothing was changed; it is checked again later.",
                "resultTitle": "Switch back to Stable",
                "resultIntro": "Not everything switched back:",
                "resultDone": "{{name}}: now {{version}}.",
                "resultLauncherReady": "{{name}} {{version}}: downloaded; it installs when the launcher restarts ({{button}}).",
                "resultFailed": "{{name}}: still {{version}} ({{reason}}).",
                "resultRetry": "What did not switch back is as it was. Switch back to Stable here asks again when you want to retry.",
                "resultAllDone": "Switched back to Stable.",
                "reasonCancelled": "cancelled",
                "reasonBusy": "it could not start now",
                "reasonUnknown": "it did not finish"
            },
            "nav": {
                "home": "Home",
                "library": "Library",
                "downloads": "Downloads",
                "support": "Support",
                "settings": "Settings",
                "game": "MLB The Show 23"
            },
            "common": {
                "clear": "Clear",
                "ok": "OK",
                "cancel": "Cancel",
                "play": "Play",
                "verify": "Verify Files",
                "install": "Install",
                "installing": "Installing",
                "verifying": "Verifying",
                "uninstalling": "Uninstalling",
                "queued": "Queued",
                "reinstall": "Reinstall",
                "manageInstall": "Manage Install",
                "setup": "Setup",
                "finishSetup": "Finish Setup",
                "setUp": "Set up",
                "stop": "Stop",
                "uninstall": "Uninstall",
                "browse": "Browse",
                "browseLocalFiles": "Browse Local Files",
                "saveSettings": "Save Settings",
                "resetSettings": "Reset Settings",
                "continue": "Continue",
                "loading": "Loading...",
                "error": "Error",
                "pinToHome": "Pin to home",
                "unpinFromHome": "Unpin from home",
                "hideFromLibrary": "Hide game",
                "unhideGame": "Unhide game",
                "gameDetails": "Game details",
                "createShortcut": "Create shortcut",
                "comingSoon": "Coming soon"
            },
            "mods": {
                "title": "Mods",
                "loading": "Loading mods…",
                "addZip": "Add mod (.zip)",
                "addFolder": "Add mod folder",
                "openFolder": "Open mods folder",
                "openModFolder": "Open this mod’s folder",
                "refresh": "Refresh",
                "refreshHint": "Show mods added or removed outside the launcher",
                "orderHint": "Manage content packs in this install’s data/mods folder. Changes are saved locally.",
                "empty": "No mods yet. Add one from a .zip or a folder, or copy mod folders into the mods folder.",
                "setUpFirst": "Choose an install folder on the game’s Set up screen first.",
                "appliesNextLaunch": "The game is running: changes apply the next time you start it.",
                "enable": "Enable",
                "disable": "Disable",
                "moveUp": "Load earlier",
                "moveDown": "Load later (wins over mods above)",
                "remove": "Remove",
                "removeTitle": "Remove mod",
                "removeBody": "Delete “{{name}}” and its files from the mods folder? The game’s own files are not affected.",
                "removed": "Removed {{name}}",
                "removeFailed": "The mod could not be removed.",
                "version": "v{{version}}",
                "by": "by {{author}}",
                "fileCount": "{{count}} files, {{size}}",
                "fileCountOne": "1 file, {{size}}",
                "pickZip": "Choose a mod (.zip)",
                "zipFilter": "Zip archives",
                "installing": "Installing {{name}}…",
                "installed": "Installed {{name}}",
                "installFailed": "The mod could not be installed.",
                "saveFailed": "The change could not be saved."
            },
            "game": {
                "ready": "Ready",
                "notReady": "Not set up",
                "setup": "Set up",
                "play": "Play",
                "verify": "Verify Files",
                "verifying": "Verifying runtime files…",
                "mods": "Mods",
                "setupPending": "Setup is not implemented in this version."
            },
            "setup": {
                "title": "Set up MLB The Show 23",
                "base": "Base game",
                "package": "Choose package",
                "folder": "Choose unpacked folder",
                "update": "1.21 update package (optional)",
                "pickUpdate": "Choose update package",
                "install": "Install folder",
                "browse": "Browse…",
                "continue": "Continue"
            },
            "settings": {
                "title": "Settings",
                "general": "General",
                "language": "Language",
                "theme": "Theme",
                "defaultTheme": "Default",
                "darkTheme": "Dark",
                "navyTheme": "Navy",
                "offline": "Offline mode",
                "offlineActive": "Offline mode is active.",
                "restartOnline": "Restart online",
                "mode": "Data location",
                "portable": "Portable",
                "installed": "Installed",
                "applyMode": "Change location and restart",
                "restart": "Restart",
                "restartOfflineBody": "Restart the launcher to go offline.",
                "updates": "Updates",
                "check": "Check for updates",
                "checking": "Checking for updates…",
                "noVersion": "Not installed",
                "saveError": "Could not save the setting.",
                "launcher": "Launcher",
                "emulator": "Emulator build",
                "server": "Local server",
                "profile": "Game profile",
                "goStable": "Switch back to Stable",
                "keepBeta": "Keep the beta",
                "confirmDowngrade": "Switch from {{current}} to {{target}}?",
                "downgradeFailed": "The Stable release changed or cannot be installed."
            },
            "support": {
                "title": "Support",
                "diagnostics": "Diagnostics",
                "copy": "Copy diagnostics",
                "refresh": "Refresh",
                "logs": "Open logs folder",
                "copied": "Diagnostics copied.",
                "copyFailed": "Could not copy diagnostics.",
                "error": "Could not load the requested information.",
                "notice": "Review the report before sharing it. The launcher keeps it on this device."
            }
        },
        "fr": {
            "app": {
                "title": "show23-pc launcher"
            },
            "updateChannel": {
                "section": "Mises a jour",
                "label": "Canal de mise a jour",
                "help": "Beta recoit les versions de test en avance ; elles peuvent contenir des bugs. Vous pouvez revenir a Stable a tout moment.",
                "stable": "Stable",
                "beta": "Beta",
                "badge": "Beta",
                "footer": "Canal de mise a jour : {{channel}}",
                "itemLauncher": "show23-pc launcher",
                "devBuild": "{{version}} (version de developpement)",
                "betaAvailable": "{{version}} disponible",
                "checking": "Verification du canal…",
                "offline": "Mode hors ligne : le canal n'est pas verifie.",
                "fromCache": "GitHub n'a pas pu etre contacte ({{error}}). Derniere beta trouvee utilisee ({{version}}).",
                "aheadNote": "Vous etes sur une beta ({{current}}), plus recente que la derniere version ({{stable}}). Elle reste jusqu'a ce que vous reveniez a Stable.",
                "nowBeta": "Canal de mise a jour : Beta. Les versions de test s'installent comme des mises a jour.",
                "nowStable": "Canal de mise a jour : Stable.",
                "switchBackTitle": "Vous etes sur une beta",
                "switchBackBody": "Vous etes sur une beta ({{current}}). Revenir a {{stable}} ?",
                "switchBackDetail": "Revenir reinstalle les versions stables ; vos parametres, jeux, mods et builds sont conserves. Garder la beta ne change rien jusqu'a la prochaine version.",
                "switchBackYes": "Revenir",
                "keepBeta": "Garder la beta",
                "switchBackButton": "Revenir a Stable",
                "fromCacheRepo": "{{name}} : GitHub n'a pas pu etre contacte ({{error}}). Dernier resultat utilise ({{version}}).",
                "checkFailedRepo": "{{name}} : verification impossible ({{error}}), et aucun resultat precedent n'est disponible. Rien n'a change ; une nouvelle verification aura lieu plus tard.",
                "resultTitle": "Revenir a Stable",
                "resultIntro": "Tout n'est pas revenu a Stable :",
                "resultDone": "{{name}} : maintenant {{version}}.",
                "resultLauncherReady": "{{name}} {{version}} : telecharge ; il s'installe au redemarrage du lanceur ({{button}}).",
                "resultFailed": "{{name}} : toujours {{version}} ({{reason}}).",
                "resultRetry": "Ce qui n'est pas revenu reste tel quel. Revenir a Stable, ici, redemande quand vous voulez reessayer.",
                "resultAllDone": "Revenu a Stable.",
                "reasonCancelled": "annule",
                "reasonBusy": "impossible de demarrer maintenant",
                "reasonUnknown": "ne s'est pas termine"
            },
            "launcherUpdate": {
                "availableCached": "Mise a jour disponible : show23-pc launcher {{version}} (derniere beta trouvee ; GitHub injoignable : {{error}})",
                "upToDateCached": "show23-pc launcher est a jour (derniere beta trouvee ; GitHub injoignable : {{error}}).",
                "aheadOfStable": "Vous utilisez la beta {{version}}, plus recente que la derniere version ({{latest}})."
            },
            "window": {
                "minimize": "Reduire",
                "maximize": "Agrandir",
                "restore": "Restaurer",
                "close": "Fermer"
            },
            "nav": {
                "home": "Accueil",
                "library": "Bibliotheque",
                "downloads": "Telechargements",
                "support": "Support",
                "settings": "Parametres",
                "game": "MLB The Show 23"
            },
            "common": {
                "ok": "OK",
                "cancel": "Annuler",
                "play": "Jouer",
                "verify": "Verifier les fichiers",
                "install": "Installer",
                "installing": "Installation...",
                "verifying": "Verification...",
                "uninstalling": "Desinstallation...",
                "queued": "En file",
                "reinstall": "Reinstaller",
                "manageInstall": "Gerer l'installation",
                "setup": "Configurer",
                "finishSetup": "Terminer la configuration",
                "setUp": "Configurer",
                "stop": "Arreter",
                "uninstall": "Desinstaller",
                "browse": "Parcourir",
                "browseLocalFiles": "Parcourir les fichiers locaux",
                "saveSettings": "Enregistrer",
                "resetSettings": "Reinitialiser",
                "continue": "Continuer",
                "loading": "Chargement...",
                "error": "Erreur",
                "pinToHome": "Epingler a l'accueil",
                "unpinFromHome": "Detacher de l'accueil",
                "hideFromLibrary": "Masquer le jeu",
                "unhideGame": "Afficher le jeu",
                "gameDetails": "Details du jeu",
                "createShortcut": "Creer un raccourci",
                "comingSoon": "Bientot disponible"
            },
            "game": {
                "ready": "Prêt",
                "notReady": "Non configuré",
                "setup": "Configurer",
                "play": "Jouer",
                "verify": "Vérifier les fichiers",
                "mods": "Mods",
                "setupPending": "La configuration n’est pas disponible dans cette version."
            },
            "settings": {
                "title": "Paramètres",
                "general": "Général",
                "language": "Langue",
                "theme": "Thème",
                "offline": "Mode hors ligne",
                "updates": "Mises à jour",
                "check": "Rechercher des mises à jour",
                "noVersion": "Non installé"
            },
            "setup": {
                "title": "Configurer MLB The Show 23",
                "base": "Jeu de base",
                "package": "Choisir un paquet",
                "folder": "Choisir un dossier extrait",
                "update": "Paquet de mise à jour 1.21 (facultatif)",
                "pickUpdate": "Choisir la mise à jour",
                "install": "Dossier d’installation",
                "browse": "Parcourir…",
                "continue": "Continuer"
            }
        },
        "es": {
            "app": {
                "title": "show23-pc launcher"
            },
            "updateChannel": {
                "section": "Actualizaciones",
                "label": "Canal de actualizacion",
                "help": "Beta recibe versiones de prueba antes; pueden tener errores. Puedes volver a Estable cuando quieras.",
                "stable": "Estable",
                "beta": "Beta",
                "badge": "Beta",
                "footer": "Canal de actualizacion: {{channel}}",
                "itemLauncher": "show23-pc launcher",
                "devBuild": "{{version}} (version de desarrollo)",
                "betaAvailable": "{{version}} disponible",
                "checking": "Comprobando el canal…",
                "offline": "Modo sin conexion: el canal no se comprueba.",
                "fromCache": "No se pudo contactar con GitHub ({{error}}). Se usa la ultima beta encontrada ({{version}}).",
                "aheadNote": "Estas en una beta ({{current}}), mas nueva que la ultima version ({{stable}}). Se queda hasta que vuelvas a Estable.",
                "nowBeta": "Canal de actualizacion: Beta. Las versiones de prueba se instalan como actualizaciones.",
                "nowStable": "Canal de actualizacion: Estable.",
                "switchBackTitle": "Estas en una beta",
                "switchBackBody": "Estas en una beta ({{current}}). ¿Volver a {{stable}}?",
                "switchBackDetail": "Volver reinstala las versiones estables; tus ajustes, juegos, mods y compilaciones se conservan. Mantener la beta no cambia nada hasta la proxima version.",
                "switchBackYes": "Volver",
                "keepBeta": "Mantener la beta",
                "switchBackButton": "Volver a Estable",
                "fromCacheRepo": "{{name}}: no se pudo contactar con GitHub ({{error}}). Se usa el ultimo resultado ({{version}}).",
                "checkFailedRepo": "{{name}}: no se pudo comprobar ({{error}}) y no hay un resultado anterior. No se cambio nada; se volvera a comprobar mas tarde.",
                "resultTitle": "Volver a Estable",
                "resultIntro": "No todo volvio a Estable:",
                "resultDone": "{{name}}: ahora {{version}}.",
                "resultLauncherReady": "{{name}} {{version}}: descargado; se instala cuando el launcher se reinicie ({{button}}).",
                "resultFailed": "{{name}}: sigue en {{version}} ({{reason}}).",
                "resultRetry": "Lo que no volvio queda como estaba. Volver a Estable, aqui, vuelve a preguntar cuando quieras reintentarlo.",
                "resultAllDone": "Has vuelto a Estable.",
                "reasonCancelled": "cancelado",
                "reasonBusy": "no se pudo iniciar ahora",
                "reasonUnknown": "no termino"
            },
            "launcherUpdate": {
                "availableCached": "Actualizacion disponible: show23-pc launcher {{version}} (ultima beta encontrada; GitHub no responde: {{error}})",
                "upToDateCached": "show23-pc launcher esta actualizado (ultima beta encontrada; GitHub no responde: {{error}}).",
                "aheadOfStable": "Usas la beta {{version}}, mas nueva que la ultima version ({{latest}})."
            },
            "window": {
                "minimize": "Minimizar",
                "maximize": "Maximizar",
                "restore": "Restaurar",
                "close": "Cerrar"
            },
            "nav": {
                "home": "Inicio",
                "library": "Biblioteca",
                "downloads": "Descargas",
                "support": "Soporte",
                "settings": "Ajustes",
                "game": "MLB The Show 23"
            },
            "common": {
                "ok": "OK",
                "cancel": "Cancelar",
                "play": "Jugar",
                "verify": "Verificar archivos",
                "install": "Instalar",
                "installing": "Instalando",
                "verifying": "Verificando",
                "uninstalling": "Desinstalando",
                "queued": "En cola",
                "reinstall": "Reinstalar",
                "manageInstall": "Gestionar instalacion",
                "setup": "Configurar",
                "finishSetup": "Terminar configuracion",
                "setUp": "Configurar",
                "stop": "Detener",
                "uninstall": "Desinstalar",
                "browse": "Explorar",
                "browseLocalFiles": "Explorar archivos locales",
                "saveSettings": "Guardar ajustes",
                "resetSettings": "Restablecer ajustes",
                "continue": "Continuar",
                "loading": "Cargando...",
                "error": "Error",
                "pinToHome": "Anclar al inicio",
                "unpinFromHome": "Desanclar del inicio",
                "hideFromLibrary": "Ocultar juego",
                "unhideGame": "Mostrar juego",
                "gameDetails": "Detalles del juego",
                "createShortcut": "Crear acceso directo",
                "comingSoon": "Proximamente"
            },
            "game": {
                "ready": "Listo",
                "notReady": "Sin configurar",
                "setup": "Configurar",
                "play": "Jugar",
                "verify": "Verificar archivos",
                "mods": "Mods",
                "setupPending": "La configuración no está disponible en esta versión."
            },
            "settings": {
                "title": "Ajustes",
                "general": "General",
                "language": "Idioma",
                "theme": "Tema",
                "offline": "Modo sin conexión",
                "updates": "Actualizaciones",
                "check": "Buscar actualizaciones",
                "noVersion": "Sin instalar"
            },
            "setup": {
                "title": "Configurar MLB The Show 23",
                "base": "Juego base",
                "package": "Elegir paquete",
                "folder": "Elegir carpeta extraída",
                "update": "Paquete de actualización 1.21 (opcional)",
                "pickUpdate": "Elegir actualización",
                "install": "Carpeta de instalación",
                "browse": "Examinar…",
                "continue": "Continuar"
            }
        },
        "ru": {
            "app": {
                "title": "show23-pc launcher"
            },
            "window": {
                "minimize": "Свернуть",
                "maximize": "Развернуть",
                "restore": "Восстановить",
                "close": "Закрыть"
            },
            "nav": {
                "home": "Главная",
                "library": "Библиотека",
                "downloads": "Загрузки",
                "support": "Поддержка",
                "settings": "Настройки",
                "game": "MLB The Show 23"
            },
            "common": {
                "ok": "ОК",
                "cancel": "Отмена",
                "play": "Играть",
                "verify": "Проверить файлы",
                "install": "Установить",
                "installing": "Установка",
                "verifying": "Проверка",
                "uninstalling": "Удаление",
                "queued": "В очереди",
                "reinstall": "Переустановить",
                "manageInstall": "Управление установкой",
                "setup": "Настройка",
                "finishSetup": "Завершить настройку",
                "setUp": "Настроить",
                "stop": "Остановить",
                "uninstall": "Удалить",
                "browse": "Обзор",
                "browseLocalFiles": "Просмотр локальных файлов",
                "saveSettings": "Сохранить настройки",
                "resetSettings": "Сбросить настройки",
                "continue": "Продолжить",
                "loading": "Загрузка...",
                "error": "Ошибка",
                "pinToHome": "Закрепить на главной",
                "unpinFromHome": "Открепить от главной",
                "hideFromLibrary": "Скрыть игру",
                "unhideGame": "Показать игру",
                "gameDetails": "Подробности игры",
                "createShortcut": "Создать ярлык",
                "comingSoon": "Скоро"
            },
            "game": {
                "ready": "Готово",
                "notReady": "Не настроено",
                "setup": "Настроить",
                "play": "Играть",
                "verify": "Проверить файлы",
                "mods": "Моды",
                "setupPending": "Настройка недоступна в этой версии."
            },
            "settings": {
                "title": "Настройки",
                "general": "Общие",
                "language": "Язык",
                "theme": "Тема",
                "offline": "Автономный режим",
                "updates": "Обновления",
                "check": "Проверить обновления",
                "noVersion": "Не установлено"
            },
            "setup": {
                "title": "Настроить MLB The Show 23",
                "base": "Базовая игра",
                "package": "Выбрать пакет",
                "folder": "Выбрать распакованную папку",
                "update": "Пакет обновления 1.21 (необязательно)",
                "pickUpdate": "Выбрать обновление",
                "install": "Папка установки",
                "browse": "Обзор…",
                "continue": "Продолжить"
            }
        }
    };

    let currentLanguage = 'en';

    function lookup(path, language) {
        return String(path || '')
            .split('.')
            .reduce((value, segment) => (value && value[segment] !== undefined ? value[segment] : undefined), translations[language]);
    }

    function interpolate(template, variables) {
        if (!variables) return template;

        return template.replace(/\{\{(.*?)\}\}/g, (match, key) => {
            const value = variables[key.trim()];
            return value === undefined || value === null ? '' : String(value);
        });
    }

    function t(key, variables) {
        const value = lookup(key, currentLanguage) ?? lookup(key, 'en');
        if (typeof value !== 'string') return key;
        return interpolate(value, variables);
    }

    function setLanguage(language) {
        currentLanguage = translations[language] ? language : 'en';
        document.documentElement.lang = currentLanguage;
        return currentLanguage;
    }

    function getLanguage() {
        return currentLanguage;
    }

    function getGameText(gameId, field, fallback) {
        const value = lookup(`game.${gameId}.${field}`, currentLanguage) ?? lookup(`game.${gameId}.${field}`, 'en');
        return typeof value === 'string' ? value : fallback;
    }

    function applyStaticTranslations() {
        document.title = t('app.title');

        document.querySelectorAll('[data-i18n]').forEach(element => {
            element.textContent = t(element.dataset.i18n);
        });

        document.querySelectorAll('[data-i18n-html]').forEach(element => {
            element.innerHTML = t(element.dataset.i18nHtml);
        });

        document.querySelectorAll('[data-i18n-placeholder]').forEach(element => {
            element.placeholder = t(element.dataset.i18nPlaceholder);
        });

        document.querySelectorAll('[data-i18n-title]').forEach(element => {
            element.title = t(element.dataset.i18nTitle);
        });

        const versionElement = document.getElementById('version-footer');
        if (versionElement && (!versionElement.dataset.versionLoaded || versionElement.dataset.versionLoaded === 'false')) {
            versionElement.textContent = t('settings.versionLoading');
        }
    }

    window.LauncherI18n = {
        t,
        setLanguage,
        getLanguage,
        getGameText,
        applyStaticTranslations
    };
})();
