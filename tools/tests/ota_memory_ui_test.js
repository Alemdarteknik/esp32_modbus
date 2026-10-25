'use strict';

// Offline, read-only checks of the renderer embedded in the actual setup page.
// Run: node tools/tests/ota_memory_ui_test.js
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname,
    '../../main/src/network/wifi_provisioning.cpp'), 'utf8');
const script = source.match(/<script>([\s\S]*?)<\/script>/);
assert.ok(script, 'Embedded setup-page script must exist');
new vm.Script(script[1]);
const start = script[1].indexOf('function renderFirmware(x){');
const end = script[1].indexOf('async function refreshStatus()', start);
assert.ok(start >= 0 && end > start, 'Actual firmware renderer must be extractable');

const nodes = new Map();
const context = vm.createContext({
    $: id => {
        assert.ok(source.includes('id="' + id + '"'), 'Missing DOM element: ' + id);
        if (!nodes.has(id)) {
            const element = { textContent: '', className: '' };
            Object.defineProperty(element, 'innerHTML', {
                set() { throw new Error('Firmware diagnostics must use textContent'); }
            });
            nodes.set(id, element);
        }
        return nodes.get(id);
    }
});
vm.runInContext(script[1].slice(start, end), context);
const render = data => context.renderFirmware(data);
const displayed = id => nodes.get(id).textContent;
const field = suffix => 'firmware_failure_' + suffix;
let passed = 0;
function test(name, run) {
    run();
    passed++;
    console.log('PASS: ' + name);
}
const snapshot = {
    firmware_update_state: 'deferred',
    firmware_update_reason: 'awaiting_scheduled_retry',
    firmware_internal_free_bytes: 131072,
    firmware_internal_largest_block_bytes: 65536,
    firmware_failure_stage: 'post_tls_memory',
    firmware_failure_error: 'ESP_ERR_NO_MEM',
    firmware_failure_code: 257,
    firmware_failure_http_status: 200,
    firmware_failure_internal_free_bytes: 40960,
    firmware_failure_internal_largest_block_bytes: 6144,
    firmware_failure_required_free_bytes: 32768,
    firmware_failure_required_largest_block_bytes: 8192
};

test('missing older status is unavailable', () => {
    render({});
    assert.equal(displayed('firmware-failure-memory'), 'Unavailable');
    assert.equal(displayed('firmware-error'), 'Unavailable');
    assert.equal(displayed('firmware-next-check'), 'Unavailable');
});

test('scheduled check shows countdown and due time', () => {
    render({firmware_next_check_in_ms: 90000});
    assert.equal(displayed('firmware-next-check'), 'in 1 min 30 s');
    render({firmware_next_check_in_ms: 0});
    assert.equal(displayed('firmware-next-check'), 'Due now');
});

test('invalid schedule values never imply an imminent check', () => {
    for (const value of [undefined, null, '0', -1, NaN, Infinity]) {
        render({firmware_next_check_in_ms: value});
        assert.equal(displayed('firmware-next-check'), 'Unavailable');
    }
});

test('older failure fields retain stage without inventing memory', () => {
    render({firmware_failure_stage: 'post_tls_memory', firmware_failure_code: 257});
    assert.match(displayed('firmware-error'), /post tls memory.*257/);
    assert.equal(displayed('firmware-failure-memory'),
        'Available: Unavailable free; largest block Unavailable | Required: Unavailable free; largest block Unavailable');
});

test('failure snapshot and active budget stay separate from current memory/blocker', () => {
    render(snapshot);
    assert.equal(displayed('firmware-failure-memory'),
        'Available: 40.0 KiB free; largest block 6.0 KiB | Required: 32.0 KiB free; largest block 8.0 KiB');
    assert.equal(displayed('firmware-memory'), '128.0 KiB free; largest block 64.0 KiB');
    assert.match(displayed('firmware-error'), /post tls memory.*ESP_ERR_NO_MEM.*257.*HTTP 200/);
    assert.match(displayed('firmware-reason'), /No current blocker detected/);
    assert.equal(displayed('firmware-state'), 'Check postponed');
});

test('nonmemory errors retain their error and memory context', () => {
    render({...snapshot, firmware_failure_stage: 'http_status',
        firmware_failure_error: 'ESP_FAIL', firmware_failure_code: -1,
        firmware_failure_http_status: 404,
        firmware_failure_internal_free_bytes: 65536,
        firmware_failure_internal_largest_block_bytes: 32768});
    assert.match(displayed('firmware-error'), /http status.*ESP_FAIL.*-1.*HTTP 404/);
    assert.match(displayed('firmware-failure-memory'), /Available: 64.0 KiB free/);
});

test('zero is a real recorded measurement', () => {
    render({...snapshot, firmware_failure_internal_free_bytes: 0,
        firmware_failure_internal_largest_block_bytes: 0});
    assert.match(displayed('firmware-failure-memory'),
        /^Available: 0.0 KiB free; largest block 0.0 KiB/);
});

test('invalid or missing values never become fabricated zero', () => {
    for (const suffix of ['internal_free_bytes', 'internal_largest_block_bytes',
        'required_free_bytes', 'required_largest_block_bytes']) {
        for (const value of [undefined, null, '8192', -1, NaN, Infinity, '<img src=x>']) {
            render({...snapshot, [field(suffix)]: value});
            const output = displayed('firmware-failure-memory');
            assert.ok(output.includes('Unavailable'), suffix + ': ' + String(value));
            assert.ok(!output.includes('<img'));
            assert.ok(!output.includes('0.0 KiB free; largest block 0.0 KiB'));
        }
    }
});

test('cleared stage means none recorded, even with zero memory fields', () => {
    render({...snapshot, firmware_failure_stage: '', firmware_failure_error: '',
        firmware_failure_code: 0, firmware_failure_http_status: 0,
        firmware_failure_internal_free_bytes: 0,
        firmware_failure_internal_largest_block_bytes: 0,
        firmware_failure_required_free_bytes: 0,
        firmware_failure_required_largest_block_bytes: 0});
    assert.equal(displayed('firmware-error'), 'None recorded');
    assert.equal(displayed('firmware-failure-memory'), 'None recorded');
});

test('untrusted diagnostic strings stay literal text', () => {
    render({...snapshot, firmware_failure_stage: '<img src=x onerror=alert(1)>',
        firmware_failure_error: '<script>alert(2)</script>'});
    assert.equal(displayed('firmware-error'),
        'Stage: <img src=x onerror=alert(1)> | <script>alert(2)</script> (code 257) | HTTP 200');
});

test('an unavailable refresh cannot leave a stale snapshot visible', () => {
    render(snapshot);
    render({});
    assert.equal(displayed('firmware-failure-memory'), 'Unavailable');
    assert.equal(displayed('firmware-memory'), 'Unavailable');
});

console.log(passed + '/' + passed + ' embedded-renderer checks passed; no device/server contacted.');
