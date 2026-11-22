const assert = require('node:assert/strict');
const { page, test } = require('./harness');

const stage = (state, findings = []) => ({ state, findings });
const finding = (id, state, values = {}) => ({ id, state, values });
const setupResult = stages => ({ busy: false, stages });

function open(overrides = {}, scripts = []) {
    const requests = [];
    const setup = {
        'get-settings': { playerName: 'Player', language: 'en', theme: 'default', offline: true, channel: 'stable' },
        'get-show23': { ready: false, folder: '' },
        'get-launcher-update': { enabled: false },
        'browse-folder': 'Q:\\Fixture\\Destination',
        'browse-file': 'Q:\\Fixture\\Source.pkg',
        'setup-show23': { busy: true },
        'get-setup-inputs': setupResult({ check: stage('failed', [finding('base.complete', 'failed', { reason: 'truncated' })]) }),
        ...overrides
    };
    const view = page({
        scripts: ['core/i18n.js', 'core/focus-nav.js', 'app/mods-view.js', 'app/launcher-update.js', ...scripts, 'app/main.js'],
        backend: { handle(command, payload) {
            requests.push({ command, payload });
            const reply = setup[command];
            return typeof reply === 'function' ? reply(requests, payload) : (reply ?? null);
        } }
    });
    return { ...view, requests, id: name => view.document.getElementById(name) };
}

test('show23: setup captures the chosen inputs and stops before enabling Play', async () => {
    const view = open();
    await view.clock.advance(0);
    const id = view.id;
    assert.equal(id('game-state').textContent, 'Not set up');
    assert.equal(id('play').disabled, true);
    id('setup-open').click();
    assert.equal(id('setup').hidden, false);
    assert.equal(id('setup-continue').disabled, true);
    assert.equal(id('base-folder').getAttribute('aria-checked'), 'true');
    id('base-package').click();
    id('base-choose').click();
    id('install-folder').click();
    await view.clock.advance(0);
    assert.equal(id('setup-continue').disabled, false);
    id('setup-continue').click();
    await view.clock.advance(0);
    const call = view.requests.find(item => item.command === 'setup-show23');
    assert.equal(call.payload.kind, 'package');
    assert.equal(call.payload.base, 'Q:\\Fixture\\Source.pkg');
    assert.equal(call.payload.update, '');
    assert.equal(call.payload.install, 'Q:\\Fixture\\Destination');
    assert.match(id('setup-status').textContent, /incomplete|still downloading/i);
    assert.equal(id('setup-title').textContent, 'Set up stopped');
    assert.equal(id('setup-stages').querySelector('[data-stage="check"]').classList.contains('failed'), true);
    assert.equal(id('setup-stages').querySelectorAll('.todo').length, 4);
    assert.equal(id('setup-alert').hidden, false);
    assert.equal(view.document.activeElement, id('setup-retry'));
    assert.equal(id('play').disabled, true);
});

test('show23: the offline setting suppresses startup update checks', async () => {
    const view = open({ 'get-launcher-update': { enabled: true, state: 'idle' } });
    await view.clock.advance(0);
    assert.equal(view.id('check-parts').disabled, true);
    assert.equal(view.requests.some(item => item.command === 'check-launcher-update'), false);
});

test('show23: Ready comes from the native install state', async () => {
    const view = open({ 'get-show23': { ready: true, folder: 'Q:\\Fixture\\Destination' } });
    await view.clock.advance(0);
    assert.equal(view.id('game-state').textContent, 'Ready');
    assert.equal(view.id('play').disabled, false);
});

test('show23: Settings shows four installed versions and the unpublished-release result', async () => {
    const view = open({
        'get-settings': { language: 'en', theme: 'default', offline: false, channel: 'stable' },
        'get-launcher-update': { enabled: false, state: 'check-failed', error: 'No release has been published yet.' },
        'get-parts': { busy: false, parts: [
            { part: 'launcher', version: '0.1.0' },
            { part: 'emulator', version: '1.2.3-beta.1', prerelease: true },
            { part: 'server', version: '2.0.0' },
            { part: 'profile', version: '' }
        ] }
    });
    await view.clock.advance(0);
    view.id('settings').click();
    await view.clock.advance(0);
    assert.equal(view.id('parts').children.length, 4, String(view.logs.at(-1)?.[1] || ''));
    assert.equal(view.id('part-status-launcher').textContent, 'No release published yet');
    assert.match(view.id('parts').textContent, /1\.2\.3-beta\.1Beta/);
    assert.match(view.id('parts').textContent, /Not installed/);
});

