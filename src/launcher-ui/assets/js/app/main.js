(() => {
    'use strict';

    const $ = id => document.getElementById(id);
    const run = (command, data) => window.executeCommand(command, data);
    const t = (key, values) => window.LauncherI18n.t(key, values);
    const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
    const partNames = ['launcher', 'emulator', 'server', 'profile'];
    const pages = { show23: 'show23-page', setup: 'setup', mods: 'mods-panel', settings: 'settings-page', support: 'support-page' };
    const state = {
        page: 'show23', settings: {}, game: {}, parts: {}, mods: null,
        sourceKind: 'folder', setupBusy: false, setupDetails: '', location: 'installed',
        checking: false, savingChannel: false, maximized: false, diagnostics: ''
    };

    function make(tag, classes, text) {
        const el = document.createElement(tag);
        if (classes) el.className = classes;
        if (text) el.textContent = text;
        return el;
    }

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

    function showPage(page, focusId) {
        const nav = page === 'setup' || page === 'mods' ? 'show23' : page;
        state.page = page;
        for (const [name, id] of Object.entries(pages)) $(id).hidden = name !== page;
        // Keep sub-view visibility explicit so navigation and tests can track the hero.
        $('home-hero').hidden = page !== 'show23';
        for (const button of document.querySelectorAll('[data-page]')) {
            const active = button.dataset.page === nav;
            button.classList.toggle('active', active);
            if (active) button.setAttribute('aria-current', 'page');
            else button.removeAttribute('aria-current');
        }
        $('content').scrollTop = 0;
        if (focusId) $(focusId).focus();
    }

    function setRadio(id, choice) {
        for (const button of $(id).querySelectorAll('[data-choice]')) {
            const selected = button.dataset.choice === choice;
            button.classList.toggle('on', selected);
            button.setAttribute('aria-checked', String(selected));
            button.setAttribute('tabindex', selected ? '0' : '-1');
        }
    }

    function channelText() {
        return t(state.settings.channel === 'beta' ? 'updateChannel.betaFooter' : 'updateChannel.stableFooter');
    }

    function paintHome() {
        const game = state.game;
        const ready = !!game.ready;
        const setUp = ready || !!game.setUp;
        const missing = !!game.gameMissing;
        $('game-state').textContent = t(ready ? 'game.ready' : setUp ? 'inPlace.gameSetUp' : missing ? 'inPlace.gameMissing' : 'game.notReady');
        $('game-pill').className = setUp ? 'pill ok' : 'pill warn';
        $('game-state-dot').className = setUp ? 'dot ok' : 'dot warn';
        $('home-hero').classList.toggle('is-unset', !setUp);
        const sourceMessage = missing ? t(game.sourcePath ? 'inPlace.missingPath' : 'inPlace.invalidRecord', { path: game.sourcePath }) :
            setUp && !ready ? t('inPlace.runtimeMissing') : '';
        status('game-source-notice', sourceMessage);
        $('play').disabled = !ready;
        $('remove-game-copy').hidden = !game.installedCopy;
        $('play').classList.toggle('btn-primary', ready);
        $('play').classList.toggle('btn-secondary', !ready);
        if (ready) $('play').setAttribute('data-autofocus', '');
        else $('play').removeAttribute('data-autofocus');
        $('setup-open').hidden = ready;
        const setupLabel = setUp || missing ? 'inPlace.setUpAgain' : 'game.setup';
        $('setup-open-label').setAttribute('data-i18n', setupLabel);
        $('setup-open-label').textContent = t(setupLabel);
        $('setup-open').classList.toggle('btn-primary', !setUp);
        $('setup-open').classList.toggle('btn-secondary', setUp);
        $('setup-open').classList.toggle('btn-lg', !setUp);
        $('home-before').hidden = setUp || missing;
        $('home-ready').hidden = !ready;
        $('scorebug').hidden = !ready;

        const meta = $('game-meta');
        meta.textContent = '';
        if (!setUp) {
            if (!missing) meta.textContent = t('game.intro');
            return;
        }
        const fields = [make('span', '', t('game.versionLine', { version: game.version || t('game.unknown') }))];
        if (game.folder) fields.push(make('span', 'mono', game.folder));
        if (state.mods) {
            fields.push(make('span', '', t('game.modsOn', { enabled: state.mods.enabled, total: state.mods.total })));
        }
        fields.forEach((field, index) => {
            if (index) {
                const separator = make('i', 'sep');
                separator.setAttribute('aria-hidden', 'true');
                meta.appendChild(separator);
            }
            meta.appendChild(field);
        });
        $('scorebug-game').textContent = game.version || t('game.unknown');
        $('scorebug-source').textContent = game.updated ? t('game.baseAndUpdate') : t('game.baseOnly');
    }

    async function gameState() {
        state.game = await run('get-show23') || {};
        state.mods = null;
        paintHome();
        if (state.game.ready) {
            const folder = state.game.folder;
            const result = await run('get-mods', { game: 'show23' });
            if (state.game.folder === folder && result?.installed) {
                const mods = result.mods || [];
                state.mods = { enabled: mods.filter(mod => mod.enabled).length, total: mods.length };
                paintHome();
            }
        }
    }

    function setupInputsChanged() {
        const base = $('base-path').value;
        $('setup-continue').disabled = state.setupBusy || !base || !$('install-path').value;
        $('update-clear').disabled = !$('update-path').value;
        const packageMode = state.sourceKind === 'package';
        $('setup-unpacker').hidden = !packageMode;
        paintUnpacker();
        $('base-choose').textContent = t(packageMode ? 'setup.package' : 'setup.folder');
        status('base-hint', base ? t(packageMode ? 'setup.packageSelected' : 'setup.folderSelected',
            { name: base.split(/[\\/]/).filter(Boolean).pop() }) : '');
    }

    function paintUnpacker() {
        for (const view of ['setup', 'settings']) {
            $(view + '-unpacker-path').value = state.settings.unpacker || state.settings.bundledUnpacker || '';
            $(view + '-unpacker-clear').disabled = !state.settings.unpacker;
        }
    }

    function showPicker(focusId) {
        $('setup-picker').hidden = false;
        $('setup-progress').hidden = true;
        $('setup-title').textContent = t('setup.title');
        $('setup-lede').textContent = t('setup.intro');
        setupInputsChanged();
        if (focusId) $(focusId).focus();
    }

    function openSetup() {
        if (state.setupBusy) return;
        state.sourceKind = 'folder';
        $('base-path').value = '';
        $('update-path').value = '';
        $('install-path').value = state.game.folder || '';
        setRadio('base-kind', state.sourceKind);
        showPage('setup');
        showPicker('base-folder');
    }

    async function pickPackage(title) {
        return run('browse-file', { title: t(title), filters: [{ name: t('setup.packageFilter'), pattern: '*.pkg' }] });
    }

    function spaceText(bytes) {
        if (bytes < 1024 ** 2) return new Intl.NumberFormat(window.LauncherI18n.getLanguage()).format(bytes) + ' B';
        const gib = bytes / (1024 ** 3);
        const format = new Intl.NumberFormat(window.LauncherI18n.getLanguage(), { maximumFractionDigits: 2 });
        return gib >= 1 ? format.format(gib) + ' GiB' : format.format(gib * 1024) + ' MiB';
    }

    function findingText(row) {
        const [scope, field] = row.id.split('.');
        const values = row.values || {};
        const part = t(values.part === 'update' ? 'checks.updateSource' : 'checks.baseSource');
        if (scope === 'unpack') {
            if (field === 'inPlace') return t('inPlace.usingFolder');
            return t('package.' + (field === 'program' ? 'runningProgram' : field), values);
        }
        if (scope === 'settings' || scope === 'finish') return t('inPlace.' +
            (scope === 'settings' && field !== 'written' ? (field === 'tree' ? 'settingsTree' : 'settingsRecord') : field));
        if (scope === 'verify') {
            const suffix = field === 'directory' ? { passed: 'directoryMatch', failed: 'directoryShort', notice: 'directoryExtra' }[row.state]
                : field === 'executable' ? (row.state === 'passed' ? 'executableMatch' : 'executableChanged')
                : field === 'record' ? 'recordInvalid' : field;
            return t('inPlace.' + suffix, {
                part, name: values.name,
                difference: spaceText(values.difference || 0), actual: spaceText(values.actual || 0), expected: spaceText(values.expected || 0)
            });
        }
        const ok = row.state === 'passed';
        const reason = values.reason;
        const source = t(scope === 'update' ? 'checks.updateSource' : 'checks.baseSource');
        const details = { ...values, source };
        if (scope === 'check') return t('checks.' + field);
        if (scope === 'install') {
            switch (field) {
                case 'local': return t(ok ? 'checks.local' : 'checks.notLocal');
                case 'overlap': return t(ok ? 'checks.separate' : reason === 'settings' ? 'checks.settingsOverlap' : 'checks.overlap');
                case 'writable':
                    if (row.state === 'later') return t('checks.writableLater');
                    return t(ok ? 'checks.writable' : reason === 'link' ? 'checks.destinationLink'
                        : reason === 'directory' ? 'checks.destinationDirectory' : 'checks.unwritable');
                case 'space':
                    if (reason === 'size') return t('checks.estimateFailed');
                    if (reason === 'unreadable') return t('checks.spaceUnknown');
                    return t(values.kind === 'in_place' ? (ok ? 'inPlace.spaceOk' : 'inPlace.spaceLow') :
                        (ok ? 'checks.spaceOk' : 'checks.spaceLow'), {
                        needed: spaceText(values.needed), available: spaceText(values.available), drive: values.drive
                    });
            }
        }
        switch (field) {
            case 'kind':
                if (ok) return t(values.kind === 'folder' ? 'checks.folder' : 'checks.package', details);
                return t({
                    missing: 'checks.missingSource', link: 'checks.sourceLink',
                    expectedFolder: 'checks.expectedFolder', expectedFile: 'checks.expectedPackage'
                }[reason] || 'checks.unreadableSource', details);
            case 'complete':
                if (ok) return t(values.kind === 'folder' ? 'checks.folderMeasured' : 'checks.fileComplete', details);
                return t({
                    truncated: 'checks.truncated', magic: 'checks.badMagic', folder: 'checks.badFolder',
                    unreadable: 'checks.unreadableSource'
                }[reason] || 'checks.badLayout', details);
            case 'metadata':
                if (ok) return t('checks.metadata', details);
                return t({
                    missing: 'checks.metadataMissing', encrypted: 'checks.metadataEncrypted', link: 'checks.metadataLink'
                }[reason] || 'checks.metadataInvalid', details);
            case 'type': return t(ok ? 'checks.packageType' : 'checks.wrongPackageType', details);
            case 'content': return t(ok ? 'checks.content' : 'checks.wrongContent', details);
            case 'title': return t(ok ? 'checks.title' : 'checks.wrongTitle', details);
            case 'category': return t(ok ? (scope === 'base' ? 'checks.baseCategory' : 'checks.updateCategory')
                : (scope === 'base' ? 'checks.wrongBaseCategory' : 'checks.wrongUpdateCategory'));
            case 'version': return t(ok ? (scope === 'base' ? 'checks.baseVersion' : 'checks.updateVersion') : 'checks.wrongVersion', details);
            case 'executable':
                if (row.state === 'later') return t('checks.executableLater', details);
                if (ok) return t('checks.executable', details);
                return t({
                    digest: 'checks.unknownExecutable', missing: 'checks.missingExecutable',
                    link: 'checks.executableLink'
                }[reason] || 'checks.unreadableExecutable', details);
            case 'matches': return t(ok ? 'checks.matches' : 'checks.different');
            case 'offline': return t('checks.offlineUpdate');
            default: return t('checks.internal');
        }
    }

    function showChecklist(rows, listId, sectionId) {
        const list = $(listId);
        list.textContent = '';
        $(sectionId).hidden = !rows.length;
        const symbols = { passed: 'check', failed: 'alert', notice: 'info', later: 'right' };
        return rows.map(row => {
            const text = findingText(row);
            const state = Object.hasOwn(symbols, row.state) ? row.state : 'failed';
            const description = t('checks.' + state);
            const item = make('li', 'check-finding ' + state);
            item.innerHTML = '<svg class="icon" aria-hidden="true" viewBox="0 0 24 24"><use href="#icon-' + symbols[state] + '"></use></svg>';
            item.appendChild(make('span', 'check-state', description));
            item.appendChild(make('span', 'check-text', text));
            list.appendChild(item);
            return description + ' — ' + text;
        });
    }

    function showStage(reply) {
        const busy = reply?.busy ?? !reply;
        const stages = reply?.stages || { check: { state: busy ? 'running' : 'failed', findings: busy ? [] : [{ id: 'check.internal', state: 'failed' }] } };
        const finished = stages.finish?.state === 'done';
        const unpackStop = stages.unpack?.state === 'stopped';
        const states = { waiting: 'todo', running: 'current', done: 'done', not_needed: 'not-needed', stopped: 'stopped', failed: 'failed' };
        $('setup-picker').hidden = true;
        $('setup-progress').hidden = false;
        $('setup-title').textContent = t(busy ? 'setup.settingUp' : finished ? 'inPlace.setupComplete' : 'setup.stopped');
        $('setup-lede').textContent = '';
        for (const node of $('setup-stages').querySelectorAll('[data-stage]')) {
            const stage = node.dataset.stage;
            const outcome = stages[stage];
            const kind = states[outcome?.state || 'waiting'];
            node.className = 'stage ' + kind;
            node.querySelector('.glyph').setAttribute('d', kind === 'done' ? 'm10 16 4 4 8-8' : kind === 'not-needed' ? 'M10 16h12' : 'M16 10v7m0 4v.1');
            const detail = outcome?.findings?.find(row => row.state === 'passed');
            const label = t({
                done: 'setup.done', failed: 'setup.failed', current: stage === 'check' ? 'setup.checkingInputs' : 'inPlace.working',
                stopped: 'setup.stopped', 'not-needed': 'inPlace.notNeeded', todo: busy ? 'setup.waiting' : 'setup.notStarted'
            }[kind]);
            node.querySelector('.stage-sub').textContent = detail && (stage === 'unpack' || stage === 'settings')
                ? label + ' · ' + findingText(detail) : label;
        }
        $('setup-pending').hidden = !busy;
        $('cancel-unpack').hidden = !reply?.canCancel;
        const program = stages.unpack?.findings?.find(row => row.id === 'unpack.program');
        status('running-unpacker', busy && program ? findingText(program) : '');
        const extraction = reply?.unpackProgress;
        status('unpack-progress', busy && extraction ? t('package.progress', {
            elapsed: extraction.elapsed, written: spaceText(extraction.written), expected: spaceText(extraction.expected)
        }) : '');
        $('setup-stop-note').hidden = state.sourceKind === 'package';
        $('setup-alert').hidden = busy || finished;
        $('setup-alert').classList.toggle('is-stopped', unpackStop);
        $('setup-stop-icon').setAttribute('href', unpackStop ? '#icon-info' : '#icon-alert');
        $('setup-failure-actions').hidden = busy;
        $('setup-retry').hidden = unpackStop || finished;
        $('setup-edit').hidden = finished;
        $('setup-done').hidden = !finished;

        const firstFailure = Object.values(stages).flatMap(stage => stage.findings || []).find(row => row.state === 'failed');
        const stop = stages.unpack?.findings?.find(row => row.state === 'later');
        const message = (unpackStop && stop) ? findingText(stop) : firstFailure ? findingText(firstFailure) : '';
        $('setup-status').textContent = message;
        const input = showChecklist(stages.check?.findings || [], 'setup-findings', 'setup-checklist');
        const verification = showChecklist(stages.verify?.findings || [], 'setup-verify-findings', 'setup-verification');
        const other = ['unpack', 'settings', 'finish'].flatMap(stage => (stages[stage]?.findings || []).map(findingText));
        state.setupDetails = [...new Set([message, ...input, ...verification, ...other].filter(Boolean))].join('\n');
        if (!busy && state.page === 'setup') $(finished ? 'setup-done' : unpackStop ? 'setup-edit' : 'setup-retry').focus();
    }

    async function submitSetup() {
        if (state.setupBusy || !$('base-path').value || !$('install-path').value) return;
        state.setupBusy = true;
        try {
            showStage(null);
            const payload = {
                kind: state.sourceKind, base: $('base-path').value,
                update: $('update-path').value, install: $('install-path').value,
                displayedUnpacker: $('setup-unpacker-path').value
            };
            for (;;) {
                $('cancel-unpack').disabled = false;
                let reply = await run('setup-show23', { ...payload });
                while (reply?.busy) {
                    reply = await run('get-setup-inputs');
                    if (reply?.busy) { showStage(reply); await sleep(160); }
                }
                if (reply?.canceled) {
                    showPicker('base-choose');
                    break;
                }
                showStage(reply || {});
                const decision = reply?.stages?.unpack?.findings?.find(row =>
                    ['unpack.consent', 'unpack.existing', 'unpack.preview'].includes(row.id));
                if (!decision || reply.stages.unpack.state !== 'stopped') break;
                const cancel = { label: t('common.cancel'), cancel: true };
                if (decision.id === 'unpack.existing') {
                    const choice = await window.showMessageBox(t('package.existingTitle'), findingText(decision),
                        [cancel, t('package.keep'), t('package.replace')]);
                    if (choice !== 1 && choice !== 2) break;
                    payload.existingCopy = choice === 1 ? 'keep' : 'replace';
                } else if (decision.id === 'unpack.consent') {
                    const choice = await window.showMessageBox(t('package.consentTitle'), findingText(decision),
                        [cancel, t('package.run')]);
                    if (choice !== 1) break;
                    payload.unpackerConsent = decision.values.consent;
                    payload.displayedUnpacker = decision.values.path;
                } else {
                    payload.displayedUnpacker = decision.values.path;
                    $('setup-unpacker-path').value = payload.displayedUnpacker;
                    if (await window.showMessageBox(t('package.program'), findingText(decision),
                        [cancel, t('setup.continue')]) !== 1) break;
                }
            }
            await gameState();
        } catch (error) {
            console.error(error);
            showStage({});
        } finally {
            state.setupBusy = false;
            setupInputsChanged();
        }
    }

    async function verify() {
        $('verify').disabled = true;
        showChecklist([], 'game-verify-findings', 'game-verification');
        status('game-status', t('game.verifying'));
        try {
            await run('verify-show23');
            for (let attempt = 0; attempt < 1800; attempt++) {
                const result = await run('get-verify') || {};
                if (!result.busy) {
                    status('game-status', result.message || t('support.error'));
                    showChecklist(result.findings || [], 'game-verify-findings', 'game-verification');
                    await gameState();
                    break;
                }
                await sleep(500);
            }
        } finally {
            $('verify').disabled = false;
        }
    }

    function paintSettings() {
        const saved = state.settings;
        paintUnpacker();
        window.IS_OFFLINE = !!saved.offline;
        window.LauncherI18n.setLanguage(saved.language || 'en');
        window.LauncherI18n.applyStaticTranslations();
        $('language').value = window.LauncherI18n.getLanguage();
        setRadio('channel', saved.channel || 'stable');
        setRadio('location', state.location);
        $('switch-mode').disabled = (state.location === 'portable') === !!saved.portable;
        $('offline').checked = !!saved.offlineSaved;
        $('offline-status').hidden = !saved.offline;
        $('check-parts').disabled = !!saved.offline || state.checking;
        window.__setMaximized(state.maximized);
        paintHome();
        window.LauncherUpdate.render();
    }

    async function save(key, value) {
        const ok = await run('save-setting', { key, value });
        status('settings-status', ok ? '' : t('settings.saveError'));
        state.settings = await run('get-settings') || {};
        paintSettings();
        return ok;
    }

    function getPartStatus(item) {
        const launcher = window.LauncherUpdate.status;
        if (window.IS_OFFLINE) return { text: t('launcherUpdate.offline'), tone: '' };
        const own = item.part === 'launcher';
        const info = own ? launcher : item;
        const noRelease = /^(No release has been published|No release with all its files has been published)/i;
        if (info?.state === 'unpublished' || noRelease.test(info?.error || info?.status || '')) {
            return { tone: '', text: t('settings.noRelease') };
        }
        switch (info?.state) {
            case 'available':
                return { tone: 'accent', text: t('settings.available', { version: own ? launcher.latest : item.availableVersion }) };
            case 'up-to-date':
                if (!info.fromCache && !info.aheadOfStable && item.version) return { tone: 'ok', text: t('settings.upToDate') };
                break;
            case 'checking':
                return { tone: 'accent', text: t('settings.checking') };
        }
        const text = own ? window.LauncherUpdate.statusLine(launcher) : item.status;
        return { text: text || t('settings.notChecked'), tone: '' };
    }

    function paintPartStatus(target, item) {
        const spec = getPartStatus(item);
        target.textContent = '';
        const dot = make('i', 'dot ' + spec.tone);
        dot.setAttribute('aria-hidden', 'true');
        target.appendChild(dot);
        target.appendChild(make('span', 'status-text', spec.text));
    }

    function currentPart(name) {
        return (state.parts.parts || []).find(part => part.part === name) || { part: name };
    }

    function paintVersions() {
        $('parts').textContent = '';
        for (const name of partNames) {
            const item = currentPart(name);
            const title = t('settings.' + name);
            const version = item.version || t('settings.noVersion');
            const row = make('tr');
            row.dataset.part = name;
            row.appendChild(make('td', 'strong', title));
            const versionCell = make('td', 'v', version);
            if (item.prerelease) versionCell.appendChild(make('span', 'tag', t('updateChannel.badge')));
            row.appendChild(versionCell);
            const statusCell = make('td');
            const line = make('span', 'part-status');
            line.id = 'part-status-' + name;
            statusCell.appendChild(line);
            row.appendChild(statusCell);
            $('parts').appendChild(row);

            const scoreCell = $('scorebug').querySelector('[data-part="' + name + '"]');
            scoreCell.textContent = '';
            scoreCell.appendChild(make('div', 'sb-label', title));
            const value = make('div', 'sb-val' + (item.version ? '' : ' muted'));
            value.appendChild(make('span', 'part-version', version));
            if (item.prerelease) value.appendChild(make('span', 'tag', t('updateChannel.badge')));
            scoreCell.appendChild(value);
            scoreCell.appendChild(make('div', 'sb-sub part-status'));
        }
        paintUpdateSummary();
    }

    function paintUpdateSummary() {
        const channel = channelText();
        $('footer-channel').textContent = channel;
        $('footer-channel-dot').className = state.settings.channel === 'beta' ? 'dot warn' : 'dot ok';
        $('footer-version').textContent = t('nav.launcherVersion', { version: currentPart('launcher').version || t('game.unknown') });
        for (const name of partNames) {
            const item = currentPart(name);
            const line = $('part-status-' + name);
            const cell = $('scorebug').querySelector('[data-part="' + name + '"]').querySelector('.part-status');
            if (line) paintPartStatus(line, item);
            if (cell) paintPartStatus(cell, item);
        }
        const stamp = Math.max(state.parts.checkedAt || 0, window.LauncherUpdate.status?.checkedAt || 0);
        const checked = stamp ? t('settings.checked', {
            time: new Intl.DateTimeFormat(window.LauncherI18n.getLanguage(), { hour: '2-digit', minute: '2-digit' }).format(stamp)
        }) : '';
        status('parts-checked', checked);
        $('scorebug-checked').textContent = checked ? channel + ' · ' + checked : channel;
    }

    async function refreshParts() {
        state.parts = await run('get-parts') || {};
        paintVersions();
        return state.parts.busy;
    }

    async function followUpdates() {
        if (state.checking) return;
        state.checking = true;
        $('check-parts').disabled = true;
        const refresh = window.LauncherUpdate.follow();
        try {
            for (let attempt = 0; attempt < 95; attempt++) {
                if (!await refreshParts()) break;
                await sleep(500);
            }
            await refresh;
        } finally {
            state.checking = false;
            $('check-parts').disabled = !!state.settings.offline;
            status('update-status', '');
            await refreshParts();
        }
    }

    async function stable() {
        const saved = window.LauncherUpdate.status;
        if (!saved?.aheadOfStable || !saved.canInstall || saved.channel !== 'stable') return;
        const choice = await window.showMessageBox(
            t('settings.goStable'), t('settings.confirmDowngrade', { current: saved.current, target: saved.latest }),
            [{ label: t('settings.keepBeta'), cancel: true }, t('settings.goStable')]
        );
        if (choice !== 1) {
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
        state.diagnostics = result?.text || '';
        const log = $('diagnostics');
        log.textContent = '';
        state.diagnostics.trimEnd().split('\n').forEach((line, index) => {
            const divider = line.indexOf(': ');
            if (index === 0) log.appendChild(make('span', 'log-title', line));
            else if (divider === -1) log.appendChild(make('span', 'log-row', line));
            else {
                const row = make('span', 'log-row');
                row.appendChild(make('span', 'k', line.slice(0, divider + 1)));
                row.appendChild(make('span', '', ' ' + line.slice(divider + 2)));
                log.appendChild(row);
            }
        });
    }

    async function openSettings() {
        showPage('settings');
        await window.LauncherUpdate.refresh();
        await refreshParts();
    }

    async function returnHome(button) {
        showPage('show23', button);
        await gameState();
    }

    async function copy(text, okKey) {
        const answer = await run('copy-to-clipboard', { text });
        window.showToast(t(answer?.success ? okKey : 'support.copyFailed'), answer?.success ? 'success' : 'error');
    }

    function wireSetup() {
        for (const view of ['setup', 'settings']) {
            bind(view + '-unpacker-pick', 'click', async () => {
                const path = await run('browse-file', { title: t('package.program'), filters: [
                    { name: t('package.program'), pattern: '*.exe' }
                ] });
                if (path) await save('unpacker', path);
            });
            bind(view + '-unpacker-clear', 'click', () => save('unpacker', ''));
        }
        bind('cancel-unpack', 'click', async () => {
            $('cancel-unpack').disabled = true;
            if (!await run('cancel-setup')) $('cancel-unpack').disabled = false;
        });
        bind('remove-game-copy', 'click', async () => {
            if (!state.game.installedCopy) return;
            const choice = await window.showMessageBox(t('package.remove'), t('package.removeBody'),
                [{ label: t('common.cancel'), cancel: true }, { label: t('package.remove'), danger: true }]);
            if (choice !== 1) return;
            const removed = await run('remove-game-copy');
            await gameState();
            status('game-status', t(removed ? 'package.removed' : 'package.removeFailed'));
        });
        bind('setup-open', 'click', openSetup);
        bind('setup-back', 'click', () => returnHome('setup-open'));
        bind('setup-cancel', 'click', () => returnHome('setup-open'));
        bind('setup-edit', 'click', () => showPicker('base-choose'));
        bind('setup-done', 'click', () => returnHome('setup-open'));
        bind('setup-retry', 'click', submitSetup);
        bind('setup-continue', 'click', submitSetup);
        bind('copy-setup', 'click', () => copy(state.setupDetails, 'setup.detailsCopied'));

        for (const kind of ['package', 'folder']) {
            bind('base-' + kind, 'click', () => {
                if (state.sourceKind !== kind) $('base-path').value = '';
                state.sourceKind = kind;
                setRadio('base-kind', kind);
                setupInputsChanged();
            });
        }
        bind('base-choose', 'click', async () => {
            const path = state.sourceKind === 'package' ? await pickPackage('setup.package') : await run('browse-folder');
            if (path) $('base-path').value = path;
            setupInputsChanged();
        });
        bind('update-package', 'click', async () => {
            const path = await pickPackage('setup.pickUpdate');
            if (path) $('update-path').value = path;
            setupInputsChanged();
        });
        bind('update-folder', 'click', async () => {
            const path = await run('browse-folder');
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
    }

    function wireSettings() {
        for (const channel of ['stable', 'beta']) {
            bind('channel-' + channel, 'click', async () => {
                if (state.savingChannel || channel === state.settings.channel) return;
                state.savingChannel = true;
                try {
                    if (await save('channel', channel)) {
                        // The backend invalidates the old selection and begins the new launcher check.
                        await followUpdates();
                    }
                } finally {
                    state.savingChannel = false;
                }
            });
        }
        for (const location of ['installed', 'portable']) {
            bind('location-' + location, 'click', () => {
                state.location = location;
                setRadio('location', location);
                $('switch-mode').disabled = (location === 'portable') === !!state.settings.portable;
            });
        }
        bind('language', 'change', async () => {
            await save('language', $('language').value);
            paintVersions();
        });
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
                    [{ label: t('common.cancel'), cancel: true }, t('settings.restart')]);
                if (choice === 1) await run('relaunch');
            }
        });
        bind('restart-online', 'click', () => run('relaunch-online'));
        bind('switch-mode', 'click', async () => {
            const answer = await run('switch-portable', { portable: state.location === 'portable' });
            if (!answer?.success) status('settings-status', t('settings.saveError'));
        });
    }

    function wireControls() {
        bind('show23', 'click', () => returnHome());
        bind('settings', 'click', openSettings);
        bind('manage-updates', 'click', async () => {
            await openSettings();
            $('channel-' + (state.settings.channel || 'stable')).focus();
        });
        bind('support', 'click', async () => {
            showPage('support');
            await diagnostics();
        });
        bind('minimize-button', 'click', () => run('minimize'));
        bind('maximize-button', 'click', () => run('toggle-maximize'));
        bind('close-button', 'click', () => run('close'));
        window.__setMaximized = value => {
            state.maximized = !!value;
            const button = $('maximize-button');
            button.classList.toggle('is-maximized', state.maximized);
            button.setAttribute('aria-label', t(state.maximized ? 'window.restore' : 'window.maximize'));
        };

        bind('play', 'click', async () => {
            const result = await run('play-show23');
            status('game-status', result?.message);
        });
        bind('verify', 'click', verify);
        bind('mods', 'click', () => {
            showPage('mods', 'mods-back');
            window.ModsView.render('show23');
        });
        bind('mods-back', 'click', () => returnHome('mods'));
        bind('refresh-diagnostics', 'click', diagnostics);
        bind('open-logs', 'click', () => run('open-logs'));
        bind('copy-diagnostics', 'click', () => copy(state.diagnostics, 'support.copied'));
        wireSetup();
        wireSettings();
    }

    async function init() {
        wireControls();
        window.paintLauncherSummary = paintUpdateSummary;
        try {
            state.settings = await run('get-settings') || {};
            state.location = state.settings.portable ? 'portable' : 'installed';
            paintSettings();
            await Promise.all([gameState(), refreshParts(), window.LauncherUpdate.init()]);
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
