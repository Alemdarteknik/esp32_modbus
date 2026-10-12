'use strict';

// Exercise the actual embedded setup page without contacting an ESP or broker.
// Run: node tools/tests/inverter_selection_ui_test.js
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname,
    '../../main/src/network/wifi_provisioning.cpp'), 'utf8');
const html = source.match(/R"HTML\(([\s\S]*?)\)HTML"/);
assert.ok(html, 'Embedded setup page must exist');
const script = html[1].match(/<script>([\s\S]*?)<\/script>/);
assert.ok(script, 'Embedded setup script must exist');
new vm.Script(script[1]);

function attributes(markup) {
    const result = {};
    for (const match of markup.matchAll(/([\w-]+)(?:="([^"]*)")?/g)) {
        result[match[1]] = match[2] === undefined ? true : match[2];
    }
    return result;
}

class Option {
    constructor(text, value) {
        this.textContent = String(text);
        this.value = String(value === undefined ? text : value);
        this.dataset = {};
        this.selected = false;
        this.disabled = false;
    }
}

class Element {
    constructor(tag, attrs) {
        this.tagName = tag.toUpperCase();
        this.id = attrs.id || '';
        this.name = attrs.name || '';
        this.type = attrs.type || 'text';
        this.checked = !!attrs.checked;
        this.disabled = !!attrs.disabled;
        this.required = !!attrs.required;
        this.textContent = '';
        this.className = attrs.class || '';
        this.dataset = {};
        this.options = [];
        this._value = String(attrs.value === undefined ? '' : attrs.value);
        this._innerHTML = '';
        const classes = new Set(this.className.split(/\s+/).filter(Boolean));
        this.classList = {
            toggle: (name, forced) => {
                const present = forced === undefined ? !classes.has(name) : forced;
                if (present) classes.add(name); else classes.delete(name);
                this.className = [...classes].join(' ');
                return present;
            },
            contains: name => classes.has(name)
        };
    }

    get value() { return this._value; }
    set value(value) {
        const text = String(value);
        this._value = this.tagName === 'SELECT' &&
            !this.options.some(option => option.value === text) ? '' : text;
    }

    get selectedOptions() {
        return this.options.filter(option => option.value === this.value);
    }

    add(option) {
        this.options.push(option);
        if (this.options.length === 1 || option.selected) this._value = option.value;
    }

    get innerHTML() { return this._innerHTML; }
    set innerHTML(markup) {
        this._innerHTML = markup;
        if (this.tagName !== 'SELECT') return;
        this.options = [];
        this._value = '';
        for (const match of markup.matchAll(/<option\b([^>]*)>([\s\S]*?)<\/option>/g)) {
            const attrs = attributes(match[1]);
            const text = match[2].replace(/<[^>]*>/g, '');
            const option = new Option(text, attrs.value === undefined ? text : attrs.value);
            option.selected = !!attrs.selected;
            option.disabled = !!attrs.disabled;
            this.add(option);
        }
    }

    focus() {}
    setAttribute(name, value) { this[name] = String(value); }
}

const legacyConfig = {
    device_role: 'standalone_combined',
    topology: 'standalone_single_phase',
    phase: 'none',
    expected_inverter_count: 1,
    expected_member_count: 0,
    logical_member_id: 0,
    modbus_slave_address: 1,
    espnow_channel: 0,
    site_id: 'test_installation',
    wifi_ssid: 'Saved Wi-Fi',
    wifi_password_set: true,
    mqtt_uri_set: true,
    mqtt_username: '',
    mqtt_password_set: false,
    mqtt_topic_prefix: 'inverter'
};

const readyStatus = {
    validation: 'valid',
    site_id: 'test_installation',
    station_connected: true,
    mqtt_connected: true,
    inverter_responding: true,
    coordinator_uart_connected: true,
    espnow_coordinator_found: true,
    members_found: 1,
    members_expected: 1
};

const settle = async () => {
    for (let i = 0; i < 4; ++i) await new Promise(resolve => setImmediate(resolve));
};

