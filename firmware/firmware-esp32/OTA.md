# Atualização OTA

A imagem candidata é gravada na partição inativa e selecionada para o próximo boot somente após validação e assinatura válida. A confirmação operacional exige dez respostas HTTP 200 consecutivas da telemetria em 120 segundos; caso contrário, há tentativa de reversão.

Configure `MONITOR_OTA_URL` com HTTPS e `MONITOR_API_TOKEN` igual ao token do backend. Imagem e `<URL>.sig` são baixadas com `X-API-Key`. A assinatura é uma verificação independente da autorização.

## Assinatura

Crie uma chave ECDSA P-256 fora do repositório. Substitua a constante PEM em `main/ota_signing_key.h` pela chave pública correspondente. A chave pública incluída não permite assinar novas imagens. Instale a chave pública correta no dispositivo antes de usar seu processo de atualização.

Instale `cryptography` no ambiente Python de assinatura e execute:

```sh
python tools/sign_firmware.py build/firmware-esp32.bin CAMINHO_PRIVADO/chave.pem firmware.bin.sig
```

Coloque somente a imagem de aplicação em `backend/firmware/firmware.bin` e a assinatura em `backend/firmware/firmware.bin.sig`. O Caddy monta esse diretório em modo somente leitura. Não publique os binários no repositório: podem conter credenciais.

Para clientes antigos sem cabeçalho OTA, confirme primeiro a instalação da versão compatível, por cabo ou pelo mecanismo assinado já provisionado, antes de exigir autenticação no gateway. A cópia pública requer configuração própria e não deve substituir diretamente uma instalação em uso.
