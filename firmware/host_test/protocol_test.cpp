#include "eci_protocol.h"

#include <openssl/evp.h>
#include <openssl/kdf.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using ByteVector = std::vector<std::uint8_t>;

ByteVector DecodeBase64Url(std::string value) {
  for (char& byte : value) {
    if (byte == '-') {
      byte = '+';
    } else if (byte == '_') {
      byte = '/';
    }
  }
  while ((value.size() % 4) != 0) {
    value.push_back('=');
  }
  ByteVector output((value.size() / 4) * 3);
  const int decoded = EVP_DecodeBlock(output.data(),
                                      reinterpret_cast<const unsigned char*>(value.data()),
                                      static_cast<int>(value.size()));
  assert(decoded >= 0);
  std::size_t padding = 0;
  if (!value.empty() && value.back() == '=') {
    ++padding;
  }
  if (value.size() > 1 && value[value.size() - 2] == '=') {
    ++padding;
  }
  output.resize(static_cast<std::size_t>(decoded) - padding);
  return output;
}

std::array<std::uint8_t, eci::kKeyBytes> DeriveKey(const ByteVector& root,
                                                   const ByteVector& salt,
                                                   std::string_view info) {
  std::array<std::uint8_t, eci::kKeyBytes> output{};
  std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
      EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr), EVP_PKEY_CTX_free);
  assert(context != nullptr);
  assert(EVP_PKEY_derive_init(context.get()) == 1);
  assert(EVP_PKEY_CTX_set_hkdf_md(context.get(), EVP_sha256()) == 1);
  assert(EVP_PKEY_CTX_set1_hkdf_salt(context.get(), salt.data(), salt.size()) == 1);
  assert(EVP_PKEY_CTX_set1_hkdf_key(context.get(), root.data(), root.size()) == 1);
  assert(EVP_PKEY_CTX_add1_hkdf_info(
             context.get(), reinterpret_cast<const unsigned char*>(info.data()), info.size()) == 1);
  std::size_t output_size = output.size();
  assert(EVP_PKEY_derive(context.get(), output.data(), &output_size) == 1);
  assert(output_size == output.size());
  return output;
}

ByteVector Seal(const std::array<std::uint8_t, eci::kKeyBytes>& key,
                const ByteVector& nonce, std::string_view aad, const ByteVector& plaintext) {
  std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> context(
      EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
  assert(context != nullptr);
  assert(EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1);
  assert(EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN,
                             static_cast<int>(nonce.size()), nullptr) == 1);
  assert(EVP_EncryptInit_ex(context.get(), nullptr, nullptr, key.data(), nonce.data()) == 1);

  int written = 0;
  assert(EVP_EncryptUpdate(context.get(), nullptr, &written,
                           reinterpret_cast<const unsigned char*>(aad.data()),
                           static_cast<int>(aad.size())) == 1);
  ByteVector ciphertext(plaintext.size() + 16);
  assert(EVP_EncryptUpdate(context.get(), ciphertext.data(), &written, plaintext.data(),
                           static_cast<int>(plaintext.size())) == 1);
  int total = written;
  assert(EVP_EncryptFinal_ex(context.get(), ciphertext.data() + total, &written) == 1);
  total += written;
  std::array<std::uint8_t, 16> tag{};
  assert(EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, tag.size(), tag.data()) == 1);
  ciphertext.resize(static_cast<std::size_t>(total));
  ciphertext.insert(ciphertext.end(), tag.begin(), tag.end());
  return ciphertext;
}

bool Open(const std::array<std::uint8_t, eci::kKeyBytes>& key, const ByteVector& nonce,
          std::string_view aad, const ByteVector& ciphertext, ByteVector* plaintext) {
  if (plaintext == nullptr || ciphertext.size() < 16) {
    return false;
  }
  const std::size_t body_size = ciphertext.size() - 16;
  std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> context(
      EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
  if (context == nullptr ||
      EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
      EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN,
                          static_cast<int>(nonce.size()), nullptr) != 1 ||
      EVP_DecryptInit_ex(context.get(), nullptr, nullptr, key.data(), nonce.data()) != 1) {
    return false;
  }
  int written = 0;
  if (EVP_DecryptUpdate(context.get(), nullptr, &written,
                        reinterpret_cast<const unsigned char*>(aad.data()),
                        static_cast<int>(aad.size())) != 1) {
    return false;
  }
  plaintext->assign(body_size, 0);
  if (EVP_DecryptUpdate(context.get(), plaintext->data(), &written, ciphertext.data(),
                        static_cast<int>(body_size)) != 1) {
    return false;
  }
  int total = written;
  std::array<std::uint8_t, 16> tag{};
  std::copy(ciphertext.begin() + static_cast<std::ptrdiff_t>(body_size), ciphertext.end(),
            tag.begin());
  if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, tag.size(), tag.data()) != 1 ||
      EVP_DecryptFinal_ex(context.get(), plaintext->data() + total, &written) != 1) {
    plaintext->clear();
    return false;
  }
  total += written;
  plaintext->resize(static_cast<std::size_t>(total));
  return true;
}

}  // namespace