async function browser(config = legacyConfig) {
    const nodes = new Map();
    const fields = {};
    const formFields = [];
    for (const match of html[1].matchAll(/<(form|select|input|button|div|small|span|p|section)\b([^>]*)>/g)) {
        const attrs = attributes(match[2]);
        if (!attrs.id && !attrs.name) continue;
        const element = new Element(match[1], attrs);
        if (attrs.id) nodes.set(attrs.id, element);
        if (attrs.name) {
            fields[attrs.name] = element;
            formFields.push(element);
        }
        if (match[1] === 'select') {
            const close = html[1].indexOf('</select>', match.index);
            element.innerHTML = html[1].slice(match.index + match[0].length, close);
        }
    }
    nodes.get('f').elements = fields;
    const calls = [];
    let status = {...readyStatus};
    let configPost = async () => response({saved: true, restart: true});
    function response(data, ok = true) {
        return {ok, json: async () => data,
            text: async () => typeof data === 'string' ? data : JSON.stringify(data)};
    }
    const get = id => {
        assert.ok(nodes.has(id), 'Missing actual page element: ' + id);
        return nodes.get(id);
    };
    const context = vm.createContext({
        document: {
            querySelector: selector => get(selector.slice(1)),
            getElementById: get
        },
        Option,
        URL,
        AbortController,
        setTimeout: () => 1,
        clearTimeout: () => {},
        setInterval: () => 1,
        FormData: class {
            constructor(form) {
                assert.equal(form, nodes.get('f'));
                this.entries = formFields.filter(field => !field.disabled &&
                    (field.type !== 'checkbox' || field.checked));
            }
            forEach(fn) { this.entries.forEach(field => fn(field.value, field.name)); }
        },
        fetch: async (url, options = {}) => {
            calls.push({url, options});
            if (url === '/api/config') {
                return options.method === 'POST' ? configPost(options) : response(config);
            }
            if (url === '/api/status') {
                if (status instanceof Error) throw status;
                return response(status);
            }
            if (url === '/api/wifi/scan') return response({networks: []});
            if (url === '/api/finalize') return response({finished: true});
            throw new Error('Unexpected endpoint: ' + url);
        }
    });
    vm.runInContext(script[1], context);
    await settle();
    assert.doesNotMatch(get('message').textContent, /Could not load/,
        'Mock browser must complete the real configuration load');
    return {
        get,
        context,
        calls,
        posts: () => calls.filter(call => call.options.method === 'POST'),
        status: value => { status = value instanceof Error ? value : {...readyStatus, ...value}; },
        configPost: handler => { configPost = handler; },
        select: (id, value) => {
            const element = get(id);
            element.value = value;
            assert.equal(element.value, value, 'Option is present: ' + id + '=' + value);
            element.onchange({target: element});
        },
        input: (id, value) => {
            const element = get(id);
            element.value = value;
            if (element.oninput) element.oninput({target: element});
        },
        submit: () => get('f').onsubmit({preventDefault() {}}),
        finalize: () => get('finalize').onclick(),
        refresh: () => context.refreshStatus()
    };
}

let passed = 0;
async function test(name, run) {
    await run();
    ++passed;
    console.log('PASS: ' + name);
}

