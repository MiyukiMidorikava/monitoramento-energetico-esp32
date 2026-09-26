"""Assina um firmware.bin com ECDSA P-256 + SHA-256 para o gate de OTA.

Uso:
    python tools/sign_firmware.py <firmware.bin> <chave_privada.pem> [saida.sig]

A saida (por padrao <firmware.bin>.sig) contem a assinatura DER da
assinatura ECDSA sobre o SHA-256 do arquivo completo. O dispositivo
verifica essa assinatura com a chave publica embutida em
main/ota_signing_key.h antes de trocar o banco de boot; ver OTA.md.

A chave privada NUNCA deve ser copiada para a VPS nem para o
repositorio -- assine localmente, publique so o .bin e o .sig.
"""
import sys
from pathlib import Path

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec


def main() -> int:
    if len(sys.argv) not in (3, 4):
        print(__doc__)
        return 1

    firmware_path = Path(sys.argv[1])
    key_path = Path(sys.argv[2])
    sig_path = Path(sys.argv[3]) if len(sys.argv) == 4 else firmware_path.with_suffix(firmware_path.suffix + ".sig")

    private_key = serialization.load_pem_private_key(key_path.read_bytes(), password=None)
    if not isinstance(private_key, ec.EllipticCurvePrivateKey):
        print("Chave nao e ECDSA (EllipticCurvePrivateKey)")
        return 1

    data = firmware_path.read_bytes()
    signature = private_key.sign(data, ec.ECDSA(hashes.SHA256()))
    sig_path.write_bytes(signature)
    print(f"Assinado: {sig_path} ({len(signature)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