test('night game: Home highlights Set up and disables the large Play button', async () => {
    const view = open();
    await view.clock.advance(0);
    const id = view.id;
    assert.equal(id('home-hero').querySelectorAll('.btn-primary.btn-lg').length, 1);
    assert.equal(id('setup-open').classList.contains('btn-primary'), true);
    assert.equal(id('play').disabled, true);
    assert.equal(id('play').classList.contains('btn-lg'), true);
    assert.equal(id('home-before').hidden, false);
    assert.equal(id('scorebug').hidden, true);
});

test('night game: Ready shows the game and four part versions from the native bridge', async () => {
    const parts = [
        { part: 'launcher', version: '2.5.6' },
        { part: 'emulator', version: '3.4.9-beta.2', prerelease: true },
        { part: 'server', version: '4.2.8' },
        { part: 'profile', version: '7.3.5' }
    ];
    const view = open({
        'get-settings': { channel: 'beta', offline: true, language: 'en' },
        'get-show23': { ready: true, version: '1.18', folder: 'Q:\\Fixture\\Game', updated: true },
        'get-parts': { parts }
    });
    await view.clock.advance(0);
    assert.equal(view.id('scorebug').hidden, false);
    assert.equal(view.id('scorebug-game').textContent, '1.18');
    for (const part of parts) {
        const cell = view.id('scorebug').querySelector('[data-part="' + part.part + '"]');
        assert.equal(cell.querySelector('.part-version').textContent, part.version);
    }
    assert.match(view.id('footer-version').textContent, /2\.5\.6/);
    assert.match(view.id('footer-channel').textContent, /Beta/);
});

test('night game: Set up replaces Home and its breadcrumb returns', async () => {
    const view = open();
    await view.clock.advance(0);
    const nav = view.window.FocusNav;
    nav.sync();
    view.id('setup-open').click();
    nav.sync();
    assert.equal(view.id('setup').hidden, false);
    assert.equal(view.id('home-hero').hidden, true);
    view.id('setup-back').click();
    assert.equal(view.id('setup').hidden, true);
    assert.equal(view.id('home-hero').hidden, false);
    assert.equal(view.document.activeElement, view.id('setup-open'));
    nav.sync();
    nav.press('Escape');
    assert.equal(view.id('setup').hidden, true, 'Back does not reopen the sub-view');
});

test('night game: the Settings channel radiogroup saves Beta', async () => {
    const view = open({
        'get-settings': (requests) => ({
            language: 'en', offline: true,
            channel: requests.some(row => row.command === 'save-setting') ? 'beta' : 'stable'
        }),
        'save-setting': true
    });
    await view.clock.advance(0);
    view.id('settings').click();
    await view.clock.advance(0);
    assert.equal(view.id('channel').getAttribute('role'), 'radiogroup');
    const beta = view.id('channel-beta');
    view.id('channel-stable').focus();
    view.window.FocusNav.press('ArrowRight', 'pad');
    await view.clock.advance(0);
    assert.equal(view.document.activeElement, beta);
    assert.equal(beta.getAttribute('aria-checked'), 'true');
    const save = view.requests.find(row => row.command === 'save-setting');
    assert.equal(save.payload.key, 'channel');
    assert.equal(save.payload.value, 'beta');
});

test('night game: an available empty Mods folder shows its three actions', async () => {
    const view = open({ 'get-mods': { installed: true, folder: 'Q:\\Fixture\\mods', mods: [] } });
    await view.clock.advance(0);
    view.id('mods').click();
    await view.clock.advance(0);
    assert.equal(view.id('home-hero').hidden, true);
    assert.equal(view.id('mods-panel').hidden, false);
    const empty = view.id('show23-mods-panel').querySelector('.empty');
    assert.equal(empty.querySelector('h2').textContent, 'No mods yet');
    assert.equal(empty.querySelectorAll('button').length, 3);
});