async function main() {
    await test('older saved setup retains the working AOHAI USB-RS485 selection', async () => {
        const page = await browser();
        assert.equal(page.get('inverter_protocol').value, 'AOHAI_MODBUS_RTU');
        assert.equal(page.get('inverter_connection').value, 'usb_rs485');
        assert.equal(page.get('save').disabled, false);
        assert.equal(page.get('finalize').disabled, false);
        assert.equal(page.get('modbus-box').classList.contains('hidden'), false);
    });

    await test('a hostname with the MQTT path selects secure WebSockets on port 443', async () => {
        const page = await browser();
        page.input('mqtt_uri', 'mqtt-esp.alemdarteknik.com/mqtt');
        assert.equal(page.get('mqtt_port').value, '443');
        assert.equal(page.get('mqtt_port').disabled, false);
        assert.match(page.get('mqtt-uri-hint').textContent, /secure WebSockets/);
        await page.submit();
        const payload = JSON.parse(page.posts().find(call => call.url === '/api/config').options.body);
        const endpoint = new URL(payload.mqtt_uri);
        assert.equal(endpoint.protocol, 'wss:');
        assert.equal(endpoint.hostname, 'mqtt-esp.alemdarteknik.com');
        assert.equal(endpoint.pathname, '/mqtt');
        assert.equal(endpoint.port || '443', '443');
        assert.equal(page.get('mqtt_port').value, '443');
    });

    await test('full server URIs keep their transport and port despite the old port field', async () => {
        for (const [address, protocol, expectedPort, expectedPath] of [
            ['wss://mqtt-esp.alemdarteknik.com/mqtt', 'wss:', '443', '/mqtt'],
            ['wss://mqtt-esp.alemdarteknik.com:443/mqtt', 'wss:', '443', '/mqtt'],
            ['ws://broker.example:8083/mqtt', 'ws:', '8083', '/mqtt'],
            ['mqtts://broker.example:8883', 'mqtts:', '8883', ''],
            ['mqtt://broker.example:1884', 'mqtt:', '1884', '']
        ]) {
            const page = await browser();
            assert.equal(page.get('mqtt_port').value, '1883');
            page.input('mqtt_uri', address);
            assert.equal(page.get('mqtt_port').value, expectedPort);
            assert.equal(page.get('mqtt_port').disabled, true);
            assert.equal(page.get('mqtt_port').required, false);
            assert.match(page.get('mqtt-port-hint').textContent, /Edit that address/);
            page.select('device_role', 'standalone_combined');
            assert.equal(page.get('mqtt_port').required, false, 'Role updates retain derived-port state');
            await page.submit();
            const payload = JSON.parse(page.posts().find(call => call.url === '/api/config').options.body);
            const endpoint = new URL(payload.mqtt_uri);
            assert.equal(endpoint.protocol, protocol);
            assert.equal(endpoint.port || (protocol === 'wss:' ? '443' : '80'), expectedPort);
            assert.equal(endpoint.pathname, expectedPath);
            if (address === 'wss://mqtt-esp.alemdarteknik.com/mqtt') {
                assert.equal(payload.mqtt_uri, address, 'The exact entered WSS URL stays unchanged');
            }
        }
    });

    await test('full URIs without ports use their transport standard rather than 1883', async () => {
        for (const [protocol, expectedPort] of [
            ['mqtt', '1883'], ['mqtts', '8883'], ['ws', '80'], ['wss', '443']
        ]) {
            const page = await browser();
            page.input('mqtt_uri', protocol + '://broker.example');
            assert.equal(page.get('mqtt_port').value, expectedPort);
            assert.equal(page.get('mqtt_port').disabled, true);
            await page.submit();
            const payload = JSON.parse(page.posts().find(call => call.url === '/api/config').options.body);
            const endpoint = new URL(payload.mqtt_uri);
            assert.equal(endpoint.protocol, protocol + ':');
            assert.equal(endpoint.port || expectedPort, expectedPort);
        }
    });

    await test('bare hosts retain an intentionally edited MQTT port', async () => {
        const page = await browser();
        page.input('mqtt_uri', 'broker.example');
        assert.equal(page.get('mqtt_port').value, '1883');
        assert.equal(page.get('mqtt_port').disabled, false);
        page.input('mqtt_port', '1884');
        page.input('mqtt_uri', 'replacement.example');
        assert.equal(page.get('mqtt_port').value, '1884');
        await page.submit();
        const payload = JSON.parse(page.posts().find(call => call.url === '/api/config').options.body);
        assert.equal(payload.mqtt_uri, 'mqtt://replacement.example:1884');
    });

    await test('default ports follow path inference while custom ports remain deliberate', async () => {
        const page = await browser();
        page.input('mqtt_uri', 'broker.example/mqtt');
        assert.equal(page.get('mqtt_port').value, '443');
        page.input('mqtt_uri', 'broker.example/');
        assert.equal(page.get('mqtt_port').value, '1883', 'A trailing slash is not a WebSocket path');
        page.input('mqtt_port', '8443');
        page.input('mqtt_uri', 'broker.example/mqtt');
        assert.equal(page.get('mqtt_port').value, '8443');
        await page.submit();
        const payload = JSON.parse(page.posts().find(call => call.url === '/api/config').options.body);
        assert.equal(payload.mqtt_uri, 'wss://broker.example:8443/mqtt');
    });

    await test('unsupported server schemes and invalid ports do not save', async () => {
        for (const address of ['http://broker.example/mqtt', 'https://broker.example/mqtt',
            'file://broker.example/mqtt', 'mqtt://broker.example:0',
            'wss://broker.example:65536/mqtt']) {
            const page = await browser();
            page.input('mqtt_uri', address);
            await page.submit();
            assert.equal(page.posts().length, 0, address);
            assert.match(page.get('message').textContent, /address|port|URL/, address);
        }
    });

    await test('a blank saved server keeps the saved endpoint even if its displayed port is invalid', async () => {
        const page = await browser();
        assert.equal(page.get('mqtt_uri').value, '');
        assert.match(page.get('mqtt-uri-hint').textContent, /Leave blank to keep/);
        assert.equal(page.get('mqtt_port').disabled, true);
        assert.equal(page.get('mqtt_port').required, false);
        assert.match(page.get('mqtt-port-hint').textContent, /Port saved/);
        page.input('mqtt_port', '0');
        await page.submit();
        const payload = JSON.parse(page.posts().find(call => call.url === '/api/config').options.body);
        assert.equal(payload.mqtt_uri, '');
        assert.equal(Object.hasOwn(payload, 'mqtt_port'), false);
    });

    await test('PI30 defaults to HID and submits the native selection', async () => {
        const page = await browser();
        page.select('inverter_protocol', 'PI30');
        assert.equal(page.get('inverter_connection').value, 'usb_hid');
        assert.equal(page.get('inverter_baud').value, '2400');
        assert.equal(page.get('save').disabled, false);
        assert.equal(page.get('modbus-box').classList.contains('hidden'), true);
        await page.refresh();
        assert.equal(page.get('finalize').disabled, true);
        await page.finalize();
        assert.equal(page.posts().length, 0);
        await page.submit();
        const payload = JSON.parse(page.posts().find(call => call.url === '/api/config').options.body);
        assert.equal(payload.inverter_protocol, 'PI30');
        assert.equal(payload.inverter_connection, 'usb_hid');
        assert.equal(payload.inverter_usb_vid, 0);
        assert.equal(payload.inverter_usb_pid, 0);
        assert.match(page.get('connection-hint').textContent, /detected automatically/);
        assert.equal(page.get('inverter_usb_vid').required, false);
        assert.equal(page.get('inverter_usb_pid').required, false);
    });

    await test('USB HID filters are optional Advanced fields and preserve explicit saved IDs', async () => {
        const advanced = html[1].slice(html[1].indexOf('<summary>Advanced settings</summary>'),
            html[1].indexOf('</details>'));
        assert.match(advanced, /id="hid-options-box"/);
        assert.doesNotMatch(html[1], /0 uses the Voltronic HID default/);
        const page = await browser({...legacyConfig,
            inverter_protocol: 'PI30', inverter_connection: 'usb_hid',
            inverter_usb_vid: 1637, inverter_usb_pid: 20833});
        assert.equal(page.get('inverter_usb_vid').value, '1637');
        assert.equal(page.get('inverter_usb_pid').value, '20833');
        await page.submit();
        let payload = JSON.parse(page.posts().find(call => call.url === '/api/config').options.body);
        assert.equal(payload.inverter_usb_vid, 1637);
        assert.equal(payload.inverter_usb_pid, 20833);
        page.get('inverter_usb_vid').value = '0';
        page.get('inverter_usb_pid').value = '0';
        page.get('inverter_usb_pid').oninput();
        await page.submit();
        payload = JSON.parse(page.posts().at(-1).options.body);
        assert.equal(payload.inverter_usb_vid, 0);
        assert.equal(payload.inverter_usb_pid, 0);
    });

    await test('a saved PI30 selection is restored with serial settings', async () => {
        const page = await browser({...legacyConfig,
            inverter_protocol: 'PI30', inverter_connection: 'uart_rs232', inverter_baud: 2400,
            inverter_tx_pin: 4, inverter_rx_pin: 5});
        assert.equal(page.get('inverter_protocol').value, 'PI30');
        assert.equal(page.get('inverter_connection').value, 'uart_rs232');
        assert.equal(page.get('inverter_baud').value, '2400');
        assert.equal(page.get('save').disabled, false);
        await page.submit();
        const payload = JSON.parse(page.posts().find(call => call.url === '/api/config').options.body);
        assert.equal(payload.inverter_baud, 2400);
        assert.equal(payload.inverter_tx_pin, 4);
    });

    await test('JK Bluetooth requires a peer address and excludes USB HID', async () => {
        const page = await browser();
        page.select('inverter_protocol', 'JK02_32');
        assert.equal(page.get('inverter_connection').value, 'bluetooth');
        assert.equal(page.get('save').disabled, true);
        await page.submit();
        assert.equal(page.posts().length, 0);
        page.get('inverter_bluetooth_address').value = 'AA:BB:CC:DD:EE:FF';
        page.get('inverter_bluetooth_address').oninput();
        assert.equal(page.get('save').disabled, false);
        page.select('inverter_connection', 'usb_hid');
        assert.equal(page.get('save').disabled, true);
        await page.submit();
        assert.equal(page.posts().length, 0);
        page.select('inverter_connection', 'bluetooth');
        await page.submit();
        const payload = JSON.parse(page.posts()[0].options.body);
        assert.equal(payload.inverter_bluetooth_address, 'AA:BB:CC:DD:EE:FF');
        assert.equal(payload.inverter_bluetooth_address_type, 0);
    });

    await test('VE.Direct requires TTL UART and selects 19200 baud', async () => {
        const page = await browser();
        page.select('inverter_protocol', 'VED');
        assert.equal(page.get('inverter_connection').value, 'uart_ttl');
        assert.equal(page.get('inverter_baud').value, '19200');
        assert.equal(page.get('uart-pins-box').classList.contains('hidden'), false);
        page.select('inverter_connection', 'usb_hid');
        await page.submit();
        assert.equal(page.posts().length, 0);
    });

    await test('unknown or malformed saved selections cannot become the active pair', async () => {
        for (const protocol of ['NEW_PROTOCOL', null, 123, false]) {
            const page = await browser({...legacyConfig, inverter_protocol: protocol});
            assert.notEqual(page.get('inverter_protocol').value, 'AOHAI_MODBUS_RTU');
            assert.equal(page.get('save').disabled, true);
            await page.submit();
            assert.equal(page.posts().length, 0);
        }
    });

    await test('an incompatible HID connection blocks the existing AOHAI protocol', async () => {
        const page = await browser();
        page.select('inverter_connection', 'usb_hid');
        await page.refresh();
        await page.submit();
        await page.finalize();
        assert.equal(page.get('save').disabled, true);
        assert.equal(page.get('finalize').disabled, true);
        assert.equal(page.posts().length, 0);
    });

    await test('returning to AOHAI restores buttons according to live connection readiness', async () => {
        const page = await browser();
        page.select('inverter_protocol', 'PI30');
        page.status({inverter_responding: false});
        await page.refresh();
        page.select('inverter_protocol', 'AOHAI_MODBUS_RTU');
        assert.equal(page.get('save').disabled, false);
        assert.equal(page.get('finalize').disabled, true);
        assert.equal(page.get('modbus-box').classList.contains('hidden'), false);
        page.status({inverter_responding: true});
        await page.refresh();
        assert.equal(page.get('finalize').disabled, false);
    });

    await test('a failed status refresh removes the previously ready finalize state', async () => {
        const page = await browser();
        assert.equal(page.get('finalize').disabled, false);
        page.status(new Error('Connection lost'));
        await page.refresh();
        assert.equal(page.get('finalize').disabled, true);
        page.select('inverter_protocol', 'PI30');
        page.select('inverter_protocol', 'AOHAI_MODBUS_RTU');
        assert.equal(page.get('finalize').disabled, true);
        assert.equal(page.get('save').disabled, false);
    });

    await test('supported save submits the explicit selection with existing networking fields', async () => {
        const page = await browser();
        await page.submit();
        const post = page.posts().find(call => call.url === '/api/config');
        assert.ok(post);
        const payload = JSON.parse(post.options.body);
        assert.equal(payload.inverter_protocol, 'AOHAI_MODBUS_RTU');
        assert.equal(payload.inverter_connection, 'usb_rs485');
        assert.equal(payload.wifi_ssid, legacyConfig.wifi_ssid);
        assert.equal(payload.mqtt_topic_prefix, legacyConfig.mqtt_topic_prefix);
        assert.equal(payload.modbus_slave_address, 1);
        assert.equal(payload.clear_wifi_password, false);
    });

    await test('in-flight save stays disabled when selection handlers run', async () => {
        const page = await browser();
        let finish;
        page.configPost(() => new Promise(resolve => { finish = resolve; }));
        const saving = page.submit();
        await settle();
        assert.equal(page.get('save').disabled, true);
        page.select('inverter_protocol', 'PI30');
        page.select('inverter_protocol', 'AOHAI_MODBUS_RTU');
        assert.equal(page.get('save').disabled, true);
        finish({ok: true, text: async () => '{"saved":true}'});
        await saving;
    });

    await test('gateway role keeps networking usable without a local inverter choice', async () => {
        const page = await browser();
        page.select('inverter_protocol', 'PI30');
        page.select('inverter_connection', 'usb_hid');
        page.select('topology', 'parallel_single_phase');
        page.select('device_role', 'internet_gateway');
        await page.refresh();
        assert.equal(page.get('inverter-card').classList.contains('hidden'), true);
        assert.equal(page.get('inverter_protocol').disabled, true);
        assert.equal(page.get('inverter_connection').disabled, true);
        assert.equal(page.get('save').disabled, false);
        assert.equal(page.get('finalize').disabled, false);
        await page.submit();
        const post = page.posts().find(call => call.url === '/api/config');
        assert.ok(post, 'Gateway networking configuration can still be saved');
        const payload = JSON.parse(post.options.body);
        assert.equal(payload.device_role, 'internet_gateway');
        assert.equal(payload.wifi_ssid, legacyConfig.wifi_ssid);
        assert.equal(payload.inverter_protocol, undefined);
        assert.equal(payload.inverter_connection, undefined);
    });

    console.log(passed + '/' + passed + ' embedded-page behavior checks passed; no device/server contacted.');
}

main().catch(error => {
    console.error(error);
    process.exitCode = 1;
});