int main() {
  constexpr std::string_view kInfo = "eci.v1/device-to-host/audio";
  constexpr std::string_view kAad =
      "{\"v\":1,\"device\":\"dev_test\",\"stream\":\"stream_1\",\"seq\":42,"
      "\"sent_at\":1785661200,\"kind\":\"ptt.audio\"}";

  const ByteVector root = DecodeBase64Url("AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8");
  const ByteVector salt = DecodeBase64Url("oKGio6SlpqeoqaqrrK2urw");
  const ByteVector nonce = DecodeBase64Url("8PHy8_T19vf4-fr7");
  const ByteVector plaintext =
      DecodeBase64Url("c2xvdD0yO2ZyYW1lPTc7aW1hPTAwMTEyMjMzNDQ1NQ");
  const ByteVector expected_key =
      DecodeBase64Url("x-zSq0MHnsywfBt_GwdgZObKIRDg8V5NKo_EmPQ_SZ8");
  const ByteVector expected_ciphertext = DecodeBase64Url(
      "Ym7rGkHLZp1qu1BFncT-DlAcPV_PHFV4KGymdy189CGp1zEBRoG0CmQlO0qpfI4");

  const auto key = DeriveKey(root, salt, kInfo);
  assert(ByteVector(key.begin(), key.end()) == expected_key);
  const ByteVector ciphertext = Seal(key, nonce, kAad, plaintext);
  assert(ciphertext == expected_ciphertext);
  ByteVector opened;
  assert(Open(key, nonce, kAad, ciphertext, &opened));
  assert(opened == plaintext);
  ByteVector truncated = ciphertext;
  truncated.pop_back();
  assert(!Open(key, nonce, kAad, truncated, &opened));

  assert(eci::IsSupportedVersion(1));
  assert(!eci::IsSupportedVersion(0));
  assert(!eci::IsSupportedVersion(2));
  assert(eci::IsValidIdentifier("dev_test"));
  assert(eci::IsValidIdentifier(std::string(eci::kMaxIdentifierBytes, 'a')));
  assert(!eci::IsValidIdentifier(std::string(eci::kMaxIdentifierBytes + 1, 'a')));
  assert(!eci::IsValidIdentifier("bad/device"));
  eci::MessageKind kind{};
  assert(eci::ParseMessageKind("ptt.audio", &kind));
  assert(kind == eci::MessageKind::kPttAudio);
  assert(!eci::ParseMessageKind("unknown", &kind));

  eci::ReplayWindow replay;
  assert(replay.CheckAndRecord(10) == eci::ReplayResult::kAccepted);
  assert(replay.CheckAndRecord(10) == eci::ReplayResult::kDuplicate);
  assert(replay.CheckAndRecord(12) == eci::ReplayResult::kAccepted);
  assert(replay.CheckAndRecord(11) == eci::ReplayResult::kAccepted);
  assert(replay.CheckAndRecord(76) == eci::ReplayResult::kAccepted);
  assert(replay.CheckAndRecord(12) == eci::ReplayResult::kStale);

  eci::FrameTracker tracker;
  tracker.NoteTransportSend(7);
  tracker.NoteTransportSend(8);
  assert(tracker.IsPending(7));
  assert(tracker.PendingCount() == 2);
  assert(!tracker.ApplyBusinessAck(9));
  assert(tracker.ApplyBusinessAck(7));
  assert(!tracker.IsPending(7));
  assert(tracker.IsPending(8));

  std::cout << "eci C++ golden vector passed\n";
  return 0;
}
