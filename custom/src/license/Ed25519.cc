/****************************************************************************
 *
 * RotorLab GC - Ed25519 (RFC 8032). See Ed25519.h.
 *
 * verify() is crypto_ed25519_check from Monocypher 4.0.3 (SHA-512, pure
 * Ed25519; not crypto_eddsa_check, which is EdDSA over BLAKE2b and would
 * never verify RotorLab's signatures). The Monocypher headers declare their
 * functions extern "C" themselves.
 *
 ****************************************************************************/

#include "Ed25519.h"

#include "third_party/monocypher/monocypher-ed25519.h"
#include "third_party/monocypher/monocypher.h"

#include <cstddef>
#include <cstdint>

namespace {

/// The group order L, little-endian.
const std::uint8_t kL[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
                             0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                             0,    0,    0,    0,    0,    0,    0,    0,
                             0,    0,    0,    0,    0,    0,    0,    0x10};

/// True when the little-endian 32-byte scalar `s` is below L. Monocypher
/// refuses S >= L too (crypto_eddsa_check_equation); GC keeps its own check
/// so the rule does not depend on a library version.
bool scalarBelowL(const std::uint8_t *s)
{
    for (int i = 31; i >= 0; --i) {
        if (s[i] < kL[i]) {
            return true;
        }
        if (s[i] > kL[i]) {
            return false;
        }
    }
    return false;   // equal to L
}

const std::uint8_t *bytes(const QByteArray &a)
{
    return reinterpret_cast<const std::uint8_t *>(a.constData());
}

} // namespace

namespace Ed25519 {

bool verify(const QByteArray &signature, const QByteArray &message, const QByteArray &publicKey)
{
    if (signature.size() != kSignatureSize || publicKey.size() != kPublicKeySize) {
        return false;
    }
    if (!scalarBelowL(bytes(signature) + 32)) {
        return false;
    }
    // 0 is a valid signature; -1 anything else. Never treat non-zero as true.
    return crypto_ed25519_check(bytes(signature), bytes(publicKey),
                                bytes(message), static_cast<std::size_t>(message.size())) == 0;
}

QByteArray publicKeyFromSeed(const QByteArray &seed)
{
    if (seed.size() != kSeedSize) {
        return {};
    }
    std::uint8_t seedCopy[32];   // crypto_ed25519_key_pair wipes the seed it is given
    std::uint8_t secret[64];
    QByteArray pub(kPublicKeySize, '\0');
    for (int i = 0; i < 32; ++i) {
        seedCopy[i] = bytes(seed)[i];
    }
    crypto_ed25519_key_pair(secret, reinterpret_cast<std::uint8_t *>(pub.data()), seedCopy);
    crypto_wipe(secret, sizeof(secret));
    return pub;
}

QByteArray sign(const QByteArray &seed, const QByteArray &message)
{
    if (seed.size() != kSeedSize) {
        return {};
    }
    std::uint8_t seedCopy[32];
    std::uint8_t secret[64];
    std::uint8_t pub[32];
    for (int i = 0; i < 32; ++i) {
        seedCopy[i] = bytes(seed)[i];
    }
    crypto_ed25519_key_pair(secret, pub, seedCopy);
    QByteArray sig(kSignatureSize, '\0');
    crypto_ed25519_sign(reinterpret_cast<std::uint8_t *>(sig.data()), secret,
                        bytes(message), static_cast<std::size_t>(message.size()));
    crypto_wipe(secret, sizeof(secret));
    return sig;
}

} // namespace Ed25519
