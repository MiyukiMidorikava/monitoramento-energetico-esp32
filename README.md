# Sistema de Monitoramento Energético

Protótipo de aquisição e supervisão com ESP32-S3, ESP-IDF/FreeRTOS, Flask, InfluxDB 3 Core e Grafana.

Mede tensão e corrente RMS, calcula potência e energia, acompanha temperatura e umidade e transmite telemetria por HTTPS. A infraestrutura grava séries temporais e disponibiliza painéis de acompanhamento.

```text
ESP32-S3 → HTTPS/Caddy → Flask → InfluxDB → Grafana
```

## Estrutura

- `firmware/firmware-esp32/`: firmware, componentes e testes de host.
- `backend/`: API, testes e Docker Compose.
- `infra/caddy/`: gateway HTTPS e autorização de download OTA.
- `infra/grafana/`: dashboard para configurar na própria instalação.

## Firmware

Use ESP-IDF 5.5.3, alvo ESP32-S3 e flash de 4 MB.

```sh
cd firmware/firmware-esp32
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
```

Em `Monitor Energetico`, configure rede Wi-Fi, token e URLs HTTPS do seu servidor. `energia.example.invalid` é um endereço de exemplo, sem serviço associado. O token deve coincidir com `API_SECURITY_TOKEN` no backend. Calibre os sensores para sua montagem; os parâmetros incluídos não substituem ensaios de referência.

Uma instalação inicial usa `idf.py -p PORTA flash monitor`. Antes de atualizar instalações existentes, verifique partições, credenciais e compatibilidade. Consulte [OTA](firmware/firmware-esp32/OTA.md).

## Backend e infraestrutura

No diretório `backend/`, copie `.env.example` para `.env` e preencha domínio, senha do Grafana, token da API e token de escrita do InfluxDB. Use credenciais exclusivas. Crie o banco e os tokens de forma privada conforme a documentação do InfluxDB; o projeto não fornece credenciais ou inicialização sem autenticação.

```sh
cd backend
docker compose --env-file .env config --quiet
docker compose --env-file .env up -d --build
```

O domínio deve apontar para o host, com portas 80/443 disponíveis para o Caddy. Flask, Grafana e InfluxDB não publicam portas diretamente. Preserve os volumes e `backend/influxdb3/data/` nas atualizações.

Configure no Grafana uma fonte InfluxDB/SQL de leitura com endereço interno `http://influxdb:8181`, banco configurado e identificador `influxdb-energy`, ou adapte o dashboard. O arquivo utiliza Grafana Dashboard v2: confira a compatibilidade da versão instalada. Nenhuma credencial acompanha o dashboard.

## API

- `POST /telemetria`: JSON com `device`, `uptime_ms`, `status` e `dados`, autenticado por `X-API-Key` ou Bearer.
- `GET /telemetria/ultimas`: últimas mensagens em memória, com autenticação.
- `/firmware-auth`: verificação interna utilizada pelo Caddy.

HTTP 200 confirma a escrita no banco; falhas de persistência retornam 503. O histórico em memória não substitui o banco. Diagnósticos adicionais do firmware são opcionais no backend para compatibilidade com mensagens anteriores.

## Testes

```sh
python -m pip install -r backend/requirements.txt
python -m unittest discover -s backend/tests -v
python firmware/firmware-esp32/tests/run_host_tests.py --idf-path CAMINHO_ESP_IDF --cc COMPILADOR_C
```

O runner também aceita `--zig CAMINHO_ZIG`. Os testes simulam periféricos e chamadas ao banco; não substituem validação elétrica, ensaio em hardware ou auditoria da implantação.

## Segurança e terceiros

Leia [SECURITY.md](SECURITY.md). A configuração OTA exige cliente compatível: ativá-la antes da atualização de um cliente antigo bloqueia seus downloads. Substitua a chave pública de verificação pela correspondente à chave de assinatura da sua instalação.

As licenças e os avisos do driver SSD1306 estão preservados em `firmware/firmware-esp32/components/espressif__ssd1306/`. As demais dependências mantêm suas próprias licenças. O protótipo não é um instrumento certificado para faturamento ou proteção elétrica.