async function submitChecklist(view) {
    await view.clock.advance(0);
    view.id('setup-open').click();
    view.id('base-choose').click();
    view.id('install-folder').click();
    await view.clock.advance(0);
    view.id('setup-continue').click();
    await view.clock.advance(0);
}

test('setup: a base package with no unpacker stops after listing the input findings', async () => {
    const view = open({ 'get-setup-inputs': setupResult({
        check: stage('done', [
            finding('base.version', 'passed', { version: '1.00' }),
            finding('base.executable', 'later'), finding('update.offline', 'notice')
        ]),
        unpack: stage('stopped', [finding('unpack.noProgram', 'later')])
    }) });
    await submitChecklist(view);
    assert.equal(view.id('setup-stages').querySelector('[data-stage="check"]').classList.contains('done'), true);
    assert.equal(view.id('setup-stages').querySelector('[data-stage="unpack"]').classList.contains('stopped'), true);
    assert.match(view.id('setup-status').textContent, /no unpacker.*unpacked folder/i);
    assert.equal(view.id('setup-findings').children.length, 3);
    assert.match(view.id('setup-findings').textContent, /1\.00/);
    assert.equal(view.id('setup-retry').hidden, true);
    assert.equal(view.document.activeElement, view.id('setup-edit'));
});

test('setup: a package update with a folder base stops at Unpack', async () => {
    const view = open({ 'get-setup-inputs': setupResult({
        check: stage('done'), unpack: stage('stopped', [finding('unpack.mixed', 'later')])
    }) });
    await submitChecklist(view);
    assert.match(view.id('setup-status').textContent, /packages for both.*unpacked folders for both/i);
    assert.equal(view.id('setup-edit').hidden, false);
    assert.equal(view.id('setup-retry').hidden, true);
});

test('setup: an unpacked folder skips Unpack and successful Verify lists the findings', async () => {
    const view = open({ 'get-setup-inputs': setupResult({
        check: stage('done'), unpack: stage('not_needed', [finding('unpack.inPlace', 'passed')]),
        verify: stage('done', [finding('verify.directory', 'passed', { part: 'base', name: 'PS4', actual: 4 })]),
        settings: stage('done', [finding('settings.written', 'passed')]), finish: stage('done', [finding('finish.ready', 'passed')])
    }) });
    await submitChecklist(view);
    const unpack = view.id('setup-stages').querySelector('[data-stage="unpack"]');
    assert.equal(unpack.classList.contains('not-needed'), true);
    assert.match(unpack.querySelector('.stage-sub').textContent, /Not needed.*Using your unpacked folder where it is/);
    assert.equal(view.id('setup-verification').hidden, false);
    assert.match(view.id('setup-verify-findings').textContent, /PS4 sizes match the reference/);
    assert.match(view.id('setup-stages').querySelector('[data-stage="settings"] .stage-sub').textContent, /game record was written/);
    assert.equal(view.id('setup-alert').hidden, true);
    assert.equal(view.document.activeElement, view.id('setup-done'));
});

test('setup: Verify failure names the short directory and displays notice findings', async () => {
    const view = open({ 'get-setup-inputs': setupResult({
        check: stage('done'), unpack: stage('not_needed', [finding('unpack.inPlace', 'passed')]),
        verify: stage('failed', [
            finding('verify.directory', 'failed', { part: 'base', name: 'PS4', difference: 5 }),
            finding('verify.moduleDigest', 'notice', { part: 'update', name: 'libc.prx' })
        ])
    }) });
    await submitChecklist(view);
    assert.match(view.id('setup-status').textContent, /PS4 is incomplete \(5 B missing\)/);
    assert.match(view.id('setup-verify-findings').textContent, /checksum of libc\.prx differs/);
    assert.equal(view.id('setup-stages').querySelector('[data-stage="settings"]').classList.contains('todo'), true);
    assert.equal(view.id('setup-retry').hidden, false);
});

