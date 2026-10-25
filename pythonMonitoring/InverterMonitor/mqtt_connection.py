"""TLS broker configuration and diagnostics for rejected WebSocket upgrades."""
import base64
from dataclasses import dataclass
import http.client
import os
import ssl


MQTT_USER_AGENT = 'InverterMonitor/1.0'


@dataclass(frozen=True)
class BrokerConfig:
    host: str = 'mqtt-esp.alemdarteknik.com'
    port: int = 443
    transport: str = 'websockets'
    path: str = '/mqtt'

    @classmethod
    def from_env(cls):
        transport = os.environ.get('INVERTER_MQTT_TRANSPORT', 'websockets').strip().lower()
        if transport not in ('websockets', 'tcp'):
            raise ValueError('INVERTER_MQTT_TRANSPORT must be websockets or tcp (both use TLS).')
        host = os.environ.get('INVERTER_MQTT_HOST', cls.host).strip()
        path = os.environ.get('INVERTER_MQTT_PATH', '/mqtt').strip()
        try:
            port = int(os.environ.get('INVERTER_MQTT_PORT', '443' if transport == 'websockets' else '8883'))
        except ValueError:
            raise ValueError('INVERTER_MQTT_PORT must be an integer.') from None
        if not host or any(c.isspace() or c in '/\\?#@' for c in host):
            raise ValueError('INVERTER_MQTT_HOST must be a hostname, without a URL scheme or path.')
        if not 1 <= port <= 65535:
            raise ValueError('INVERTER_MQTT_PORT must be between 1 and 65535.')
        if not path.startswith('/') or any(c in path for c in '\r\n'):
            raise ValueError('INVERTER_MQTT_PATH must start with /.')
        return cls(host, port, transport, path)

    @property
    def endpoint(self):
        if self.transport == 'websockets':
            return f'wss://{self.host}:{self.port}{self.path}'
        return f'mqtts://{self.host}:{self.port}'


def websocket_failure(config, original_error):
    """Probe only after an upgrade failure; never send MQTT or inverter commands."""
    prefix = f'Cannot open MQTT WebSocket at {config.endpoint}. '
    connection = http.client.HTTPSConnection(config.host, config.port, timeout=5,
                                             context=ssl.create_default_context())
    try:
        # Match Paho's Host/Origin headers and the application's User-Agent.
        # http.client.request() otherwise inserts a different Host and adds
        # Accept-Encoding, which can produce a different proxy decision.
        connection.putrequest('GET', config.path, skip_host=True, skip_accept_encoding=True)
        headers = {
            'Host': f'{config.host}:{config.port}', 'User-Agent': MQTT_USER_AGENT,
            'Upgrade': 'websocket', 'Connection': 'Upgrade',
            'Origin': f'https://{config.host}:{config.port}',
            'Sec-WebSocket-Version': '13', 'Sec-WebSocket-Protocol': 'mqtt',
            'Sec-WebSocket-Key': base64.b64encode(os.urandom(16)).decode('ascii')}
        for name, value in headers.items():
            connection.putheader(name, value)
        connection.endheaders()
        response = connection.getresponse()
        if response.getheader('cf-mitigated', '').lower() == 'challenge':
            return (prefix + f'Diagnostic request returned HTTP {response.status}: Cloudflare requires a browser challenge. '
                    'The MQTT client cannot complete it. Configure the Cloudflare rule for this '
                    'hostname/path to permit the MQTT handshake, or configure a dedicated TLS MQTT endpoint.')
        if response.status != 101:
            return (prefix + f'Diagnostic request returned HTTP {response.status}, rather than a WebSocket upgrade. '
                    'Check the broker listener, WebSocket path, and reverse-proxy rules.')
        return prefix + f'WebSocket negotiation failed: {original_error}'
    except (OSError, http.client.HTTPException, ValueError):
        return (prefix + f'{original_error}. The server/proxy did not complete the WebSocket upgrade. '
                'Check the broker listener, WebSocket path, and proxy security rules.')
    finally:
        connection.close()
