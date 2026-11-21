// Lists and manages local content packs in data/mods.
(function () {
    'use strict';

    const SUPPORTED = new Set(['show23']);

    const state = {};
    const escapeHtml = value => String(value ?? '').replace(/[&<>"']/g, ch =>
        ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[ch]);

    function t(key, variables) {
        return window.LauncherI18n ? window.LauncherI18n.t(key, variables) : key;
    }

    function backendId(gameId) {
        return gameId;
    }

    function run(command, payload) {
        return window.executeCommand(command, payload);
    }

    function panel(gameId) {
        return document.getElementById(`${gameId}-mods-panel`);
    }

    function getState(gameId) {
        if (!state[gameId]) {
            state[gameId] = { data: null, busy: false, importing: null };
        }
        return state[gameId];
    }

    function formatSize(bytes) {
        if (!bytes) return '0 KB';
        const units = ['B', 'KB', 'MB', 'GB'];
        let value = bytes;
        let unit = 0;
        while (value >= 1024 && unit < units.length - 1) {
            value /= 1024;
            unit++;
        }
        return `${value < 10 && unit > 0 ? value.toFixed(1) : Math.round(value)} ${units[unit]}`;
    }

    function reportError(error) {
        console.error(error);
        window.showToast(String((error && error.message) || error), 'error');
    }

    function rowHTML(mod, index, count) {
        const title = mod.title || mod.name;
        // The list is replaced after edits/refreshes. Keep each row's actions
        // distinct so controller/keyboard focus returns to the same mod.
        const focus = action => `data-focus="${escapeHtml(`mod-${mod.name}-${action}`)}"`;
        const details = [
            mod.version ? t('mods.version', { version: mod.version }) : '',
            mod.author ? t('mods.by', { author: mod.author }) : '',
            t(mod.files === 1 ? 'mods.fileCountOne' : 'mods.fileCount', { count: mod.files, size: formatSize(mod.size) })
        ].filter(Boolean).join(' · ');

        return `
            <li class="content-pack${mod.enabled ? '' : ' is-disabled'}" data-name="${escapeHtml(mod.name)}">
                <label class="content-pack-toggle" title="${escapeHtml(t(mod.enabled ? 'mods.disable' : 'mods.enable'))}">
                    <input type="checkbox" class="content-pack-enabled" ${focus('enabled')}${mod.enabled ? ' checked' : ''}>
                    <span class="content-pack-switch"></span>
                </label>
                <div class="content-pack-info">
                    <div class="content-pack-title">${escapeHtml(title)}${mod.title && mod.title !== mod.name ? ` <span class="content-pack-folder">${escapeHtml(mod.name)}</span>` : ''}</div>
                    <div class="content-pack-details">${escapeHtml(details)}</div>
                    ${mod.description ? `<div class="content-pack-description">${escapeHtml(mod.description)}</div>` : ''}
                </div>
                <div class="content-pack-order">
                    <button class="content-pack-icon-button content-pack-up" ${focus('up')} title="${escapeHtml(t('mods.moveUp'))}"${index === 0 ? ' disabled' : ''}>&#9650;</button>
                    <button class="content-pack-icon-button content-pack-down" ${focus('down')} title="${escapeHtml(t('mods.moveDown'))}"${index === count - 1 ? ' disabled' : ''}>&#9660;</button>
                </div>
                <div class="content-pack-actions">
                    <button class="secondary-action content-pack-open" ${focus('open')} title="${escapeHtml(t('mods.openModFolder'))}"><span class="secondary-action-icon folder-icon"></span></button>
                    <button class="secondary-action content-pack-remove" ${focus('remove')} title="${escapeHtml(t('mods.remove'))}">${escapeHtml(t('mods.remove'))}</button>
                </div>
            </li>`;
    }

    function render(gameId) {
        const host = panel(gameId);
        if (!host) return;
        const s = getState(gameId);

        host.innerHTML = `
            <div class="content-packs">
                <div class="content-packs-toolbar">
                    <div class="content-packs-add">
                        <button class="secondary-action content-packs-add-zip">${escapeHtml(t('mods.addZip'))}</button>
                        <button class="secondary-action content-packs-add-folder">${escapeHtml(t('mods.addFolder'))}</button>
                    </div>
                    <div class="content-packs-tools">
                        <button class="secondary-action content-packs-open"><span class="secondary-action-icon folder-icon"></span>${escapeHtml(t('mods.openFolder'))}</button>
                        <button class="secondary-action content-packs-refresh" title="${escapeHtml(t('mods.refreshHint'))}">${escapeHtml(t('mods.refresh'))}</button>
                    </div>
                </div>
                <p class="content-packs-hint">${escapeHtml(t('mods.orderHint'))}</p>
                <div class="content-packs-status" hidden></div>
                <div class="content-packs-body"><div class="content-packs-empty">${escapeHtml(t('mods.loading'))}</div></div>
            </div>`;

        host.querySelector('.content-packs-add-zip').addEventListener('click', () => addMod(gameId, 'zip'));
        host.querySelector('.content-packs-add-folder').addEventListener('click', () => addMod(gameId, 'folder'));
        host.querySelector('.content-packs-open').addEventListener('click', () => openFolder(gameId));
        host.querySelector('.content-packs-refresh').addEventListener('click', () => load(gameId));
        host.querySelector('.content-packs-body').addEventListener('click', event => onListClick(gameId, event));
        host.querySelector('.content-packs-body').addEventListener('change', event => onListChange(gameId, event));

        if (s.importing) {
            showStatus(gameId, t('mods.installing', { name: s.importing }));
        }
        load(gameId);
    }

    function showStatus(gameId, text, kind) {
        const status = panel(gameId) && panel(gameId).querySelector('.content-packs-status');
        if (!status) return;
        status.hidden = !text;
        status.textContent = text || '';
        status.className = `content-packs-status${kind ? ` is-${kind}` : ''}`;
    }

    async function load(gameId) {
        const s = getState(gameId);
        try {
            s.data = await run('get-mods', { game: backendId(gameId) });
        } catch (error) {
            reportError(error);
            return;
        }
        renderList(gameId);
    }

    function renderList(gameId) {
        const host = panel(gameId);
        if (!host) return;
        const body = host.querySelector('.content-packs-body');
        const data = getState(gameId).data;

        if (!data || !data.installed) {
            body.innerHTML = `<div class="content-packs-empty">${escapeHtml(t('mods.setUpFirst'))}</div>`;
            host.querySelectorAll('.content-packs-add button, .content-packs-open').forEach(b => { b.disabled = true; });
            return;
        }
        host.querySelectorAll('.content-packs-add button, .content-packs-open').forEach(b => { b.disabled = false; });

        if (data.running) {
            showStatus(gameId, t('mods.appliesNextLaunch'), 'info');
        }

        const mods = data.mods || [];
        if (!mods.length) {
            body.innerHTML = `<div class="content-packs-empty">${escapeHtml(t('mods.empty'))}</div>`;
            return;
        }
        body.innerHTML = `<ol class="content-packs-list">${mods.map((mod, i) => rowHTML(mod, i, mods.length)).join('')}</ol>`;
    }

    async function onListChange(gameId, event) {
        const checkbox = event.target.closest('.content-pack-enabled');
        if (!checkbox) return;
        const name = checkbox.closest('.content-pack').dataset.name;
        try {
            const result = await run('set-mod-enabled', { game: backendId(gameId), name, enabled: checkbox.checked });
            if (!result || !result.success) throw new Error((result && result.error) || t('mods.saveFailed'));
        } catch (error) {
            reportError(error);
        }
        load(gameId);
    }

    async function onListClick(gameId, event) {
        const row = event.target.closest('.content-pack');
        if (!row) return;
        const name = row.dataset.name;

        if (event.target.closest('.content-pack-up')) return move(gameId, name, -1);
        if (event.target.closest('.content-pack-down')) return move(gameId, name, 1);
        if (event.target.closest('.content-pack-open')) {
            const data = getState(gameId).data;
            return run('open-folder', { path: `${data.folder}\\${name}` }).catch(reportError);
        }
        if (event.target.closest('.content-pack-remove')) return remove(gameId, name);
    }

    async function move(gameId, name, delta) {
        const data = getState(gameId).data;
        const names = (data.mods || []).map(mod => mod.name);
        const from = names.indexOf(name);
        const to = from + delta;
        if (from < 0 || to < 0 || to >= names.length) return;
        names.splice(to, 0, names.splice(from, 1)[0]);
        try {
            const result = await run('set-mod-order', { game: backendId(gameId), names });
            if (!result || !result.success) throw new Error((result && result.error) || t('mods.saveFailed'));
        } catch (error) {
            reportError(error);
        }
        load(gameId);
    }

    async function remove(gameId, name) {
        const choice = await window.showMessageBox(t('mods.removeTitle'), t('mods.removeBody', { name }), [t('common.cancel'), t('mods.remove')]);
        if (choice !== 1) return;
        try {
            const result = await run('uninstall-mod', { game: backendId(gameId), name });
            if (!result || !result.success) throw new Error((result && result.error) || t('mods.removeFailed'));
            window.showToast(t('mods.removed', { name }), 'success');
        } catch (error) {
            reportError(error);
        }
        load(gameId);
    }

    async function openFolder(gameId) {
        const data = getState(gameId).data;
        if (!data || !data.folder) return;
        try {
            const prepared = await run('prepare-mods-folder', { game: backendId(gameId) });
            if (!prepared?.success) throw new Error(prepared?.error || t('mods.saveFailed'));
            await run('open-folder', { path: data.folder });
        } catch (error) {
            reportError(error);
        }
    }

    async function addMod(gameId, kind) {
        const s = getState(gameId);
        if (s.importing) return;

        let source = null;
        try {
            source = kind === 'zip'
                ? await run('browse-file', { title: t('mods.pickZip'), filters: [{ name: t('mods.zipFilter'), pattern: '*.zip' }] })
                : await run('browse-folder', {});
        } catch (error) {
            reportError(error);
            return;
        }
        if (!source) return;

        const label = source.split(/[\\/]/).pop();
        try {
            const started = await run('import-mod', { game: backendId(gameId), path: source, kind });
            if (!started || !started.success) throw new Error((started && started.error) || t('mods.installFailed'));
        } catch (error) {
            reportError(error);
            return;
        }

        s.importing = label;
        showStatus(gameId, t('mods.installing', { name: label }));
        pollImport(gameId);
    }

    async function pollImport(gameId) {
        const s = getState(gameId);
        let job = null;
        try {
            job = await run('get-mod-import', { game: backendId(gameId) });
        } catch (error) {
            job = { active: false, finished: true, error: String(error.message || error) };
        }

        if (job && job.active) {
            setTimeout(() => pollImport(gameId), 300);
            return;
        }

        s.importing = null;
        if (job && job.error) {
            showStatus(gameId, job.error, 'error');
        } else {
            showStatus(gameId, '');
            window.showToast(t('mods.installed', { name: (job && job.name) || '' }), 'success');
        }
        load(gameId);
    }

    // Refresh when the game's page is opened, so mods added outside the launcher show up.
    document.addEventListener('click', event => {
        const item = event.target.closest && event.target.closest('.game-item[data-game]');
        if (item && SUPPORTED.has(backendId(item.dataset.game)) && panel(item.dataset.game)) {
            setTimeout(() => load(item.dataset.game), 0);
        }
    }, true);

    window.ModsView = {
        supports: gameId => SUPPORTED.has(backendId(gameId)),
        render
    };
})();
