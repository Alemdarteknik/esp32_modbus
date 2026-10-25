import os
import unittest
from unittest.mock import Mock, patch

import paho.mqtt.client as mqtt
from mqtt_backend import MonitorSession
from mqtt_connection import BrokerConfig, MQTT_USER_AGENT, websocket_failure


class BrokerConnectionTests(unittest.TestCase):
    def test_defaults_and_explicit_tls_transport(self):
        with patch.dict(os.environ, {}, clear=True):
            self.assertEqual(BrokerConfig.from_env().endpoint,
                             'wss://mqtt-esp.alemdarteknik.com:443/mqtt')
        with patch.dict(os.environ, {'INVERTER_MQTT_HOST': 'broker.example.com',
                                   'INVERTER_MQTT_TRANSPORT': 'tcp'}, clear=True):
            self.assertEqual(BrokerConfig.from_env().endpoint, 'mqtts://broker.example.com:8883')

    def test_cloudflare_diagnosis_and_socket_cleanup(self):
        with patch('mqtt_connection.http.client.HTTPSConnection') as factory:
            connection = factory.return_value
            response = connection.getresponse.return_value
            response.status = 403
            response.getheader.return_value = 'challenge'
            message = websocket_failure(BrokerConfig(), 'handshake failed')
        self.assertIn('HTTP 403', message)
        self.assertIn('Cloudflare', message)
        self.assertIn('browser challenge', message)
        self.assertIn('/mqtt', message)
        connection.close.assert_called_once()
        connection.putrequest.assert_called_once_with('GET', '/mqtt', skip_host=True, skip_accept_encoding=True)
        connection.putheader.assert_any_call('Host', 'mqtt-esp.alemdarteknik.com:443')
        connection.putheader.assert_any_call('User-Agent', MQTT_USER_AGENT)

    def test_http_404_is_not_reported_as_cloudflare(self):
        with patch('mqtt_connection.http.client.HTTPSConnection') as factory:
            response = factory.return_value.getresponse.return_value
            response.status = 404
            response.getheader.return_value = ''
            message = websocket_failure(BrokerConfig(), 'handshake failed')
        self.assertIn('HTTP 404', message)
        self.assertNotIn('Cloudflare', message)

    def test_failed_diagnostic_preserves_original_error(self):
        with patch('mqtt_connection.http.client.HTTPSConnection') as factory:
            factory.return_value.endheaders.side_effect = TimeoutError()
            message = websocket_failure(BrokerConfig(), 'original handshake error')
        self.assertIn('original handshake error', message)
        factory.return_value.close.assert_called_once()

    def test_failed_handshake_finishes_without_command_or_retry(self):
        session = MonitorSession()
        session.client = Mock()
        session.active = True
        session.client.connect.side_effect = mqtt.WebsocketConnectionError('connection not upgraded')
        session.client.disconnect.side_effect = OSError('already closed')
        with patch('mqtt_backend.websocket_failure', return_value='HTTP 403: Cloudflare browser challenge') as diagnostic:
            session.run()
        diagnostic.assert_called_once()
        self.assertFalse(session.active)
        self.assertFalse(session.ready)
        self.assertTrue(session.snapshot()['connection_error'])
        self.assertIn('HTTP 403', session.status)
        session.client.connect.assert_called_once()
        session.client.publish.assert_not_called()

    def test_configured_client_uses_tls_host_port_and_ws_path(self):
        session = MonitorSession()
        with patch.dict(os.environ, {'INVERTER_MQTT_HOST': 'broker.example.com',
                                    'INVERTER_MQTT_PORT': '9443',
                                    'INVERTER_MQTT_PATH': '/websocket'}, clear=True), \
                patch('mqtt_backend.mqtt.Client') as factory, patch('mqtt_backend.threading.Thread'):
            session.connect('ABCDEF123456')
        factory.return_value.ws_set_options.assert_called_once_with(
            path='/websocket', headers={'User-Agent': MQTT_USER_AGENT})
        factory.return_value.tls_set.assert_called_once()
        session.stop.set()
        session.run()
        factory.return_value.connect.assert_called_once_with('broker.example.com', 9443, keepalive=30)

    def test_invalid_configuration_fails_before_worker_start(self):
        for settings in ({'INVERTER_MQTT_HOST': 'https://broker.example.com'},
                         {'INVERTER_MQTT_TRANSPORT': 'http'}, {'INVERTER_MQTT_PORT': '0'},
                         {'INVERTER_MQTT_PATH': '/mqtt\r\nInjected: true'}):
            with self.subTest(settings=settings), patch.dict(os.environ, settings, clear=True), \
                    patch('mqtt_backend.threading.Thread') as worker:
                session = MonitorSession()
                with self.assertRaises(ValueError):
                    session.connect('ABCDEF123456')
                self.assertFalse(session.active)
                worker.assert_not_called()
