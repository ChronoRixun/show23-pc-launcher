// Lists and manages local content packs in data/mods.
(function () {
    'use strict';

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
            state[gameId] = { data: null, request: 0, importing: null };
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

    function icon(name) {
        return '<svg class="icon" aria-hidden="true" viewBox="0 0 24 24"><use href="#icon-' + name + '"></use></svg>';
    }

    function rowHTML(mod, index, count) {
        const title = mod.title || mod.name;
        const focus = action => 'data-focus="' + escapeHtml('mod-' + mod.name + '-' + action) + '"';
        const details = [
            mod.version && t('mods.version', { version: mod.version }),
            mod.author && t('mods.by', { author: mod.author }),
            t(mod.files === 1 ? 'mods.fileCountOne' : 'mods.fileCount', { count: mod.files || 0, size: formatSize(mod.size) }),
            !mod.enabled && t('mods.off')
        ].filter(Boolean).map(escapeHtml).join(' <i class="sep" aria-hidden="true"></i> ');
        const iconButton = (action, symbol, labelKey, disabled = false) => {
            const label = escapeHtml(t(labelKey));
            return `<button class="icon-btn content-pack-${action}" type="button" ${focus(action)} aria-label="${label}" title="${label}"${disabled ? ' disabled' : ''}>${icon(symbol)}</button>`;
        };
        return `
            <div class="mod content-pack${mod.enabled ? '' : ' off'}" role="row" data-name="${escapeHtml(mod.name)}">
                <div class="mod-order" role="cell"><span>${index + 1}</span></div>
                <div role="cell" class="mod-info">
                    <button type="button" class="mod-name content-pack-open" ${focus('open')} title="${escapeHtml(mod.description || t('mods.openModFolder'))}" aria-label="${escapeHtml(t('mods.openNamed', { name: title }))}">${escapeHtml(title)}</button>
                    <div class="mod-meta">${details}</div>
                </div>
                <div role="cell"><input type="checkbox" class="switch content-pack-enabled" role="switch" aria-label="${escapeHtml(t('mods.enableNamed', { name: title }))}" ${focus('enabled')}${mod.enabled ? ' checked' : ''}></div>
                <div class="mod-move" role="cell">
                    ${iconButton('up', 'up', 'mods.moveUp', index === 0)}
                    ${iconButton('down', 'down', 'mods.moveDown', index === count - 1)}
                </div>
                <div role="cell">${iconButton('remove', 'trash', 'mods.remove')}</div>
            </div>`;
    }

    function entryButtons() {
        return `
            <button class="btn btn-primary content-packs-add-zip" type="button">${icon('plus')}${escapeHtml(t('mods.addZip'))}</button>
            <button class="btn btn-secondary content-packs-add-folder" type="button">${escapeHtml(t('mods.addFolder'))}</button>`;
    }

    function openButton(withIcon) {
        return `<button class="btn btn-ghost content-packs-open" type="button">${withIcon ? icon('folder') : ''}${escapeHtml(t('mods.openFolder'))}</button>`;
    }

    function render(gameId) {
        const host = panel(gameId);
        if (!host) return;
        const s = getState(gameId);
        if (!host.querySelector('.content-packs-body')) {
            host.innerHTML = '<div class="status-line content-packs-status" role="status" hidden></div>' +
                '<div class="content-packs-body">' + escapeHtml(t('mods.loading')) + '</div>';
            const body = host.querySelector('.content-packs-body');
            body.addEventListener('click', event => {
                const target = event.target;
                if (target.closest('.content-packs-add-zip')) return addMod(gameId, 'zip');
                if (target.closest('.content-packs-add-folder')) return addMod(gameId, 'folder');
                if (target.closest('.content-packs-open')) return openFolder(gameId);
                if (target.closest('.content-packs-refresh')) return load(gameId);
                return onListClick(gameId, event);
            });
            body.addEventListener('change', event => onListChange(gameId, event));
        }
        if (s.importing) showStatus(gameId, t('mods.installing', { name: s.importing }));
        return load(gameId);
    }

    function showStatus(gameId, text, kind) {
        const status = panel(gameId)?.querySelector('.content-packs-status');
        if (!status) return;
        status.hidden = !text;
        status.textContent = text || '';
        status.className = 'status-line content-packs-status' + (kind === 'error' ? ' error-text' : '');
    }

    async function load(gameId) {
        const s = getState(gameId);
        const request = ++s.request;
        try {
            const data = await run('get-mods', { game: backendId(gameId) });
            if (request !== s.request) return;
            s.data = data;
        } catch (error) {
            reportError(error);
            return;
        }
        renderList(gameId);
    }

    function renderList(gameId) {
        const host = panel(gameId);
        const body = host?.querySelector('.content-packs-body');
        if (!body) return;
        const data = getState(gameId).data;
        const mods = data?.mods || [];
        const count = document.getElementById('mods-count');
        if (count) {
            count.textContent = String(mods.length);
            count.hidden = !mods.length;
        }

        if (!data?.installed || !mods.length) {
            const art = '<svg class="empty-art" viewBox="0 0 88 88" aria-hidden="true">' +
                '<path d="M44 9 77 42 44 75 11 42Z" fill="none" stroke="currentColor" stroke-width="2" stroke-dasharray="5 5"/>' +
                '<path d="m44 3 10 10-10 10L34 13zm33 29 10 10-10 10-10-10zm-66 0 10 10-10 10L1 42Z" fill="var(--surface-3)" stroke="currentColor" stroke-width="2" stroke-linejoin="round"/>' +
                '<circle cx="44" cy="42" r="12" fill="var(--accent)"/><path d="M44 36v12m-6-6h12" stroke="var(--accent-ink)" stroke-width="3" stroke-linecap="round"/></svg>';
            body.classList.add('is-empty');
            body.innerHTML = `<div class="empty">
                ${art}<h2>${escapeHtml(t('mods.emptyTitle'))}</h2>
                <p>${escapeHtml(t(data?.installed ? 'mods.empty' : 'mods.setUpFirst'))}</p>
                ${data?.installed ? '<div class="actions">' + entryButtons() + openButton(false) + '</div>' : ''}
            </div>`;
            return;
        }

        body.classList.remove('is-empty');
        const refreshLabel = escapeHtml(t('mods.refreshHint'));
        body.innerHTML = `
            <div class="mods-toolbar spread">
                <div class="actions">${entryButtons()}</div>
                <div class="actions">${openButton(true)}
                    <button type="button" class="icon-btn content-packs-refresh" aria-label="${refreshLabel}" title="${refreshLabel}">${icon('refresh')}</button>
                </div>
            </div>
            <div class="mods" role="table" aria-label="${escapeHtml(t('game.mods'))}">
                <div class="mods-head" role="row">
                    <span role="columnheader">#</span><span role="columnheader">${escapeHtml(t('mods.columnMod'))}</span>
                    <span role="columnheader">${escapeHtml(t('mods.columnOn'))}</span>
                    <span role="columnheader">${escapeHtml(t('mods.columnOrder'))}</span><span role="columnheader" aria-label="${escapeHtml(t('mods.remove'))}"></span>
                </div>
                ${mods.map((mod, i) => rowHTML(mod, i, mods.length)).join('')}
            </div>
            <p class="small faint mods-note">${escapeHtml(t('mods.orderNote'))}</p>`;
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
        const choice = await window.showMessageBox(t('mods.removeTitle'), t('mods.removeBody', { name }), [{ label: t('common.cancel'), cancel: true }, { label: t('mods.remove'), danger: true }]);
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

    window.ModsView = {
        render
    };
})();
