/****************************************************************************
 *
 * RotorLab GC - Ed25519 (RFC 8032, pure Ed25519 with SHA-512) for the
 * signed offline activation record (G89).
 *
 * A thin wrapper over Monocypher 4.0.3's optional Ed25519 module
 * (custom/src/license/third_party/monocypher, vendored unmodified;
 * BSD-2-Clause or CC0-1.0). See its README and NOTICE.
 *
 * GC only ever verifies. sign() and publicKeyFromSeed() exist so the unit
 * tests can make records with a key pair generated inside the test; GC holds
 * no private key and never signs anything.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QByteArray>

namespace Ed25519 {

constexpr int kPublicKeySize = 32;
constexpr int kSeedSize      = 32;
constexpr int kSignatureSize = 64;

/// True when `signature` (64 bytes) is `publicKey`'s (32 bytes) pure Ed25519
/// signature over `message`. A signature whose S is not below the group order
/// is refused (RFC 8032 section 5.1.7), so a signature has one spelling.
bool verify(const QByteArray &signature, const QByteArray &message, const QByteArray &publicKey);

/// Tests only. The 32-byte public key of a 32-byte seed (RFC 8032 secret key).
QByteArray publicKeyFromSeed(const QByteArray &seed);

/// Tests only. The 64-byte signature of `message` under the 32-byte `seed`.
QByteArray sign(const QByteArray &seed, const QByteArray &message);

} // namespace Ed25519
