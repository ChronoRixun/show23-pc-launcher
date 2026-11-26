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

test('show23: a fresh profile explains the first launch and labels the game engine', async () => {
    const view = open({
        'get-show23': { ready: true, firstStart: true },
        'get-parts': { parts: [{ part: 'emulator', version: '2.0.0' }] }
    });
    await view.clock.advance(0);
    assert.match(view.id('game-source-notice').textContent, /first launch can take a few minutes/i);
    assert.match(view.id('scorebug').querySelector('[data-part="emulator"]').textContent, /game engine/i);
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
    assert.match(view.id('game-source-notice').textContent, /game engine and local server are unavailable/i);
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


test('Play shows preparation and keeps Stop available before the game starts', async () => {
    let preparing = false;
    let stopped = false;
    const view = open({
        'get-show23': { ready: true, folder: 'Q:\\Fixture\\Destination' },
        'play-show23': () => { preparing = true; return true; },
        'stop-play': () => { stopped = true; return true; },
        'get-play': () => stopped ? { phase: 'idle', result: 'stopped' } :
            preparing ? { phase: 'preparing' } : { phase: 'idle' }
    });
    await view.clock.advance(0);
    view.id('play').click();
    await view.clock.advance(0);
    assert.match(view.id('game-state').textContent, /preparing game files/i);
    assert.equal(view.id('stop-play').hidden, false);
    view.id('stop-play').click();
    await view.clock.advance(250);
    assert.equal(stopped, true);
    assert.equal(view.id('stop-play').hidden, true);
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
    assert.match(view.id('game-state').textContent, /Playing.*first start/i);
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
        'get-play': { phase: 'idle', result: 'prepareExit', code: 19, log: 'Preparation failed' }
    });
    await view.clock.advance(0);
    view.id('play').click();
    await view.clock.advance(0);
    assert.equal(view.id('play-result').hidden, false);
    assert.match(view.id('play-result-text').textContent, /preparation exited with code 19/i);
    assert.equal(view.id('play-result-log').textContent, 'Preparation failed');
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

test('Play shows an engine failure and its log', async () => {
    let finished = false;
    const view = open({
        'get-show23': { ready: true },
        'play-show23': true,
        'get-play': () => finished ? { phase: 'idle', result: 'engineExit', code: 12, log: 'Runtime failed' } :
            { phase: 'playing' }
    });
    await view.clock.advance(0);
    view.id('play').click();
    await view.clock.advance(0);
    assert.match(view.id('game-state').textContent, /Playing.*first start/i);
    finished = true;
    await view.clock.advance(250);
    assert.match(view.id('play-result-text').textContent, /engine exited with code 12/);
    assert.equal(view.id('play-result-log').textContent, 'Runtime failed');
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


test('Settings groups controls by the player tasks in every language', async () => {
    const expected = [
        ['settings-game', ['player-name', 'resolution', 'fullscreen', 'unlock-all-cards']],
        ['settings-updates', ['channel', 'parts', 'check-parts']],
        ['settings-setup', ['settings-unpacker-path', 'location', 'switch-mode']],
        ['settings-launcher', ['offline', 'language', 'use-this-copy']]
    ];
    const headings = {
        en: ['Game', 'Updates', 'Setup and files', 'Launcher'],
        fr: ['Jeu', 'Mises à jour', 'Installation et fichiers', 'Lanceur'],
        es: ['Juego', 'Actualizaciones', 'Instalación y archivos', 'Iniciador'],
        ru: ['Игра', 'Обновления', 'Установка и файлы', 'Лаунчер']
    };
    for (const [language, labels] of Object.entries(headings)) {
        const view = open({ 'get-settings': { language } });
        await view.clock.advance(0);
        view.id('settings').click();
        await view.clock.advance(0);
        const cards = [...view.id('settings-page').querySelectorAll('.panel')];
        assert.deepEqual(cards.map(card => card.id), expected.map(([id]) => id));
        assert.deepEqual(cards.map(card => card.querySelector('.section-h').textContent), labels);
        expected.forEach(([id, controls]) => {
            assert.deepEqual(controls, [...view.id(id).querySelectorAll('[id]')]
                .map(control => control.id).filter(control => controls.includes(control)));
        });
    }
});

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
    const choice = view.id('resolution');
    assert.equal(choice.tagName, 'SELECT');
    assert.equal(choice.value, '1080p');
    view.id('settings').click();
    await view.clock.advance(0);
    choice.focus();
    const nav = view.window.FocusNav;
    nav.press('Enter', 'pad');
    nav.press('ArrowUp', 'pad');
    nav.press('Enter', 'pad');
    await view.clock.advance(0);
    assert.equal(resolution, '720p');
    assert.equal(choice.value, '720p');
    choice.value = '1080p';
    choice.dispatchEvent(new view.window.Event('change', { bubbles: true }));
    await view.clock.advance(0);
    assert.equal(resolution, '1080p');
    assert.equal(choice.value, '1080p');
});

test('Unlock all cards defaults off and writes the selected profile setting', async () => {
    let unlockAllCards = false;
    const view = open({
        'get-settings': () => ({ language: 'en', unlockAllCardsAvailable: true, unlockAllCards }),
        'save-setting': (_requests, payload) => {
            assert.equal(payload.key, 'unlockAllCards');
            assert.ok(payload.value === 'true' || payload.value === 'false');
            unlockAllCards = payload.value === 'true';
            return true;
        }
    });
    await view.clock.advance(0);
    view.id('settings').click();
    await view.clock.advance(0);
    const toggle = view.id('unlock-all-cards');
    assert.equal(toggle.checked, false);
    assert.equal(toggle.disabled, false);
    toggle.checked = true;
    toggle.dispatchEvent(new view.window.Event('change', { bubbles: true }));
    await view.clock.advance(0);
    assert.equal(unlockAllCards, true);
    assert.equal(toggle.checked, true);
    toggle.checked = false;
    toggle.dispatchEvent(new view.window.Event('change', { bubbles: true }));
    await view.clock.advance(0);
    assert.equal(unlockAllCards, false);
    assert.equal(toggle.checked, false);
});

test('Play explains when preparation did not produce a usable cache', async () => {
    const view = open({
        'get-show23': { ready: true },
        'play-show23': true,
        'get-play': { phase: 'idle', result: 'prepareOutput' }
    });
    await view.clock.advance(0);
    view.id('play').click();
    await view.clock.advance(0);
    assert.match(view.id('play-result-text').textContent, /usable cache/i);
});


if (process.env.SHOW23_SETTINGS_UI_EVIDENCE === '1') {
    test('Settings exports localized layouts for visual review', async () => {
        const fs = require('node:fs');
        const path = require('node:path');
        const ui = path.resolve(__dirname, '../../launcher-ui');
        const destination = path.resolve(__dirname, '../../../build/settings-evidence');
        fs.mkdirSync(destination, { recursive: true });
        for (const language of ['en', 'fr', 'es', 'ru']) {
            const view = open({
                'get-settings': {
                    language, playerName: 'Player', resolution: '1080p', channel: 'stable',
                    unpacker: 'Q:\\Tools\\orbis-unpkg.exe', offline: true, offlineSaved: true
                },
                'get-parts': {
                    parts: ['launcher', 'emulator', 'server', 'profile'].map(part => ({
                        part, version: '0.1.1'
                    }))
                }
            });
            await view.clock.advance(0);
            view.id('settings').click();
            await view.clock.advance(0);
            view.id('location-portable').click();
            await view.clock.advance(0);

            // Reflect harness-only form properties in the exported static document.
            for (const input of view.document.querySelectorAll('input')) {
                if (input.value) input.setAttribute('value', input.value);
                if (input.checked) input.setAttribute('checked', '');
            }
            for (const select of view.document.querySelectorAll('select')) {
                for (const option of select.options) {
                    if (option.value === select.value) option.setAttribute('selected', '');
                    else option.removeAttribute('selected');
                }
            }
            const source = fs.readFileSync(path.join(ui, 'main.html'), 'utf8');
            const uri = require('node:url').pathToFileURL(ui + path.sep).href;
            const header = source.slice(0, source.indexOf('<body>'))
                .replace(/<meta http-equiv="Content-Security-Policy"[^>]*>\s*/g, '')
                .replace(/<script[^>]*><\/script>\s*/g, '')
                .replace('<head>', '<head><base href="' + uri + '">');
            fs.writeFileSync(path.join(destination, 'settings-' + language + '.html'),
                header + '<body>' + view.document.body.innerHTML + '</body></html>', 'utf8');
        }
    });
}


test('Launcher shortcuts action and confirmation are localized in all four languages', async () => {
    const labels = {
        en: ['Use this copy for shortcuts', 'Both shortcuts now open this copy.'],
        fr: ['Utiliser cette copie pour les raccourcis', 'Les deux raccourcis ouvrent maintenant cette copie.'],
        es: ['Usar esta copia para los accesos directos', 'Los dos accesos directos abren ahora esta copia.'],
        ru: ['Использовать эту копию для ярлыков', 'Теперь оба ярлыка открывают эту копию.']
    };
    for (const [language, [label, confirmation]] of Object.entries(labels)) {
        const view = open({ 'get-settings': { language }, 'use-this-copy': { desktop: true, startMenu: true } });
        await view.clock.advance(0);
        view.id('settings').click();
        await view.clock.advance(0);
        const action = view.id('use-this-copy');
        assert.equal(action.textContent.trim(), label);
        action.click();
        await view.clock.advance(0);
        assert.equal(view.requests.filter(entry => entry.command === 'use-this-copy').length, 1);
        assert.deepEqual(view.toasts.at(-1), [confirmation, 'success']);
    }
});

test('Launcher shortcuts action reports a partial update', async () => {
    const view = open({
        'get-settings': { language: 'en' }, 'use-this-copy': { desktop: true, startMenu: false }
    });
    await view.clock.advance(0);
    view.id('settings').click();
    await view.clock.advance(0);
    view.id('use-this-copy').click();
    await view.clock.advance(0);
    assert.deepEqual(view.toasts.at(-1), ['Updated the desktop shortcut. Could not update the Start menu shortcut.', 'error']);
});


test('Bundled parts identify the launcher download in every language', async () => {
    const labels = {
        en: ['Included with the launcher', 'Bundled in the launcher download.'],
        fr: ['Inclus avec le lanceur', 'Fait partie du téléchargement du lanceur.'],
        es: ['Incluido con el iniciador', 'Incluido en la descarga del iniciador.'],
        ru: ['Входит в комплект лаунчера', 'Входит в загружаемый комплект лаунчера.']
    };
    for (const [language, [label, hint]] of Object.entries(labels)) {
        const view = open({
            'get-settings': { language, offline: false, channel: 'stable' },
            'get-parts': { parts: [
                { part: 'emulator', version: '0.1.0', bundled: true, state: 'unpublished' },
                { part: 'server', version: '0.1.0', bundled: true, state: 'available', availableVersion: '0.1.3' },
                { part: 'profile', version: '0.1.0', bundled: true, state: 'error', status: 'Bad manifest' }
            ] }
        });
        await view.clock.advance(0);
        view.id('settings').click();
        await view.clock.advance(0);
        const cell = view.id('part-status-emulator');
        assert.equal(cell.textContent, label);
        assert.ok(cell.querySelector('.dot.ok'));
        assert.equal(cell.querySelector('.status-text').getAttribute('title'), hint);
        assert.equal(view.id('part-status-server').textContent, label);
        assert.equal(view.id('part-status-profile').textContent, label);
    }
});

test('Unbundled or offline parts keep their existing status', async () => {
    for (const [offline, expected] of [[false, 'No release published yet'], [true, 'Included with the launcher']]) {
        const view = open({
            'get-settings': { language: 'en', offline },
            'get-parts': { parts: [{ part: 'emulator', state: 'unpublished', bundled: offline }] }
        });
        await view.clock.advance(0);
        assert.equal(view.id('part-status-emulator').textContent, expected);
    }
});

test('versioned launch keeps Play and update checks closed until the trial is committed', async () => {
    let polls = 0;
    const view = open({
        'get-settings': { language: 'en', offline: false },
        'get-startup': () => ++polls < 2 ? { waiting: true } : { notices: 'rebuilt\ninvalid-request\n' },
        'get-show23': { ready: true },
        'get-launcher-update': { enabled: true, state: 'idle' }
    });
    await view.clock.advance(0);
    assert.equal(view.id('play').disabled, true);
    assert.match(view.id('startup-notice').textContent, /Confirming/);
    assert.equal(view.requests.some(item => item.command === 'check-launcher-update'), false);
    assert.deepEqual(view.requests.filter(item => ['show', 'confirm-launcher-started'].includes(item.command))
        .map(item => item.command), ['show', 'confirm-launcher-started']);
    await view.clock.advance(101);
    assert.equal(view.id('play').disabled, false);
    assert.match(view.id('startup-notice').textContent, /rebuilt.*invalid version request/);
});

test('versioned launch keeps a deferred start read-only and explains the restart', async () => {
    const view = open({ 'get-startup': { deferred: true }, 'get-show23': { ready: true } });
    await view.clock.advance(0);
    assert.equal(view.id('play').disabled, true);
    assert.equal(view.id('setup-open').inert, true);
    assert.match(view.id('startup-notice').textContent, /Restart the launcher/);
});

test('Home exposes explicit Repair for a damaged runtime and disables it while in use', async () => {
    let inUse = true;
    const view = open({
        'get-show23': () => ({ setUp: true, ready: false, runtimeRepairable: true, runtimeInUse: inUse, runtimeError: 'server/olserver.py' }),
        'repair-runtime': true,
        'get-runtime-repair': { busy: false, success: true }
    });
    await view.clock.advance(0);
    assert.equal(view.id('play').disabled, true);
    assert.equal(view.id('repair-runtime').hidden, false);
    assert.equal(view.id('repair-runtime').disabled, true);
    assert.match(view.id('game-source-notice').textContent, /server\/olserver\.py/);
    inUse = false;
    await view.clock.advance(1000);
    assert.equal(view.id('repair-runtime').disabled, false);
    view.id('repair-runtime').click();
    await view.clock.advance(0);
    assert.equal(view.requests.some(item => item.command === 'repair-runtime'), false);
    assert.equal(view.messages.length, 1);
    assert.match(view.messages[0][1], /same runtime version/);
});

test('Home never offers repair for an unidentifiable runtime', async () => {
    const view = open({ 'get-show23': { setUp: true, ready: false, runtimeError: 'bad runtime marker' } });
    await view.clock.advance(0);
    assert.equal(view.id('play').disabled, true);
    assert.equal(view.id('repair-runtime').hidden, true);
    assert.match(view.id('game-source-notice').textContent, /bad runtime marker/);
});

test('runtime update is confirmed once from Settings and reports the activated version', async () => {
    let installed = false;
    const view = open({
        'get-show23': () => installed ? { ready: true, runtimeRollbackVersion: '0.2.0' } :
            { ready: true, runtimeUpdateVersion: '0.2.1' },
        'change-runtime': () => { installed = true; return true; },
        'get-runtime-repair': { busy: false, success: true }
    });
    view.window.showMessageBox = async () => 1;
    await view.clock.advance(0);
    assert.equal(view.id('update-runtime').hidden, false);
    view.id('settings').click();
    await view.clock.advance(0);
    view.id('settings-update-runtime').click();
    await view.clock.advance(0);
    const requests = view.requests.filter(item => item.command === 'change-runtime');
    assert.equal(requests.length, 1);
    assert.equal(requests[0].payload.version, '0.2.1');
    assert.equal(requests[0].payload.operation, 'update');
    assert.match(view.id('settings-runtime-result').textContent, /Updated the engine and server to 0\.2\.1/);
    assert.equal(view.id('settings-update-runtime').hidden, true);
    assert.equal(view.id('settings-rollback-runtime').hidden, false);
});

test('runtime controls honor Play contention and explain a server-blocked rollback', async () => {
    const view = open({ 'get-show23': { ready: true, runtimeInUse: true, runtimeUpdateVersion: '0.2.1',
        runtimeRollbackBlocked: true } });
    await view.clock.advance(0);
    assert.equal(view.id('update-runtime').disabled, true);
    assert.equal(view.id('settings-update-runtime').disabled, true);
    assert.equal(view.id('settings-rollback-runtime').hidden, true);
    assert.match(view.id('settings-runtime-note').textContent, /newer server/i);
});

test('confirmed Repair reports success and restores Play only after verification', async () => {
    let repaired = false;
    const view = open({
        'get-show23': () => repaired ? { setUp: true, ready: true } :
            { setUp: true, runtimeRepairable: true, runtimeError: 'bad runtime file' },
        'repair-runtime': () => { repaired = true; return true; },
        'get-runtime-repair': { busy: false, success: true }
    });
    view.window.showMessageBox = async () => 1;
    await view.clock.advance(0);
    view.id('repair-runtime').click();
    await view.clock.advance(0);
    assert.ok(view.requests.some(item => item.command === 'repair-runtime'));
    assert.match(view.id('runtime-repair-result').textContent, /Repaired/);
    assert.equal(view.id('play').disabled, false);
    assert.equal(view.id('repair-runtime').hidden, true);
});

test('a ready update disables restart while a runtime session is active', async () => {
    const ready = { enabled: true, state: 'ready', readyVersion: '0.1.3-beta.2', channelChangeAllowed: false };
    const view = open({
        'get-show23': { setUp: true, ready: true, runtimeInUse: true },
        'get-launcher-update': ready,
        'check-launcher-update': ready
    });
    await view.clock.advance(0);
    assert.equal(view.id('launcher-update-action').disabled, true);
    assert.equal(view.id('channel-stable').disabled, true);
});

test('version retry queues only the failed slot the player confirms', async () => {
    const status = { enabled: true, canInstall: true, state: 'up-to-date',
        failedFolder: '0.1.3-beta.2~2', failedVersion: '0.1.3-beta.2' };
    const view = open({ 'get-settings': { language: 'en', offline: false },
        'get-launcher-update': status, 'check-launcher-update': status, 'retry-launcher-version': true });
    view.window.showMessageBox = async () => 1;
    await view.clock.advance(0);
    view.id('retry-launcher-version').click();
    await view.clock.advance(0);
    const request = view.requests.find(item => item.command === 'retry-launcher-version');
    assert.equal(request.payload.folder, '0.1.3-beta.2~2');
});

test('an older unversioned Stable release is a separate extraction link', async () => {
    const status = { enabled: true, canInstall: true, state: 'up-to-date', channel: 'stable',
        latest: '0.1.2', aheadOfStable: true, stableNeedsExtraction: true };
    const view = open({ 'get-settings': { language: 'en', channel: 'stable', offline: false },
        'get-launcher-update': status, 'check-launcher-update': status });
    await view.clock.advance(0);
    assert.equal(view.id('switch-stable').hidden, false);
    view.id('switch-stable').click();
    await view.clock.advance(0);
    assert.ok(view.requests.some(item => item.command === 'open-launcher-release'));
    assert.equal(view.requests.some(item => item.command === 'request-stable'), false);
});

test('Home keeps a fresh runtime read-only until Play', async () => {
    const view = open({ 'get-show23': { setUp: true, runtimeFresh: true },
        'play-show23': true, 'get-play': { phase: 'idle' } });
    await view.clock.advance(0);
    assert.match(view.id('game-source-notice').textContent, /when you press Play/);
    assert.equal(view.id('play').disabled, false);
    assert.equal(view.id('repair-runtime').hidden, true);
    assert.equal(view.requests.some(item => item.command === 'play-show23'), false);
    view.id('play').click();
    await view.clock.advance(0);
    assert.ok(view.requests.some(item => item.command === 'play-show23'));
});

test('Home explains a confirmed fallback when a version record could not be written', async () => {
    const view = open({ 'get-startup': { notices: 'fallback-write\n' } });
    await view.clock.advance(0);
    assert.match(view.id('startup-notice').textContent, /previous confirmed version/);
    assert.match(view.id('startup-notice').textContent, /waiting to save/);
});

test('Update banner explains a choice queued at the pointer deadline', async () => {
    const view = open();
    await view.clock.advance(0);
    const offer = view.window.LauncherUpdate.barFor({
        enabled: true, state: 'ready', readyVersion: '0.1.3', requestDeferred: true
    });
    assert.match(offer.text, /choice is queued for the next start/);
    assert.equal(offer.action, view.window.LauncherI18n.t('launcherUpdate.restartNow'));
    assert.equal(offer.failed, false);
});