test('check inputs: a failed finding is the headline and Unpack never starts', async () => {
    const view = open({ 'get-setup-inputs': setupResult({ check: stage('failed', [
        finding('base.complete', 'failed', { reason: 'truncated' }), finding('install.local', 'passed')
    ]) }) });
    await submitChecklist(view);
    assert.equal(view.id('setup-stages').querySelector('[data-stage="check"]').classList.contains('failed'), true);
    assert.equal(view.id('setup-stages').querySelector('[data-stage="unpack"]').classList.contains('todo'), true);
    assert.match(view.id('setup-status').textContent, /incomplete|still downloading/i);
    assert.equal(view.id('setup-findings').children.length, 2);
    assert.equal(view.document.activeElement, view.id('setup-retry'));
});

test('setup: polling switches the running stage from Check inputs to Verify', async () => {
    let reply = { busy: true };
    const view = open({ 'get-setup-inputs': () => reply });
    await submitChecklist(view);
    assert.equal(view.id('setup-stages').querySelector('[data-stage="check"]').classList.contains('current'), true);
    assert.equal(view.id('setup-pending').hidden, false);
    assert.equal(view.id('setup-checklist').hidden, true);
    reply = { busy: true, stages: {
        check: stage('done', [finding('install.space', 'passed', { kind: 'in_place', needed: 2 ** 31, available: 2 ** 34, drive: 'Q:' })]),
        unpack: stage('not_needed', [finding('unpack.inPlace', 'passed')]), verify: stage('running')
    } };
    await view.clock.advance(160);
    assert.equal(view.id('setup-stages').querySelector('[data-stage="verify"]').classList.contains('current'), true);
    assert.match(view.id('setup-findings').textContent, /game stays where it is.*2 GiB.*16 GiB/);
    reply = setupResult({ ...reply.stages, verify: stage('done'), settings: stage('done'), finish: stage('done') });
    await view.clock.advance(160);
    assert.equal(view.id('setup-pending').hidden, true);
});

test('show23: Game set up with no runtime shows the saved version and secondary Set up again', async () => {
    const view = open({ 'get-show23': { ready: false, setUp: true, version: '1.21', updated: true } });
    await view.clock.advance(0);
    assert.equal(view.id('game-state').textContent, 'Game set up');
    assert.match(view.id('game-meta').textContent, /1\.21/);
    assert.match(view.id('game-source-notice').textContent, /emulator and local server are not installed/i);
    assert.equal(view.id('setup-open-label').textContent, 'Set up again');
    assert.equal(view.id('setup-open').classList.contains('btn-secondary'), true);
    assert.equal(view.id('play').disabled, true);
});

test('show23: missing recorded game folder shows its path and primary Set up again', async () => {
    const sourcePath = 'Q:\\Fixture\\Missing';
    const view = open({ 'get-show23': { ready: false, gameMissing: true, sourcePath } });
    await view.clock.advance(0);
    assert.equal(view.id('game-state').textContent, 'Game folder not found');
    assert.ok(view.id('game-source-notice').textContent.includes(sourcePath));
    assert.equal(view.id('setup-open-label').textContent, 'Set up again');
    assert.equal(view.id('setup-open').classList.contains('btn-primary'), true);
    assert.equal(view.id('play').disabled, true);
});

test('show23: Verify Files displays game findings alongside the runtime message', async () => {
    const view = open({
        'get-show23': { ready: false, setUp: true, version: '1.00' },
        'verify-show23': true,
        'get-verify': { busy: false, message: 'No active runtime to verify.', findings: [
            finding('verify.executable', 'passed', { part: 'base' }),
            finding('verify.directory', 'notice', { part: 'base', name: 'FRONTEND', difference: 8 })
        ] }
    });
    await view.clock.advance(0);
    view.id('verify').click();
    await view.clock.advance(0);
    assert.equal(view.id('game-verification').hidden, false);
    assert.match(view.id('game-status').textContent, /No active runtime/);
    assert.match(view.id('game-verify-findings').textContent, /executable checksum matches/);
    assert.match(view.id('game-verify-findings').textContent, /FRONTEND has extra files/);
});

test('package source exposes the unpacker picker, folder source hides it', async () => {
    const view = open();
    await view.clock.advance(0);
    view.id('setup-open').click();
    assert.equal(view.id('setup-unpacker').hidden, true);
    view.id('base-package').click();
    assert.equal(view.id('setup-unpacker').hidden, false);
    view.id('base-folder').click();
    assert.equal(view.id('setup-unpacker').hidden, true);
});

