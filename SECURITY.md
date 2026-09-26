# Segurança

## Configuração

Use segredos exclusivos fora do repositório. Nunca publique `.env`, `sdkconfig`, chaves privadas, dumps do banco ou binários com credenciais. Os exemplos estão vazios e os endpoints devem ser configurados para sua instalação.

A API exige token e compara credenciais em tempo constante. O gateway autoriza antes de entregar o firmware e a assinatura. O cliente verifica HTTPS e assinatura ECDSA P-256/SHA-256, envia credencial nos dois downloads e não segue redirecionamentos. Falta de autorização ou indisponibilidade do backend bloqueia o arquivo.

`TRUST_CADDY_PROXY=1` exige um único Caddy confiável e nenhum acesso público direto ao backend. O Caddy substitui o IP encaminhado pelo IP da conexão. Não adicione proxies ou publique a porta do backend sem revisar essa configuração.

## Instalações existentes

Clientes antigos podem não enviar autenticação OTA. Planeje a transição: instale e confirme uma versão assinada compatível antes de exigir o token de download no gateway. Não altere unilateralmente token ou senha Wi-Fi sem atualizar o dispositivo. Não publique uma rota alternativa aberta para contornar a proteção.

## Limitações

O token é compartilhado, sem autorização individual por dispositivo. Credenciais do firmware são incorporadas à imagem: autenticar o download não cifra o binário. Secure Boot e Flash Encryption não são habilitados. Cópias antigas podem exigir rotação coordenada de credenciais. A chave de assinatura deve ficar fora do servidor de distribuição.

Os limites de requisição e o histórico recente ficam na memória de um processo. Múltiplas réplicas exigem armazenamento compartilhado para os limites. Backups, firewall, contas, atualização e auditoria das imagens Docker são responsabilidades da implantação.

Testes e auditoria de dependências não garantem ausência de vulnerabilidades. Não inclua credenciais ao relatar problemas.
