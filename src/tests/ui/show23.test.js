const assert = require('node:assert/strict');
const { page, test } = require('./harness');

function open(overrides = {}) {
    const requests = [];
    const setup = {
        'get-settings': { language: 'en', theme: 'default', offline: true, channel: 'stable' },
        'get-show23': { ready: false, folder: '' },
        'get-launcher-update': { enabled: false },
        'browse-folder': 'Q:\\Fixture\\Destination',
        'browse-file': 'Q:\\Fixture\\Source.pkg',
        'setup-show23': { pending: true },
        ...overrides
    };
    const view = page({
        scripts: ['core/i18n.js', 'core/focus-nav.js', 'app/mods-view.js', 'app/launcher-update.js', 'app/main.js'],
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
    assert.equal(id('setup-status').textContent, 'Setup is not implemented in this version.');
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