test('package setup displays real byte progress and cancellation retains the selections', async () => {
    let canceled = false;
    const view = open({
        'get-setup-inputs': () => ({
            busy: !canceled, canceled, canCancel: !canceled,
            unpackProgress: { elapsed: 4, written: 45, expected: 100 },
            stages: { check: stage('done'), unpack: stage(canceled ? 'stopped' : 'running') }
        }),
        'cancel-setup': () => { canceled = true; return true; }
    });
    await submitChecklist(view);
    assert.match(view.id('unpack-progress').textContent, /4.*45 B.*100 B/);
    const base = view.id('base-path').value;
    view.id('cancel-unpack').click();
    await view.clock.advance(160);
    assert.equal(view.id('setup-picker').hidden, false);
    assert.equal(view.id('base-path').value, base);
});

test('installed copy offers removal only after confirmation', async () => {
    const view = open({ 'get-show23': { setUp: true, installedCopy: true }, 'remove-game-copy': true });
    await view.clock.advance(0);
    let confirm;
    view.window.showMessageBox = async (...args) => { confirm = args; return 0; };
    view.id('remove-game-copy').click();
    await view.clock.advance(0);
    assert.match(confirm[0], /Remove game copy/);
    assert.equal(view.requests.some(item => item.command === 'remove-game-copy'), false);
    view.window.showMessageBox = async () => 1;
    view.id('remove-game-copy').click();
    await view.clock.advance(0);
    assert.equal(view.requests.some(item => item.command === 'remove-game-copy'), true);
});


test('Home keeps Remove game copy hidden for in-place setup', async () => {
    const view = open({ 'get-show23': { setUp: true, installedCopy: false } });
    await view.clock.advance(0);
    assert.equal(view.id('remove-game-copy').hidden, true);
});


test('package setup requests consent for the exact displayed file before retrying', async () => {
    const path = 'Q:/Fixture/orbis-unpkg.exe';
    let submissions = 0;
    const view = open({
        'setup-show23': () => ++submissions === 1
            ? setupResult({ unpack: stage('stopped', [finding('unpack.consent', 'later', { path, consent: 'digest:path' })]) })
            : setupResult({ unpack: stage('done'), finish: stage('done') })
    });
    let prompt;
    view.window.showMessageBox = async (...args) => { prompt = args; return 1; };
    await submitChecklist(view);
    assert.ok(prompt[1].includes(path));
    assert.match(prompt[1], /will run with your permissions/);
    const calls = view.requests.filter(item => item.command === 'setup-show23');
    assert.equal(calls.length, 2);
    assert.equal(calls[0].payload.unpackerConsent, undefined);
    assert.equal(calls[1].payload.unpackerConsent, 'digest:path');
});

test('package setup passes the keep decision back to the installer', async () => {
    let submissions = 0;
    const view = open({
        'setup-show23': () => ++submissions === 1
            ? setupResult({ unpack: stage('stopped', [finding('unpack.existing', 'later')]) })
            : setupResult({ unpack: stage('not_needed'), finish: stage('done') })
    });
    view.window.showMessageBox = async () => 1;
    await submitChecklist(view);
    const calls = view.requests.filter(item => item.command === 'setup-show23');
    assert.equal(calls.length, 2);
    assert.equal(calls[1].payload.existingCopy, 'keep');
});


test('Play shows Playing and Stop drives the session to a result', async () => {
    let played = false;
    let stopped = false;
    const view = open({
        'get-show23': { ready: true, setUp: true, updated: true },
        'play-show23': () => { played = true; return true; },
        'stop-play': () => { stopped = true; return true; },
        'get-play': () => stopped ? { phase: 'idle', result: 'stopped' } :
            played ? { phase: 'playing' } : { phase: 'idle' }
    });
    await view.clock.advance(0);
    assert.match(view.id('game-source-notice').textContent, /1\.00/);
    view.id('play').click();
    await view.clock.advance(0);
    assert.equal(view.id('game-state').textContent, 'Playing');
    assert.equal(view.id('stop-play').hidden, false);
    view.id('stop-play').click();
    await view.clock.advance(300);
    assert.equal(view.id('stop-play').hidden, true);
    assert.match(view.id('play-result-text').textContent, /stopped/i);
});

