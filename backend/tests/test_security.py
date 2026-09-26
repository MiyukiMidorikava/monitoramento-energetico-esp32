"""Offline regression tests: no local credentials or database are used."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import unittest
from unittest.mock import patch

with patch("dotenv.load_dotenv"):
    spec = importlib.util.spec_from_file_location(
        "backend_app", Path(__file__).resolve().parents[1] / "main-flask.py"
    )
    backend = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(backend)


class SecurityTests(unittest.TestCase):
    def test_firmware_authorization_requires_token(self):
        for token, expected in ((None, 401), ('wrong', 401), ('test-api-token', 204)):
            with self.subTest(token=token):
                client = backend.app.test_client()
                headers = {} if token is None else {'X-API-Key': token}
                self.assertEqual(client.get('/firmware-auth', headers=headers).status_code, expected)
        self.post.assert_not_called()

    def test_firmware_authorization_fails_closed_without_secret(self):
        with patch.dict(backend.app.config, {'API_SECURITY_TOKEN': ''}):
            self.assertEqual(self.client.get('/firmware-auth').status_code, 401)

    def test_proxy_ip_requires_explicit_opt_in(self):
        for enabled, expected in (('0', '127.0.0.1'), ('1', '203.0.113.9')):
            with self.subTest(enabled=enabled), patch('dotenv.load_dotenv'), patch.dict(
                os.environ, {'TRUST_CADDY_PROXY': enabled}
            ):
                module_spec = importlib.util.spec_from_file_location('proxy_test', Path(backend.__file__))
                module = importlib.util.module_from_spec(module_spec)
                module_spec.loader.exec_module(module)
                module.app.add_url_rule('/test-address', view_func=lambda: module.request.remote_addr)
                response = module.app.test_client().get('/test-address', headers={
                    'X-Forwarded-For': '198.51.100.99, 203.0.113.9',
                    'X-Forwarded-Host': 'untrusted.example',
                })
                self.assertEqual(response.get_data(as_text=True), expected)

    def setUp(self):
        backend.app.config.update(TESTING=True)
        backend.limiter.enabled = False
        backend.ULTIMAS_TELEMETRIAS.clear()
        self.client = backend.app.test_client()
        self.token_config = patch.dict(backend.app.config, {"API_SECURITY_TOKEN": "test-api-token"})
        self.token_config.start()
        self.addCleanup(self.token_config.stop)
        self.client.environ_base['HTTP_X_API_KEY'] = 'test-api-token'
        self.payload = {"device": "esp32-energia", "uptime_ms": 1000,
                        "status": {}, "dados": {"tensao_rms_v": 127.0, "corrente_rms_a": 2.0,
                        "potencia_ativa_w": 127.0, "fator_potencia": 0.5, "energia_kwh": 0.01,
                        "potencia_va": 160.0, "potencia_reativa_var": 96.0,
                        "temperatura_c": 42.5, "temperatura_ambiente_c": 24.3, "umidade_pct": 61.5}}
        self.network = patch.object(backend.requests, "post")
        self.post = self.network.start()
        self.addCleanup(self.network.stop)
        self.post.return_value.ok = True
        self.post.return_value.status_code = 204
        self.env = patch.dict(os.environ, {"INFLUX_TOKEN": "test-only"})
        self.env.start()
        self.addCleanup(self.env.stop)

    def test_extended_diagnostics_reach_influx(self):
        self.payload['firmware_sha256'] = 'ab' * 32
        self.payload['status'].update(reset_reason=6, dht_valido=1,
                                      corrente_pre_correcao_a=1.202)
        line = backend._build_line_protocol(self.payload)
        for value in ('firmware_sha256=' + 'ab' * 32, 'reset_reason=6i',
                      'dht_valido=1i', 'corrente_pre_correcao_a=1.202',
                      'temperatura_ambiente_c=24.3', 'umidade_pct=61.5'):
            self.assertIn(value, line)

    def test_invalid_dht_is_not_stored_as_environmental_measurement(self):
        self.payload['status']['dht_valido'] = 0
        line = backend._build_line_protocol(self.payload)
        self.assertNotIn('temperatura_ambiente_c=', line)
        self.assertNotIn('umidade_pct=', line)
        self.assertIn('temperatura_c=42.5', line)
        self.assertIn('potencia_w=127.0', line)
        self.assertIn('dht_valido=0i', line)

    def test_extended_diagnostics_reject_invalid_values(self):
        for key, values in {
            'reset_reason': [True, -1, 256, 1.5, '6'],
            'dht_valido': [True, -1, 2, 1.0, '1'],
            'corrente_pre_correcao_a': [True, -0.1, float('nan'), float('inf'), '1'],
        }.items():
            for value in values:
                with self.subTest(key=key, value=value):
                    self.payload['status'] = {key: value}
                    self.assertTrue(backend.validar_telemetria(self.payload))

    def test_firmware_identity_requires_sha256_hex(self):
        for value in ('', 'a' * 63, 'a' * 65, 'z' * 64, 'a' * 63 + '\n', 123, None):
            with self.subTest(value=value):
                self.payload['firmware_sha256'] = value
                self.assertTrue(backend.validar_telemetria(self.payload))

    def test_unauthorized_requests_do_not_read_body_or_write_database(self):
        self.client.environ_base.clear()
        cases = [{}, {'X-API-Key': ''}, {'X-API-Key': '   '}, {'Authorization': ''},
                 {'Authorization': 'Bearer   '}, {'X-API-Key': 'wrong'},
                 {'Authorization': 'Bearer wrong'}, {'Authorization': 'Bearer'},
                 {'Authorization': 'Basic test-api-token'},
                 {'X-API-Key': 'inválido'}]
        with patch.object(backend.app.request_class, 'get_json',
                          side_effect=AssertionError('Unauthorized body was read')):
            for headers in cases:
                with self.subTest(headers=headers):
                    response = self.client.post('/telemetria', headers=headers,
                                                data='x' * 20000, content_type='application/json')
                    self.assertEqual(response.status_code, 401)
                    self.assertEqual(response.json, {'error': 'Unauthorized'})
        self.post.assert_not_called()
        self.assertEqual(backend.ULTIMAS_TELEMETRIAS, [])

    def test_either_valid_header_authorizes_telemetry(self):
        self.client.environ_base.clear()
        for headers in ({'X-API-Key': 'test-api-token'},
                        {'Authorization': 'Bearer test-api-token'},
                        {'Authorization': 'bearer test-api-token'},
                        {'X-API-Key': 'wrong', 'Authorization': 'Bearer test-api-token'}):
            with self.subTest(headers=headers):
                response = self.client.post('/telemetria', headers=headers, json=self.payload)
                self.assertEqual(response.status_code, 200)
        self.assertEqual(self.post.call_count, 4)

    def test_unconfigured_token_rejects_all_requests(self):
        with patch.dict(backend.app.config, {'API_SECURITY_TOKEN': ''}):
            for token in ('', 'test-api-token'):
                response = self.client.post('/telemetria', headers={'X-API-Key': token}, json=self.payload)
                self.assertEqual(response.status_code, 401)
                self.assertEqual(response.json, {'error': 'Unauthorized'})
        self.post.assert_not_called()

    def test_exact_measurement_keys_required(self):
        valid = self.payload['dados']
        cases = [{}]
        cases += [{k: v for k, v in valid.items() if k != missing} for missing in valid]
        cases += [valid | {extra: 1} for extra in
                  ('potencia_w', 'temperatura', 'umidade', 'foo')]
        cases.append(valid | {'potencia_w': 127})
        legacy = dict(valid)
        legacy['potencia_w'] = legacy.pop('potencia_ativa_w')
        cases.append(legacy)
        for dados in cases:
            with self.subTest(dados=dados):
                self.assertEqual(self.client.post('/telemetria', json=self.payload | {'dados': dados}).status_code, 400)
        self.post.assert_not_called()

    def test_integer_uptime_preserves_precision_and_range(self):
        for uptime in (0, 2**32 + 1, 2**53 + 1, 2**63 - 1):
            with self.subTest(uptime=uptime):
                self.assertEqual(self.client.post('/telemetria', json=self.payload | {'uptime_ms': uptime}).status_code, 200)
                self.assertIn(f'uptime_ms={uptime}i', self.post.call_args.kwargs['data'].decode())

    def test_all_electrical_fields_reach_influx(self):
        for scale in (1, 0):
            with self.subTest(scale=scale):
                self.payload['dados'] = {key: value * scale for key, value in {
                    'tensao_rms_v': 127.0, 'corrente_rms_a': 2.0,
                    'potencia_ativa_w': 203.2, 'fator_potencia': 0.8,
                    'energia_kwh': 0.000123456789,
                    'potencia_va': 254.0, 'potencia_reativa_var': 152.4,
                    'temperatura_c': 42.5, 'temperatura_ambiente_c': 24.3, 'umidade_pct': 61.5,
                }.items()}
                response = self.client.post('/telemetria', json=self.payload)
                self.assertEqual(response.status_code, 200)
                line = self.post.call_args.kwargs['data'].decode()
                fields = dict(item.split('=') for item in line.split(' ')[1].split(','))
                self.assertEqual(fields.pop('uptime_ms'), '1000i')
                expected = dict(self.payload['dados'])
                expected['potencia_w'] = expected.pop('potencia_ativa_w')
                self.assertEqual({k: float(v) for k, v in fields.items()}, expected)

    def test_status_diagnostic_fields_reach_influx(self):
        self.payload['status'] = {'wifi_rssi': -57, 'heap_livre_bytes': 123456, 'stack_min_bytes': 4096}
        response = self.client.post('/telemetria', json=self.payload)
        self.assertEqual(response.status_code, 200)
        line = self.post.call_args.kwargs['data'].decode()
        fields = dict(item.split('=') for item in line.split(' ')[1].split(','))
        self.assertEqual(fields['wifi_rssi'], '-57i')
        self.assertEqual(fields['heap_livre_bytes'], '123456i')
        self.assertEqual(fields['stack_min_bytes'], '4096i')

    def test_status_diagnostic_fields_are_optional(self):
        # self.payload already carries status: {} -- older firmware or a
        # boot with no Wi-Fi AP yet must not be rejected for omitting these.
        response = self.client.post('/telemetria', json=self.payload)
        self.assertEqual(response.status_code, 200)
        line = self.post.call_args.kwargs['data'].decode()
        for key in ('wifi_rssi', 'heap_livre_bytes', 'stack_min_bytes'):
            self.assertNotIn(f'{key}=', line)
        # Legacy payloads still persist environmental values without the new
        # validity flag, and remain available in authenticated recent history.
        self.assertIn('temperatura_ambiente_c=24.3', line)
        self.assertIn('umidade_pct=61.5', line)
        history = self.client.get('/telemetria/ultimas')
        self.assertEqual(history.status_code, 200)
        self.assertEqual(history.get_json()['itens'][-1]['payload'], self.payload)

    def test_status_diagnostic_fields_reject_non_integers(self):
        for key in ('wifi_rssi', 'heap_livre_bytes', 'stack_min_bytes'):
            for value in (1.5, '1', True, False, None, float('nan'), float('inf')):
                with self.subTest(key=key, value=value):
                    response = self.client.post('/telemetria', json=self.payload | {'status': {key: value}})
                    self.assertEqual(response.status_code, 400)
        self.post.assert_not_called()

    def test_status_integers_respect_influx_signed_64_bit_range(self):
        for key in backend.STATUS_FIELDS:
            for value in (-(2**63), 2**63 - 1):
                with self.subTest(key=key, value=value):
                    response = self.client.post('/telemetria', json=self.payload | {'status': {key: value}})
                    self.assertEqual(response.status_code, 200)
                    self.assertIn(f'{key}={value}i', self.post.call_args.kwargs['data'].decode())
            self.post.reset_mock()
            for value in (-(2**63) - 1, 2**63, 10**100):
                with self.subTest(key=key, value=value):
                    response = self.client.post('/telemetria', json=self.payload | {'status': {key: value}})
                    self.assertEqual(response.status_code, 400)
            self.post.assert_not_called()

    def test_invalid_inputs_never_reach_database(self):
        cases = [{tag: value} for tag in ('device', 'ts_source', 'boot_sync')
                 for value in ('x\ny', 'x\ry', '', {'x': 1})]
        cases += [{'uptime_ms': x} for x in (True, False, -1, 1.0, 1.5, '1000', None, float('nan'), float('inf'), 2**63)]
        cases += [{'timestamp': x} for x in (123, 'bad', '9999-01-01T00:00:00')]
        cases += [{'dados': self.payload['dados'] | {key: x}}
                  for key in self.payload['dados']
                  for x in (True, '12', None, float('nan'), float('inf'), -float('inf'), 10**400)]
        cases += [{'dados': {'potencia_w': 1, 'potencia_ativa_w': 2}}]
        for change in cases:
            with self.subTest(change=change):
                response = self.client.post('/telemetria', json=self.payload | change)
                self.assertEqual(response.status_code, 400)
        self.post.assert_not_called()

    def test_tag_escape_rejects_newlines_defensively(self):
        for value in ('x\ny', 'x\ry'):
            with self.assertRaises(ValueError):
                backend._tag_escape(value)

    def test_valid_tags_timestamp_and_power_mapping(self):
        self.payload.update(device="device a,b=c", timestamp="2026-09-08T12:00:00")
        response = self.client.post('/telemetria', json=self.payload)
        self.assertEqual(response.status_code, 200)
        line = self.post.call_args.kwargs['data'].decode()
        self.assertIn(r'device=device\ a\,b\=c', line)
        self.assertEqual(line.count('potencia_w='), 1)
        self.assertTrue(line.endswith('1788868800000000000'))
        self.assertFalse(self.post.call_args.kwargs['allow_redirects'])

    def test_redirect_is_not_a_success(self):
        self.post.return_value.status_code = 307
        self.assertEqual(self.client.post('/telemetria', json=self.payload).status_code, 503)

    def test_malformed_structure_is_rejected(self):
        for payload in ([], {}, self.payload | {'dados': []}, self.payload | {'status': []}):
            with self.subTest(payload=payload):
                self.assertEqual(self.client.post('/telemetria', json=payload).status_code, 400)
        self.post.assert_not_called()

    def test_body_limit_returns_json(self):
        response = self.client.post('/telemetria', json=self.payload | {'extra': 'x'*(16 * 1024)})
        self.assertEqual(response.status_code, 413)
        self.assertFalse(response.json['ok'])
        self.post.assert_not_called()

    def test_payload_boundary(self):
        import json
        body = json.dumps(self.payload)
        for size, expected in ((16384, 200), (16385, 413)):
            response = self.client.post('/telemetria', data=body.ljust(size),
                                        content_type='application/json')
            self.assertEqual(response.status_code, expected)

    def test_quotes_in_all_tags_are_rejected(self):
        for tag in ('device', 'ts_source', 'boot_sync'):
            response = self.client.post('/telemetria', json=self.payload | {tag: 'a"b'})
            self.assertEqual(response.status_code, 400)
        self.post.assert_not_called()

    def test_blank_config_and_blank_headers_fail_closed(self):
        self.client.environ_base.clear()
        with patch.dict(backend.app.config, {'API_SECURITY_TOKEN': '   '}):
            response = self.client.post('/telemetria', headers={'X-API-Key': '   '}, json=self.payload)
            self.assertEqual(response.status_code, 401)

    def test_history_requires_authentication(self):
        self.client.environ_base.clear()
        self.assertEqual(self.client.get('/telemetria/ultimas').status_code, 401)
        self.assertEqual(self.client.get('/telemetria/ultimas',
                         headers={'Authorization': 'Bearer test-api-token'}).status_code, 200)

    def test_rate_limit_returns_json_429(self):
        backend.limiter.enabled = True
        backend.limiter.reset()
        responses = [self.client.post('/telemetria', json=self.payload) for _ in range(6)]
        self.assertIn(429, [r.status_code for r in responses])
        limited = next(r for r in responses if r.status_code == 429)
        self.assertFalse(limited.json['ok'])

    def test_database_failures_are_503_without_internal_details(self):
        for failure in ('http', 'timeout', 'missing_token'):
            with self.subTest(failure=failure):
                self.post.side_effect = None
                self.post.return_value.ok = False
                self.post.return_value.status_code = 500
                self.post.return_value.text = 'PRIVATE_INTERNAL_DETAIL'
                self.post.return_value.is_redirect = False
                if failure == 'timeout':
                    self.post.side_effect = backend.requests.Timeout('PRIVATE_INTERNAL_DETAIL')
                with patch.dict(os.environ, {'INFLUX_TOKEN': '' if failure == 'missing_token' else 'test-only'}):
                    response = self.client.post('/telemetria', json=self.payload)
                self.assertEqual(response.status_code, 503)
                self.assertFalse(response.json['ok'])
                self.assertNotIn('PRIVATE_INTERNAL_DETAIL', response.get_data(as_text=True))

    def test_no_raw_payload_stdout(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.client.post('/telemetria', json=self.payload)
        self.assertEqual(output.getvalue(), '')


if __name__ == '__main__':
    unittest.main()
