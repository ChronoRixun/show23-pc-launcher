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
        scripts: ['core/i18n.js', 'app/launcher-update.js', 'app/main.js'],
        backend: { handle(command, payload) {
            requests.push({ command, payload });
            return setup[command] ?? null;
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
    id('base-package').click();
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
    assert.match(view.id('part-status-launcher').textContent, /No release has been published yet/);
    assert.match(view.id('parts').textContent, /1\.2\.3-beta\.1Beta/);
    assert.match(view.id('parts').textContent, /Not installed/);
});