test('Play failure shows the diagnostic log in the result panel', async () => {
    const view = open({
        'get-show23': { ready: true },
        'play-show23': true,
        'get-play': { phase: 'idle', result: 'emulatorExit', code: 12, log: 'Wrong executable' }
    });
    await view.clock.advance(0);
    view.id('play').click();
    await view.clock.advance(0);
    assert.equal(view.id('play-result').hidden, false);
    assert.match(view.id('play-result-text').textContent, /12/);
    assert.equal(view.id('play-result-log').textContent, 'Wrong executable');
});


test('Settings saves a trimmed player name after checking length and characters', async () => {
    const view = open({ 'get-settings': { language: 'en', playerName: '' }, 'save-setting': true });
    await view.clock.advance(0);
    const input = view.id('player-name');
    const save = view.id('save-player-name');
    assert.equal(input.value, '');
    for (const invalid of ['AB', 'ABCDEFGHIJKLMNOPQ', 'Bad!Name', 'No\tTabs', 'Élodie']) {
        input.value = invalid;
        save.click();
        await view.clock.advance(0);
        assert.match(view.id('player-name-status').textContent, /3 to 16/);
    }
    assert.equal(view.requests.filter(row => row.command === 'save-setting').length, 0);
    input.value = '  Batter_3-Home  ';
    save.click();
    await view.clock.advance(0);
    const saved = view.requests.find(row => row.command === 'save-setting');
    assert.equal(saved.payload.key, 'playerName');
    assert.equal(saved.payload.value, 'Batter_3-Home');
    assert.equal(input.value, 'Batter_3-Home');
});

test('First Play prefills a name and validates before saving and starting', async () => {
    const view = open({
        'get-settings': { language: 'en', playerName: '' },
        'get-show23': { ready: true },
        'suggest-player-name': 'Suggested',
        'save-setting': true, 'play-show23': true, 'get-play': { phase: 'idle', result: 'finished' }
    });
    await view.clock.advance(0);
    view.id('play').click();
    await view.clock.advance(0);
    const input = view.id('first-player-name');
    const dialog = view.id('player-name-dialog');
    const submit = view.id('save-player-name-play');
    assert.equal(dialog.hidden, false);
    assert.equal(input.value, 'Suggested');
    assert.equal(view.document.activeElement, input);
    input.value = '?';
    submit.click();
    await view.clock.advance(0);
    assert.equal(dialog.hidden, false);
    assert.equal(view.requests.some(row => row.command === 'play-show23'), false);
    input.value = '  Edited Name  ';
    submit.click();
    await view.clock.advance(0);
    assert.equal(dialog.hidden, true);
    const relevant = view.requests.filter(row => ['save-setting', 'play-show23'].includes(row.command));
    assert.equal(relevant[0].payload.value, 'Edited Name');
    assert.equal(relevant[1].payload.playerName, 'Edited Name');
    view.id('play').click();
    await view.clock.advance(0);
    assert.equal(view.requests.filter(row => row.command === 'suggest-player-name').length, 1);
    assert.equal(view.requests.filter(row => row.command === 'play-show23').length, 2);
});

