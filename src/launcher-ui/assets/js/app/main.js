(() => {
    'use strict';

    const $ = id => document.getElementById(id);
    const run = (command, data) => window.executeCommand(command, data);
    const t = (key, values) => window.LauncherI18n.t(key, values);
    const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
    const state = { settings: {}, game: {}, sourceKind: '', checking: false };

    function status(id, text) {
        $(id).textContent = text || '';
        $(id).hidden = !text;
    }

    function bind(id, event, callback) {
        $(id).addEventListener(event, async () => {
            try {
                await callback();
            } catch (error) {
                console.error(error);
                window.showToast(t('support.error'), 'error');
            }
        });
    }

    function showPage(page) {
        for (const button of document.querySelectorAll('[data-page]')) {
            const active = button.dataset.page === page;
            $(button.dataset.page + '-page').hidden = !active;
            button.classList.toggle('active', active);
            if (active) button.setAttribute('aria-current', 'page');
            else button.removeAttribute('aria-current');
        }
    }

    async function gameState() {
        state.game = await run('get-show23') || {};
        $('game-state').textContent = t(state.game.ready ? 'game.ready' : 'game.notReady');
        $('play').disabled = !state.game.ready;
    }

    function setupInputsChanged() {
        $('setup-continue').disabled = !state.sourceKind || !$('base-path').value || !$('install-path').value;
        $('update-clear').disabled = !$('update-path').value;
        status('setup-status', '');
    }

    function closeSetup() {
        $('setup').hidden = true;
        $('base-path').value = '';
        $('update-path').value = '';
        state.sourceKind = '';
        setupInputsChanged();
        $('setup-open').focus();
    }

    async function pickPackage() {
        return run('browse-file', {
            title: t('setup.package'),
            filters: [{ name: 'Package (*.pkg)', pattern: '*.pkg' }]
        });
    }

    async function browseBase(kind) {
        const path = kind === 'package' ? await pickPackage() : await run('browse-folder');
        if (!path) return;
        state.sourceKind = kind;
        $('base-path').value = path;
        setupInputsChanged();
    }

    async function verify() {
        $('verify').disabled = true;
        status('game-status', t('game.verifying'));
        try {
            await run('verify-show23');
            for (let attempt = 0; attempt < 1800; attempt++) {
                const result = await run('get-verify') || {};
                if (!result.busy) {
                    status('game-status', result.message || t('support.error'));
                    return;
                }
                await sleep(500);
            }
            status('game-status', t('game.verifying'));
        } finally {
            $('verify').disabled = false;
        }
    }

    function drawSettings() {
        const saved = state.settings;
        window.IS_OFFLINE = !!saved.offline;
        window.LauncherI18n.setLanguage(saved.language || 'en');
        window.LauncherI18n.applyStaticTranslations();
        if (['dark', 'navy-gradient'].includes(saved.theme)) document.documentElement.dataset.theme = saved.theme;
        else delete document.documentElement.dataset.theme;
        $('language').value = window.LauncherI18n.getLanguage();
        $('theme').value = saved.theme || 'default';
        $('channel').value = saved.channel || 'stable';
        $('offline').checked = !!saved.offlineSaved;
        $('offline-status').hidden = !saved.offline;
        $('data-mode').textContent = t(saved.portable ? 'settings.portable' : 'settings.installed');
        $('check-parts').disabled = !!saved.offline || state.checking;
        window.LauncherDropdowns?.enhance(document.getElementById("settings-page"));
        window.LauncherUpdate.render();
    }

    async function save(key, value) {
        const ok = await run('save-setting', { key, value });
        status('settings-status', ok ? '' : t('settings.saveError'));
        state.settings = await run('get-settings') || {};
        drawSettings();
        return ok;
    }

    async function renderParts() {
        const installed = await run('get-parts') || {};
        $('parts').textContent = '';
        for (const item of installed.parts || []) {
            const row = document.createElement('div');
            row.className = 'part-row';
            const name = document.createElement('dt');
            name.textContent = t('settings.' + item.part);
            const data = document.createElement('dd');
            data.textContent = item.version || t('settings.noVersion');
            if (item.prerelease) {
                const badge = document.createElement('span');
                badge.className = 'part-badge';
                badge.textContent = t('updateChannel.badge');
                data.appendChild(badge);
            }
            const line = document.createElement('span');
            line.className = 'part-status';
            line.id = 'part-status-' + item.part;
            line.textContent = item.status || '';
            data.appendChild(line);
            row.appendChild(name);
            row.appendChild(data);
            $('parts').appendChild(row);
        }
        window.LauncherUpdate.render();
        return installed.busy;
    }

    async function followUpdates() {
        if (state.checking) return;
        state.checking = true;
        $('check-parts').disabled = true;
        const refresh = window.LauncherUpdate.follow();
        try {
            for (let attempt = 0; attempt < 95; attempt++) {
                if (!await renderParts()) break;
                await sleep(500);
            }
            await refresh;
        } finally {
            state.checking = false;
            $('check-parts').disabled = !!state.settings.offline;
            status('update-status', '');
            await renderParts();
        }
    }

    async function stable() {
        const saved = window.LauncherUpdate.status;
        if (!saved?.aheadOfStable || !saved.canInstall || saved.channel !== 'stable') return;
        const choice = await window.showMessageBox(
            t('settings.goStable'), t('settings.confirmDowngrade', { current: saved.current, target: saved.latest }),
            [t('settings.goStable'), { label: t('settings.keepBeta'), cancel: true }]
        );
        if (choice !== 0) {
            await run('keep-beta');
            return;
        }
        if (!await run('request-stable', { version: saved.latest })) {
            status('update-status', t('settings.downgradeFailed'));
            return;
        }
        await followUpdates();
    }

    async function diagnostics() {
        const result = await run('get-support-diagnostics');
        $('diagnostics').textContent = result?.text || '';
    }

    function wireControls() {
        for (const button of document.querySelectorAll('[data-page]')) {
            bind(button.id, 'click', async () => {
                showPage(button.dataset.page);
                if (button.dataset.page === 'settings') {
                    await window.LauncherUpdate.refresh();
                    await renderParts();
                }
                if (button.dataset.page === 'support') await diagnostics();
                if (button.dataset.page === 'show23') await gameState();
            });
        }

        bind('minimize-button', 'click', () => run('minimize'));
        bind('maximize-button', 'click', () => run('toggle-maximize'));
        bind('close-button', 'click', () => run('close'));
        window.__setMaximized = value => $('maximize-button').classList.toggle('is-maximized', value);

        bind('setup-open', 'click', () => {
            $('setup').hidden = false;
            $('install-path').value = state.game.folder || '';
            setupInputsChanged();
            $('base-package').focus();
        });
        bind('base-package', 'click', () => browseBase('package'));
        bind('base-folder', 'click', () => browseBase('folder'));
        bind('update-package', 'click', async () => {
            const path = await pickPackage();
            if (path) $('update-path').value = path;
            setupInputsChanged();
        });
        bind('update-clear', 'click', () => {
            $('update-path').value = '';
            setupInputsChanged();
        });
        bind('install-folder', 'click', async () => {
            const path = await run('browse-folder');
            if (path) $('install-path').value = path;
            setupInputsChanged();
        });
        bind('setup-cancel', 'click', closeSetup);
        bind('setup-continue', 'click', async () => {
            $('setup-continue').disabled = true;
            try {
                const reply = await run('setup-show23', {
                    kind: state.sourceKind,
                    base: $('base-path').value,
                    update: $('update-path').value,
                    install: $('install-path').value
                });
                status('setup-status', reply?.pending ? t('game.setupPending') : (reply?.message || t('support.error')));
                await gameState();
                if (!$('mods-panel').hidden) window.ModsView.render('show23');
            } finally {
                $('setup-continue').disabled = false;
            }
        });
        bind('play', 'click', async () => {
            const result = await run('play-show23');
            status('game-status', result?.message);
        });
        bind('verify', 'click', verify);
        bind('mods', 'click', () => {
            const open = $('mods-panel').hidden;
            $('mods-panel').hidden = !open;
            $('mods').setAttribute('aria-expanded', String(open));
            if (open) window.ModsView.render('show23');
        });

        for (const key of ['language', 'theme', 'channel']) {
            bind(key, 'change', async () => {
                await save(key, $(key).value);
                if (key === 'language') {
                    await gameState();
                    if (!$('mods-panel').hidden) window.ModsView.render('show23');
                }
                if (key === 'channel') await followUpdates();
                else await renderParts();
            });
        }
        bind('check-parts', 'click', async () => {
            if (!await run('check-parts')) return;
            status('update-status', t('settings.checking'));
            await followUpdates();
        });
        bind('switch-stable', 'click', stable);
        bind('offline', 'change', async () => {
            const enable = $('offline').checked;
            if (!await save('offline', String(enable)) || enable === !!state.settings.offline) return;
            if (enable) {
                const choice = await window.showMessageBox(t('settings.offline'), t('settings.restartOfflineBody'),
                    [t('settings.restart'), { label: t('common.cancel'), cancel: true }]);
                if (choice === 0) await run('relaunch');
            }
        });
        bind('restart-online', 'click', () => run('relaunch-online'));
        bind('switch-mode', 'click', async () => {
            const answer = await run('switch-portable', { portable: !state.settings.portable });
            if (!answer?.success) status('settings-status', t('settings.saveError'));
        });
        bind('refresh-diagnostics', 'click', diagnostics);
        bind('open-logs', 'click', () => run('open-logs'));
        bind('copy-diagnostics', 'click', async () => {
            const answer = await run('copy-to-clipboard', { text: $('diagnostics').textContent });
            status('support-status', t(answer?.success ? 'support.copied' : 'support.copyFailed'));
        });
    }

    async function init() {
        wireControls();
        try {
            state.settings = await run('get-settings') || {};
            drawSettings();
            await gameState();
            await window.LauncherUpdate.init();
        } catch (error) {
            console.error(error);
            status('game-status', t('support.error'));
        } finally {
            await run('show');
            await run('confirm-launcher-started');
        }
    }

    init().catch(console.error);
})();
