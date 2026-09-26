#pragma once

/* Public half of the ECDSA P-256 key used to sign OTA images (see
 * tools/sign_firmware.py and OTA.md). Not secret -- only verifies, never
 * signs. The private key stays local to whoever builds releases and must
 * never be committed or copied to the server. */
#define OTA_SIGNING_PUBLIC_KEY_PEM \
    "-----BEGIN PUBLIC KEY-----\n" \
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEVuMHFzvglMTgVvaVTeh+/CZK/KLG\n" \
    "Og85Y3qCCmwWrKZBUmvyoN0MakADfu3Ir7kMuBtiotiXs9OeF5UoB7N6xA==\n" \
    "-----END PUBLIC KEY-----\n"
