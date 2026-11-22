// The launcher's own update: a bar at the top of every page when a newer show23-pc launcher is
// released, and a line under the version in Settings (with a Beta badge on a pre-release and the
// update channel). The backend (updater/launcher_update.cpp) checks the
// channel's release (Stable: GitHub's latest; Beta: pre-releases too), downloads and verifies the
// portable zip when the player says Update, and installs it on the next start (Restart now does that
// at once). The launcher checks on startup for release builds.
(function () {
    'use strict';

    function t(key, variables) {
        return window.LauncherI18n ? window.LauncherI18n.t(key, variables) : key;
    }

    function run(command, payload) {
        return window.executeCommand(command, payload);
    }

    function el(id) {
        return document.getElementById(id);
    }

    const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

    function formatBytes(bytes) {
        return `${Math.round(bytes / (1024 * 1024))} MB`;
    }

    const LauncherUpdate = {
        status: null,
        dismissed: '', // the version whose "available" bar the player closed this session
        polling: null, // the poll under way (follow())

        async init() {
            const action = el('launcher-update-action');
            if (action) action.onclick = () => this.act();
            const notes = el('launcher-update-notes');
            if (notes) notes.onclick = () => this.openPage();
            const dismiss = el('launcher-update-dismiss');
            if (dismiss) dismiss.onclick = () => {
                this.dismissed = this.status ? this.status.latest : '';
                this.render();
            };

            await this.refresh();
            const s = this.status;
            if (s && s.updatedTo && window.showToast) {
                window.showToast(t('launcherUpdate.updated', { version: s.updatedTo }), 'success', 8000);
            }
            if (s && s.enabled && !window.IS_OFFLINE) {
                this.check();
            }
        },

        async refresh() {
            try {
                this.status = await run('get-launcher-update');
            } catch (error) {
                console.error('launcher update: status failed', error);
            }
            this.render();
            return this.status;
        },

        // Polls while the backend checks or downloads; the status it settles on. A second caller waits for
        // the same poll (Switch back to Stable reads the result).
        follow() {
            if (this.polling) return this.polling;
            this.polling = (async () => {
                try {
                    for (;;) {
                        const s = await this.refresh();
                        if (!s || (s.state !== 'checking' && s.state !== 'downloading')) return s;
                        await sleep(s.state === 'downloading' ? 500 : 1000);
                    }
                } finally {
                    this.polling = null;
                }
            })();
            return this.polling;
        },

        async check() {
            if (window.IS_OFFLINE) return;
            try {
                this.status = await run('check-launcher-update');
            } catch (error) {
                console.error('launcher update: check failed', error);
                return;
            }
            this.render();
            await this.follow();
        },

        openPage() {
            run('open-launcher-release');
        },

        async act() {
            const s = this.status;
            if (!s) return;
            if (s.state === 'ready') return this.restart();
            if (s.state !== 'available' && s.state !== 'failed') return;
            if (!s.canInstall) {
                const manual = await window.showMessageBox(t('launcherUpdate.confirmTitle'),
                    t('launcherUpdate.confirmManual', { version: s.latest, current: s.current }),
                    [{ label: t('launcherUpdate.notNow'), cancel: true }, t('launcherUpdate.download')]);
                if (manual === 1) this.openPage();
                return;
            }

            const size = s.size ? formatBytes(s.size) : '';
            const choice = await window.showMessageBox(t('launcherUpdate.confirmTitle'),
                t('launcherUpdate.confirmBody', { version: s.latest, current: s.current, size }),
                [{ label: t('launcherUpdate.notNow'), cancel: true }, t('launcherUpdate.update')]);
            if (choice !== 1) return;
            // (Never a switch back: that is asked for in Settings each time.)
            await run('start-launcher-update');
            await this.follow();
        },

        async retry() {
            const s = this.status;
            if (!s?.failedFolder || !s.canInstall || window.IS_OFFLINE) return;
            const choice = await window.showMessageBox(t('launcherUpdate.tryAgain'),
                t('versionedUpdate.confirmRetry', { version: s.failedVersion }),
                [{ label: t('common.cancel'), cancel: true }, t('launcherUpdate.tryAgain')]);
            if (choice === 1 && await run('retry-launcher-version', { folder: s.failedFolder })) await this.refresh();
        },

        async restart() {
            const result = await run('restart-launcher-update');
            if (result && result.ok) {
                const text = el('launcher-update-text');
                if (text) text.textContent = t('launcherUpdate.restarting');
                const action = el('launcher-update-action');
                if (action) action.disabled = true;
                return;
            }
            if (result && result.error === 'busy') {
                await window.showMessageBox(t('launcherUpdate.busyTitle'), t('launcherUpdate.busyBody'), [t('common.ok')]);
            }
            await this.refresh();
        },

        // The bar's text and button for a state, or null when the bar stays hidden.
        barFor(s) {
            if (!s || !s.enabled) return null;
            const version = s.latest || s.readyVersion;
            switch (s.state) {
                case 'available':
                    if (s.installError) {
                        return { text: s.installError, action: t('launcherUpdate.tryAgain'), failed: true };
                    }
                    if (this.dismissed && this.dismissed === s.latest) return null;
                    return {
                        text: t('launcherUpdate.available', { version }),
                        action: t(s.canInstall ? 'launcherUpdate.update' : 'launcherUpdate.download'),
                        dismiss: true
                    };
                case 'downloading': {
                    const percent = s.total ? Math.min(100, Math.floor((s.done * 100) / s.total)) : 0;
                    return { text: t('launcherUpdate.downloading', { version, percent }), progress: percent };
                }
                case 'ready':
                    return {
                        text: s.installError || t(s.requestDeferred ? 'launcherUpdate.requestDeferred' : 'launcherUpdate.ready',
                            { version: s.readyVersion || version }),
                        action: t('launcherUpdate.restartNow'),
                        failed: !!s.installError
                    };
                case 'failed':
                    return { text: t('launcherUpdate.failed', { error: s.error }), action: t('launcherUpdate.tryAgain'), failed: true };
                default:
                    return s.installError ? { text: s.installError, failed: true, dismiss: true } : null;
            }
        },

        statusLine(s) {
            if (!s) return '';
            if (window.IS_OFFLINE) return t('launcherUpdate.offline');
            const bar = this.barFor(s);
            switch (s.state) {
                case 'checking': return t('launcherUpdate.checking');
                case 'up-to-date':
                    if (s.stableNeedsExtraction) return t('versionedUpdate.stableExtraction', { version: s.latest });
                    if (s.aheadOfStable) return t('launcherUpdate.aheadOfStable', { version: s.current, latest: s.latest });
                    return s.fromCache ? t('launcherUpdate.upToDateCached', { error: s.cacheError }) : t('launcherUpdate.upToDate');
                case 'check-failed': return t('launcherUpdate.checkFailed', { error: s.error });
                case 'available': return s.fromCache
                    ? t('launcherUpdate.availableCached', { version: s.latest, error: s.cacheError })
                    : t('launcherUpdate.available', { version: s.latest });
                default: return bar ? bar.text : (s.enabled ? '' : t('launcherUpdate.devBuild'));
            }
        },

        render() {
            const s = this.status;
            const bar = el('launcher-update-bar');
            if (bar) {
                const spec = this.barFor(s);
                bar.hidden = !spec;
                if (spec) {
                    bar.classList.toggle('is-failed', !!spec.failed);
                    bar.classList.toggle('is-ready', s.state === 'ready' && !spec.failed);
                    el('launcher-update-icon').setAttribute('href', '#icon-' + (spec.failed ? 'alert' : s.state === 'ready' ? 'check' : 'download'));
                    el('launcher-update-text').textContent = spec.text;
                    const action = el('launcher-update-action');
                    action.hidden = !spec.action;
                    action.disabled = s.state === 'ready' && !!window.isLauncherRestartBlocked?.();
                    action.textContent = spec.action || '';
                    const progress = el('launcher-update-progress');
                    progress.hidden = spec.progress === undefined;
                    progress.setAttribute('aria-valuenow', String(spec.progress || 0));
                    el('launcher-update-fill').style.width = `${spec.progress || 0}%`;
                    el('launcher-update-notes').hidden = !(s && s.latest);
                    el('launcher-update-dismiss').hidden = !spec.dismiss;
                }
            }

            const switchBack = el('switch-stable');
            if (switchBack) {
                switchBack.hidden = !s?.enabled || !s.aheadOfStable || (!s.canInstall && !s.stableNeedsExtraction) ||
                    !!window.IS_OFFLINE || s.channel !== 'stable' ||
                    ['checking', 'downloading', 'ready'].includes(s.state);
                switchBack.querySelector('span').textContent = t(s?.stableNeedsExtraction ?
                    'launcherUpdate.releaseNotes' : 'settings.goStable');
            }
            const retry = el('retry-launcher-version');
            if (retry) {
                retry.hidden = !s?.failedFolder || !s.canInstall || !!window.IS_OFFLINE ||
                    ['checking', 'downloading', 'ready'].includes(s.state);
                retry.textContent = t('versionedUpdate.retry', { version: s?.failedVersion || '' });
            }
            for (const channel of ['stable', 'beta']) {
                const option = el('channel-' + channel);
                if (option) option.disabled = s?.channelChangeAllowed === false;
            }
            window.paintLauncherSummary?.();
        }
    };

    window.LauncherUpdate = LauncherUpdate;
})();
