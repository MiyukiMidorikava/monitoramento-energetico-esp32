import math
import re
import os
import hmac
import time
from datetime import datetime, timezone
from typing import Any

from dotenv import load_dotenv

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
load_dotenv(os.path.join(BASE_DIR, ".env"))

import requests
from flask import Flask, jsonify, request
from flask_limiter import Limiter
from flask_limiter.util import get_remote_address
from werkzeug.middleware.proxy_fix import ProxyFix


app = Flask(__name__)
# Enable only with one trusted Caddy hop and no public backend port.
if os.environ.get("TRUST_CADDY_PROXY") == "1":
    app.wsgi_app = ProxyFix(app.wsgi_app, x_for=1, x_proto=0, x_host=0, x_port=0, x_prefix=0)
app.config["MAX_CONTENT_LENGTH"] = 16 * 1024
# Fail closed: without a configured secret no telemetry is accepted.
app.config["API_SECURITY_TOKEN"] = os.environ.get("API_SECURITY_TOKEN", "")

# In-memory storage: shared by the current single Gunicorn worker; if this
# ever moves to multiple gunicorn workers, limits stop being shared across
# them unless the storage_uri points at something external (e.g. Redis).
limiter = Limiter(get_remote_address, app=app, storage_uri="memory://")

ULTIMAS_TELEMETRIAS: list[dict[str, Any]] = []
MAX_TELEMETRIAS = 20

INFLUX_URL = os.environ.get("INFLUX_URL", "http://127.0.0.1:8181")
INFLUX_DB  = os.environ.get("INFLUX_DB",  "monitoramento_energetico")


def _get_influx_token() -> str:
    token = os.environ.get("INFLUX_TOKEN", "")
    if not token:
        raise RuntimeError("variável de ambiente INFLUX_TOKEN não definida")
    return token


MEASUREMENT_FIELDS = (
    "tensao_rms_v", "corrente_rms_a", "potencia_ativa_w",
    "fator_potencia", "energia_kwh",
    "potencia_va", "potencia_reativa_var",
    "temperatura_c", "temperatura_ambiente_c", "umidade_pct",
)

# Diagnostic fields living in payload['status'], not payload['dados']: unlike
# MEASUREMENT_FIELDS these are optional (older firmware, or a boot with no
# Wi-Fi AP yet, may omit wifi_rssi) so they are validated/written only when
# present, never required for the exact-match check on 'dados'.
STATUS_FIELDS = ("wifi_rssi", "heap_livre_bytes", "stack_min_bytes")


def _tag_escape(value: str) -> str:
    """Reject controls before escaping line protocol tag separators."""
    if (not isinstance(value, str) or not value.strip() or len(value) > 64
            or '"' in value
            or any(ord(char) < 32 or ord(char) == 127 for char in value)):
        raise ValueError("tag deve ser texto de 1 a 64 caracteres sem controles")
    return value.replace("\\", "\\\\").replace(" ", "\\ ").replace(",", "\\,").replace("=", "\\=")


def _finite_number(value: Any) -> bool:
    if type(value) not in (int, float):
        return False
    try:
        return math.isfinite(value)
    except (ValueError, OverflowError):
        return False


def _influx_int(value: Any) -> bool:
    return type(value) is int and -(2**63) <= value <= 2**63 - 1


def _timestamp_ns(value: Any) -> int:
    if not isinstance(value, str):
        raise ValueError("timestamp deve ser texto")
    dt = datetime.strptime(value, "%Y-%m-%dT%H:%M:%S").replace(tzinfo=timezone.utc)
    delta = dt - datetime(1970, 1, 1, tzinfo=timezone.utc)
    timestamp = (delta.days * 86400 + delta.seconds) * 1_000_000_000
    if not -(2**63) + 2 <= timestamp <= 2**63 - 2:
        raise ValueError("timestamp fora da faixa do InfluxDB")
    return timestamp