test('Dismissing the name prompt saves the suggestion and starts Play', async () => {
    for (const method of ['close', 'escape']) {
        const view = open({
            'get-settings': { language: 'en', playerName: '' },
            'get-show23': { ready: true },
            'suggest-player-name': 'Suggested',
            'save-setting': true, 'play-show23': true, 'get-play': { phase: 'idle', result: 'finished' }
        });
        await view.clock.advance(0);
        view.id('play').click();
        await view.clock.advance(0);
        view.id('first-player-name').value = 'Discard Me';
        if (method === 'escape') {
            view.id('first-player-name').dispatchEvent(new view.window.KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
        } else view.id('dismiss-player-name').click();
        await view.clock.advance(0);
        assert.equal(view.id('player-name-dialog').hidden, true);
        assert.equal(view.requests.find(row => row.command === 'save-setting').payload.value, 'Suggested');
        assert.equal(view.requests.find(row => row.command === 'play-show23').payload.playerName, 'Suggested');
    }
});

test('The profile warning appears alongside Play status and the exit result', async () => {
    let finished = false;
    const view = open({
        'get-show23': { ready: true },
        'play-show23': true,
        'get-play': () => finished ? { phase: 'idle', result: 'finished', nameWarning: 'malformed' } :
            { phase: 'playing', nameWarning: 'malformed' }
    });
    await view.clock.advance(0);
    view.id('play').click();
    await view.clock.advance(0);
    assert.match(view.id('player-name-warning').textContent, /malformed.*unchanged/);
    assert.equal(view.id('game-state').textContent, 'Playing');
    finished = true;
    await view.clock.advance(250);
    assert.match(view.id('play-result-text').textContent, /exited/);
    assert.equal(view.id('player-name-warning').hidden, false);
});


test('Cards shows import progress, then the completed count and date', async () => {
    let running = false;
    let done = false;
    const view = open({
        'get-settings': { language: 'en' },
        'get-show23': { ready: true },
        'get-card-import': () => done ? { result: 'complete', cards: 4101, date: '2026-11-21' } :
            running ? { busy: true, page: 17, total: 165 } : {},
        'start-card-import': () => { running = true; return 'started'; }
    });
    await view.clock.advance(0);
    assert.match(view.id('cards-status').textContent, /not imported/i);
    view.id('import-cards').click();
    await view.clock.advance(0);
    assert.equal(view.id('cards-progress').getAttribute('value'), '17');
    assert.match(view.id('cards-progress-text').textContent, /17.*165/);
    assert.equal(view.id('play').disabled, true);
    done = true;
    await view.clock.advance(250);
    assert.match(view.id('cards-status').textContent, /4,101.*2026/);
    assert.equal(view.id('import-cards').textContent, 'Update special cards');
    assert.equal(view.id('play').disabled, false);
});

test('Cards reports a resumable network failure and supports Cancel', async () => {
    let outcome = { result: 'network' };
    const view = open({
        'get-settings': { language: 'en' },
        'get-show23': { ready: true },
        'get-card-import': () => outcome,
        'start-card-import': () => { outcome = { busy: true }; return 'started'; },
        'cancel-card-import': () => { outcome = { result: 'canceled' }; return true; }
    });
    await view.clock.advance(0);
    assert.match(view.id('cards-result').textContent, /finished pages are kept/i);
    view.id('import-cards').click();
    await view.clock.advance(0);
    view.id('cancel-card-import').click();
    await view.clock.advance(250);
    assert.equal(view.id('cancel-card-import').hidden, true);
    assert.match(view.id('cards-result').textContent, /canceled/i);
});

test('Cards explains why Offline mode or Play disables import', async () => {
    for (const [offline, blocked, explanation] of [[true, 'offline', /offline/i], [false, 'playing', /stop.*game/i]]) {
        const view = open({
            'get-settings': { language: 'en', offline },
            'get-show23': { ready: true },
            'get-card-import': { blocked }
        });
        await view.clock.advance(0);
        assert.equal(view.id('import-cards').disabled, true);
        assert.match(view.id('cards-blocked').textContent, explanation);
    }
});


test('Finishing setup offers optional import only once', async () => {
    let submitted = false;
    const view = open({
        'get-settings': { language: 'en', offline: false, playerName: 'Player' },
        'get-show23': () => ({ ready: submitted, setUp: submitted }),
        'setup-show23': () => { submitted = true; return { busy: true }; },
        'get-setup-inputs': setupResult({ finish: stage('done') }),
        'save-setting': true,
        'get-card-import': {}
    });
    await submitChecklist(view);
    assert.equal(view.messages.length, 1);
    assert.equal(view.messages[0][0], 'Import special cards now?');
    assert.equal(view.requests.filter(row => row.command === 'start-card-import').length, 0);
    assert.ok(view.requests.some(row => row.command === 'save-setting' && row.payload.key === 'cardsOffered'));
    view.id('setup-done').click();
    await view.clock.advance(0);
    await submitChecklist(view);
    assert.equal(view.messages.length, 1);
});

test('Cards displays the log path on other import failures', async () => {
    const view = open({
        'get-settings': { language: 'en' },
        'get-show23': { ready: true },
        'get-card-import': { result: 'failed', log: 'Q:/Fixture/card-import.log' }
    });
    await view.clock.advance(0);
    assert.match(view.id('cards-result').textContent, /import failed.*card-import\.log/i);
});


if (process.env.SHOW23_CARD_UI_EVIDENCE === '1') {
    test('Cards captures fixture states for review', async () => {
        const fs = require('node:fs');
        const path = require('node:path');
        const destination = path.resolve(__dirname, '../../../build/card-import-evidence');
        fs.mkdirSync(destination, { recursive: true });
        const escape = value => String(value).replaceAll('&', '&amp;').replaceAll('<', '&lt;')
            .replaceAll('>', '&gt;').replaceAll('"', '&quot;');
        function markup(node) {
            if (!node.tagName) return escape(node.textContent);
            const tag = node.tagName.toLowerCase();
            const attr = ['id', 'class', 'role', 'max', 'value', 'for', 'aria-labelledby', 'aria-modal']
                .filter(key => node.hasAttribute(key)).map(key => ' ' + key + '="' + escape(node.getAttribute(key)) + '"').join('');
            return '<' + tag + attr + (node.hidden ? ' hidden' : '') + (node.disabled ? ' disabled' : '') + '>' +
                Array.from(node.childNodes, markup).join('') + '</' + tag + '>';
        }
        const css = fs.readFileSync(path.resolve(__dirname, '../../launcher-ui/assets/css/main.css'), 'utf8');
        const complete = { cards: 4101, date: '2026-11-21' };
        const states = {
            empty: {}, importing: { busy: true, page: 73, total: 165 }, imported: complete,
            network: { ...complete, result: 'network' }, offline: { blocked: 'offline' }, offer: {}
        };
        const rows = [];
        for (const [name, cards] of Object.entries(states)) {
            const view = open({
                'get-settings': { language: 'en', playerName: 'Player' },
                'get-show23': { ready: true, setUp: true },
                'get-card-import': cards
            }, ['ui/message-box.js']);
            await view.clock.advance(0);
            if (name === 'offer') {
                const t = view.window.LauncherI18n.t;
                void view.window.showMessageBox(t('cards.offer'), t('cards.note'),
                    [{ label: t('cards.later'), cancel: true }, t('cards.import')]);
                await view.clock.advance(0);
            }
            const panel = view.id('cards-title').parentNode;
            const content = name === 'offer' ? view.id('message-box').querySelector('.dialog') : panel;
            rows.push('<article><h2>' + escape(name) + '</h2>' + markup(content) + '</article>');
        }
        fs.writeFileSync(path.join(destination, 'cards-states.html'),
            '<!doctype html><meta charset="utf-8"><title>Cards interface fixtures</title>' +
                '<style>' + css + '\nbody{height:auto;overflow:auto;padding:24px;display:grid;gap:22px}' +
                'article{max-width:760px}.dialog{position:relative}</style>' + rows.join(''), 'utf8');
    });
}


test('Resolution defaults to 1080p and saves either choice', async () => {
    let resolution;
    const view = open({
        'get-settings': () => ({ language: 'en', resolution }),
        'save-setting': (_requests, payload) => {
            assert.equal(payload.key, 'resolution');
            resolution = payload.value;
            return true;
        }
    });
    await view.clock.advance(0);
    const low = view.id('resolution-720p'), high = view.id('resolution-1080p');
    assert.equal(high.getAttribute('aria-checked'), 'true');
    low.click();
    await view.clock.advance(0);
    assert.equal(resolution, '720p');
    assert.equal(low.getAttribute('aria-checked'), 'true');
    high.click();
    await view.clock.advance(0);
    assert.equal(resolution, '1080p');
    assert.equal(high.getAttribute('aria-checked'), 'true');
});

test('Play explains that a malformed config was left untouched', async () => {
    const view = open({
        'get-show23': { ready: true },
        'play-show23': true,
        'get-play': { phase: 'idle', result: 'resolutionMalformed' }
    });
    await view.clock.advance(0);
    view.id('play').click();
    await view.clock.advance(0);
    assert.match(view.id('play-result-text').textContent, /config\.json.*unchanged.*Play/i);
});