def _build_line_protocol(payload: dict[str, Any]) -> str:
    """Convert canonical telemetry while preserving the historical power column."""
    errors = validar_telemetria(payload)
    if errors:
        raise ValueError("; ".join(errors))
    tags = ",".join(
        f"{key}={_tag_escape(payload.get(key, 'UNKNOWN'))}"
        for key in ("device", "ts_source", "boot_sync")
    )
    dados = payload["dados"]
    status = payload["status"]
    if "firmware_sha256" in payload:
        tags += f",firmware_sha256={payload['firmware_sha256'].lower()}"
    fields = [f"uptime_ms={payload['uptime_ms']}i"]
    for key in MEASUREMENT_FIELDS:
        # Preserve the legacy input contract, but never persist known fallback
        # values as DHT observations. Missing validity means legacy/unknown.
        if status.get("dht_valido") == 0 and key in ("temperatura_ambiente_c", "umidade_pct"):
            continue
        storage_key = "potencia_w" if key == "potencia_ativa_w" else key
        fields.append(f"{storage_key}={float(dados[key])}")
    status = payload.get("status")
    if isinstance(status, dict):
        for key in (*STATUS_FIELDS, "reset_reason", "dht_valido"):
            if key in status:
                fields.append(f"{key}={int(status[key])}i")
        if "corrente_pre_correcao_a" in status:
            fields.append(f"corrente_pre_correcao_a={float(status['corrente_pre_correcao_a'])}")
    timestamp = _timestamp_ns(payload["timestamp"]) if "timestamp" in payload else time.time_ns()
    return f"telemetria,{tags} {','.join(fields)} {timestamp}"


def _escrever_influx(line: str) -> tuple[bool, str]:
    try:
        token = _get_influx_token()
    except RuntimeError:
        return False, "credencial do banco ausente"

    headers = {
        "Authorization": f"Bearer {token}",
        "Content-Type": "text/plain; charset=utf-8",
    }
    params = {"db": INFLUX_DB, "precision": "nanosecond"}

    try:
        resp = requests.post(
            f"{INFLUX_URL}/api/v3/write_lp",
            headers=headers,
            params=params,
            data=line.encode("utf-8"),
            timeout=5,
            allow_redirects=False,
        )
        if 200 <= resp.status_code < 300:
            return True, ""
        return False, "banco recusou a escrita"
    except requests.RequestException:
        return False, "banco indisponivel"


def validar_telemetria(payload: dict[str, Any]) -> list[str]:
    errors = []
    for field in ("device", "uptime_ms", "status", "dados"):
        if field not in payload:
            errors.append(f"campo obrigatorio ausente: {field}")
    for field in ("device", "ts_source", "boot_sync"):
        if field in payload:
            try:
                _tag_escape(payload[field])
            except ValueError:
                errors.append(f"tag invalida: {field}")
    uptime = payload.get("uptime_ms")
    if "firmware_sha256" in payload:
        identity = payload["firmware_sha256"]
        if not isinstance(identity, str) or re.fullmatch(r"[0-9a-fA-F]{64}", identity) is None:
            errors.append("firmware_sha256 deve conter 64 digitos hexadecimais")
    if type(uptime) is not int or not 0 <= uptime <= 2**63 - 1:
        errors.append("uptime_ms deve ser inteiro nao negativo de 64 bits")
    if "status" in payload and not isinstance(payload["status"], dict):
        errors.append("status deve ser um objeto JSON")
    elif isinstance(payload.get("status"), dict):
        status = payload["status"]
        for key in STATUS_FIELDS:
            if key in status and not _influx_int(status[key]):
                errors.append(f"status invalido: {key} deve ser inteiro de 64 bits com sinal")
        for key, maximum in (("reset_reason", 255), ("dht_valido", 1)):
            if key in status and (type(status[key]) is not int or not 0 <= status[key] <= maximum):
                errors.append(f"status invalido: {key} deve ser inteiro entre 0 e {maximum}")
        if "corrente_pre_correcao_a" in status:
            value = status["corrente_pre_correcao_a"]
            if not _finite_number(value) or value < 0:
                errors.append("corrente_pre_correcao_a deve ser numero finito nao negativo")
    dados = payload.get("dados")
    if not isinstance(dados, dict):
        errors.append("dados deve ser um objeto JSON")
    else:
        if set(dados) != set(MEASUREMENT_FIELDS):
            errors.append("dados deve conter exatamente as dez medicoes")
        for key in MEASUREMENT_FIELDS:
            if key in dados and not _finite_number(dados[key]):
                errors.append(f"medicao deve ser numero finito: {key}")
    if "timestamp" in payload:
        try:
            _timestamp_ns(payload["timestamp"])
        except (ValueError, OverflowError):
            errors.append("timestamp invalido: use YYYY-MM-DDTHH:MM:SS em UTC")
    return errors


@app.errorhandler(413)
def corpo_muito_grande(_error):
    return jsonify(ok=False, erro="Corpo excede o limite de 16384 bytes"), 413


@app.errorhandler(429)
def limite_de_taxa_excedido(_error):
    return jsonify(ok=False, erro="Muitas requisicoes; aguarde antes de tentar novamente"), 429


@app.get("/")
def healthcheck():
	return jsonify(
		{
			"status": "ok",
			"servico": "backend-flask",
			"endpoint_post": "/telemetria",
			"endpoint_debug": "/telemetria/ultimas",
		}
	)


@app.before_request
def autenticar_telemetria():
	# Authenticate before parsing or retaining any request body, including history.
	if request.path not in ("/telemetria", "/telemetria/ultimas", "/firmware-auth"):
		return None
	expected = app.config["API_SECURITY_TOKEN"]
	api_key = request.headers.get("X-API-Key", "")
	authorization = request.headers.get("Authorization", "").split()
	bearer = authorization[1] if len(authorization) == 2 and authorization[0].lower() == "bearer" else ""
	if not expected or not expected.strip() or not any(
		hmac.compare_digest(token.encode("utf-8"), expected.encode("utf-8"))
		for token in (api_key, bearer) if token and token.strip()
	):
		return jsonify({"error": "Unauthorized"}), 401


@app.get("/firmware-auth")
def autorizar_firmware():
    """Caddy checks authorization before serving firmware and signatures."""
    return "", 204


@app.post("/telemetria")
@limiter.limit("5 per second; 200 per minute")
def receber_telemetria():

	if not request.is_json:
		return jsonify({"ok": False, "erro": "Content-Type deve ser application/json"}), 400

	payload = request.get_json(silent=True)
	if not isinstance(payload, dict):
		erro = "JSON invalido" if payload is None else "JSON deve ser um objeto"
		return jsonify({"ok": False, "erro": erro}), 400

	erros = validar_telemetria(payload)
	if erros:
		return jsonify({"ok": False, "erros": erros}), 400

	registro = {
		"recebido_em": datetime.now(timezone.utc).isoformat(timespec="seconds"),
		"payload": payload,
	}

	ULTIMAS_TELEMETRIAS.append(registro)
	if len(ULTIMAS_TELEMETRIAS) > MAX_TELEMETRIAS:
		del ULTIMAS_TELEMETRIAS[0]

	line = _build_line_protocol(payload)
	influx_ok, _ = _escrever_influx(line)
	if not influx_ok:
		app.logger.warning("Falha na persistencia da telemetria")
		return jsonify({
			"ok": False,
			"erro": "Persistencia indisponivel; tente novamente mais tarde",
			"influx": {"ok": False},
		}), 503

	return jsonify(
		{
			"ok": True,
			"mensagem": "Telemetria recebida com sucesso",
			"campos_recebidos": list(payload.keys()),
			"total_recebidas": len(ULTIMAS_TELEMETRIAS),
			"influx": {"ok": True, "erro": None},
		}
	), 200


@app.get("/telemetria/ultimas")
@limiter.limit("20 per minute")
def listar_ultimas_telemetrias():
	return jsonify({"total": len(ULTIMAS_TELEMETRIAS), "itens": ULTIMAS_TELEMETRIAS})


if __name__ == "__main__":
	app.run(host="0.0.0.0", port=5000, debug=False)
