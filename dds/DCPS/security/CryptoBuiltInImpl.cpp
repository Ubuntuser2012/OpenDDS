/*
 * Distributed under the OpenDDS License.
 * See: http://www.opendds.org/license.html
 */

#include "CryptoBuiltInImpl.h"

#include "CryptoBuiltInTypeSupportImpl.h"
#include "CommonUtilities.h"
#include "TokenWriter.h"

#include "SSL/Utils.h"

#include "dds/DdsDcpsInfrastructureC.h"
#include "dds/DdsSecurityParamsC.h"

#include "dds/DCPS/GuidUtils.h"
#include "dds/DCPS/Message_Block_Ptr.h"
#include "dds/DCPS/Serializer.h"

#include "dds/DCPS/RTPS/RtpsCoreTypeSupportImpl.h"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

using namespace DDS::Security;
using OpenDDS::DCPS::Serializer;
using OpenDDS::DCPS::Message_Block_Ptr;

namespace OpenDDS {
namespace Security {

CryptoBuiltInImpl::CryptoBuiltInImpl()
  : mutex_()
  , next_handle_(1)
{}

CryptoBuiltInImpl::~CryptoBuiltInImpl()
{}

bool CryptoBuiltInImpl::_is_a(const char* id)
{
  return CryptoKeyFactory::_is_a(id)
    || CryptoKeyExchange::_is_a(id)
    || CryptoTransform::_is_a(id);
}

const char* CryptoBuiltInImpl::_interface_repository_id() const
{
  return "";
}

bool CryptoBuiltInImpl::marshal(TAO_OutputCDR&)
{
  return false;
}

NativeCryptoHandle CryptoBuiltInImpl::generate_handle()
{
  ACE_Guard<ACE_Thread_Mutex> guard(mutex_);
  int new_handle = next_handle_++;

  if (new_handle == DDS::HANDLE_NIL) {
    new_handle = next_handle_++;
  }
  return static_cast<NativeCryptoHandle>(new_handle);
}


// Key Factory

namespace {
  const unsigned int KEY_LEN_BYTES = 32;
  const unsigned int BLOCK_LEN_BYTES = 16;
  const unsigned int MAX_BLOCKS_PER_SESSION = 1024;

  KeyMaterial_AES_GCM_GMAC make_key(unsigned int key_id, bool encrypt)
  {
    KeyMaterial_AES_GCM_GMAC k;

    for (unsigned int i = 0; i < TransformKindIndex; ++i) {
      k.transformation_kind[i] = 0;
    }
    k.transformation_kind[TransformKindIndex] =
      encrypt ? CRYPTO_TRANSFORMATION_KIND_AES256_GCM
      : CRYPTO_TRANSFORMATION_KIND_AES256_GMAC;

    k.master_salt.length(KEY_LEN_BYTES);
    RAND_bytes(k.master_salt.get_buffer(), KEY_LEN_BYTES);

    for (unsigned int i = 0; i < sizeof k.sender_key_id; ++i) {
      k.sender_key_id[i] = key_id >> (8 * i);
    }

    k.master_sender_key.length(KEY_LEN_BYTES);
    RAND_bytes(k.master_sender_key.get_buffer(), KEY_LEN_BYTES);

    for (unsigned int i = 0; i < sizeof k.receiver_specific_key_id; ++i) {
      k.receiver_specific_key_id[i] = 0;
    }

    k.master_receiver_specific_key.length(0);
    return k;
  }

  template <typename T, typename TSeq>
  void push_back(TSeq& seq, const T& t)
  {
    const unsigned int i = seq.length();
    seq.length(i + 1);
    seq[i] = t;
  }
}

ParticipantCryptoHandle CryptoBuiltInImpl::register_local_participant(
  IdentityHandle participant_identity,
  PermissionsHandle participant_permissions,
  const DDS::PropertySeq&,
  const ParticipantSecurityAttributes& participant_security_attributes,
  SecurityException& ex)
{
  if (DDS::HANDLE_NIL == participant_identity) {
    CommonUtilities::set_security_error(ex, -1, 0, "Invalid local participant ID");
    return DDS::HANDLE_NIL;
  }
  if (DDS::HANDLE_NIL == participant_permissions) {
    CommonUtilities::set_security_error(ex, -1, 0, "Invalid local permissions");
    return DDS::HANDLE_NIL;
  }

  const NativeCryptoHandle h = generate_handle();
  if (!participant_security_attributes.is_rtps_protected) {
    return h;
  }

  CommonUtilities::set_security_error(ex, -1, 0, "Unsupported configuration");
  return DDS::HANDLE_NIL;
}

ParticipantCryptoHandle CryptoBuiltInImpl::register_matched_remote_participant(
  ParticipantCryptoHandle local_participant_crypto_handle,
  IdentityHandle remote_participant_identity,
  PermissionsHandle /*remote_participant_permissions*/,
  SharedSecretHandle* shared_secret,
  SecurityException& ex)
{
  if (DDS::HANDLE_NIL == local_participant_crypto_handle) {
    CommonUtilities::set_security_error(ex, -1, 0, "Invalid local participant crypto handle");
    return DDS::HANDLE_NIL;
  }
  if (DDS::HANDLE_NIL == remote_participant_identity) {
    CommonUtilities::set_security_error(ex, -1, 0, "Invalid remote participant ID");
    return DDS::HANDLE_NIL;
  }
  if (!shared_secret) {
    CommonUtilities::set_security_error(ex, -1, 0, "Invalid Shared Secret data");
    return DDS::HANDLE_NIL;
  }

  return generate_handle();
}

namespace {
  bool is_builtin_volatile(const DDS::PropertySeq& props)
  {
    for (unsigned int i = 0; i < props.length(); ++i) {
      if (0 == std::strcmp(props[i].name.in(),
                           "dds.sec.builtin_endpoint_name")) {
        return 0 == std::strcmp(props[i].value.in(),
                                "BuiltinParticipantVolatileMessageSecureWriter")
          || 0 == std::strcmp(props[i].value.in(),
                              "BuiltinParticipantVolatileMessageSecureReader");
      }
    }
    return false;
  }

  bool is_volatile_placeholder(const KeyMaterial_AES_GCM_GMAC& keymat)
  {
    static const CryptoTransformKind placeholder =
      {DCPS::VENDORID_OCI[0], DCPS::VENDORID_OCI[1], 0, 1};
    return 0 == std::memcmp(placeholder, keymat.transformation_kind,
                            sizeof placeholder);
  }

  KeyMaterial_AES_GCM_GMAC make_volatile_placeholder()
  {
    // not an actual key, just used to identify the local datawriter/reader
    // crypto handle for a Built-In Participant Volatile Msg endpoint
    const KeyMaterial_AES_GCM_GMAC k = {
      {DCPS::VENDORID_OCI[0], DCPS::VENDORID_OCI[1], 0, 1},
      KeyOctetSeq(), {0, 0, 0, 0}, KeyOctetSeq(), {0, 0, 0, 0}, KeyOctetSeq()
    };
    return k;
  }

  struct PrivateKey {
    EVP_PKEY* pkey_;
    explicit PrivateKey(const KeyOctetSeq& key)
      : pkey_(EVP_PKEY_new_mac_key(EVP_PKEY_HMAC, 0, key.get_buffer(),
                                   key.length())) {}
    explicit PrivateKey(const DDS::OctetSeq& key)
      : pkey_(EVP_PKEY_new_mac_key(EVP_PKEY_HMAC, 0, key.get_buffer(),
                                   key.length())) {}
    operator EVP_PKEY*() { return pkey_; }
    ~PrivateKey() { EVP_PKEY_free(pkey_); }
  };

  struct DigestContext {
    EVP_MD_CTX* ctx_;
    DigestContext() : ctx_(EVP_MD_CTX_new()) {}
    operator EVP_MD_CTX*() { return ctx_; }
    ~DigestContext() { EVP_MD_CTX_free(ctx_); }
  };

  void hkdf(KeyOctetSeq& result, const DDS::OctetSeq_var& prefix,
            const char (&cookie)[17], const DDS::OctetSeq_var& suffix,
            const DDS::OctetSeq_var& data)
  {
    char* cookie_buffer = const_cast<char*>(cookie); // OctetSeq has no const
    DDS::OctetSeq cookieSeq(16, 16,
                            reinterpret_cast<CORBA::Octet*>(cookie_buffer));
    std::vector<const DDS::OctetSeq*> input(3);
    input[0] = prefix.ptr();
    input[1] = &cookieSeq;
    input[2] = suffix.ptr();
    DDS::OctetSeq key;
    if (SSL::hash(input, key) != 0) {
      return;
    }

    PrivateKey pkey(key);
    DigestContext ctx;
    const EVP_MD* md = EVP_get_digestbyname("SHA256");
    if (EVP_DigestInit_ex(ctx, md, 0) != 1) {
      return;
    }

    if (EVP_DigestSignInit(ctx, 0, md, 0, pkey) != 1) {
      return;
    }

    if (EVP_DigestSignUpdate(ctx, data->get_buffer(), data->length()) != 1) {
      return;
    }

    size_t req = 0;
    if (EVP_DigestSignFinal(ctx, 0, &req) != 1) {
      return;
    }

    result.length(static_cast<unsigned int>(req));
    if (EVP_DigestSignFinal(ctx, result.get_buffer(), &req) != 1) {
      result.length(0);
    }
  }

  KeyMaterial_AES_GCM_GMAC
  make_volatile_key(const DDS::OctetSeq_var& challenge1,
                    const DDS::OctetSeq_var& challenge2,
                    const DDS::OctetSeq_var& sharedSec)
  {
    static const char KxSaltCookie[] = "keyexchange salt";
    static const char KxKeyCookie[] = "key exchange key";
    KeyMaterial_AES_GCM_GMAC k = {
      {0, 0, 0, CRYPTO_TRANSFORMATION_KIND_AES256_GCM},
      KeyOctetSeq(), {0, 0, 0, 0}, KeyOctetSeq(), {0, 0, 0, 0}, KeyOctetSeq()
    };
    hkdf(k.master_salt, challenge1, KxSaltCookie, challenge2, sharedSec);
    hkdf(k.master_sender_key, challenge2, KxKeyCookie, challenge1, sharedSec);
    return k;
  }
}

DatawriterCryptoHandle CryptoBuiltInImpl::register_local_datawriter(
  ParticipantCryptoHandle participant_crypto,
  const DDS::PropertySeq& properties,
  const EndpointSecurityAttributes& security_attributes,
  SecurityException& ex)
{
  if (DDS::HANDLE_NIL == participant_crypto) {
    CommonUtilities::set_security_error(ex, -1, 0, "Invalid Participant Crypto Handle");
    return DDS::HANDLE_NIL;
  }

  const NativeCryptoHandle h = generate_handle();
  const PluginEndpointSecurityAttributesMask plugin_attribs =
    security_attributes.plugin_endpoint_attributes;
  KeySeq keys;

  if (is_builtin_volatile(properties)) {
    push_back(keys, make_volatile_placeholder());

  } else {
    bool used_h = false;
    if (security_attributes.is_submessage_protected) {
      push_back(keys,
                make_key(h, plugin_attribs & FLAG_IS_SUBMESSAGE_ENCRYPTED));
      used_h = true;
    }
    if (security_attributes.is_payload_protected) {
      const unsigned int key_id = used_h ? generate_handle() : h;
      push_back(keys,
                make_key(key_id, plugin_attribs & FLAG_IS_PAYLOAD_ENCRYPTED));
    }
  }

  ACE_Guard<ACE_Thread_Mutex> guard(mutex_);
  keys_[h] = keys;
  EntityInfo e(DATAWRITER_SUBMESSAGE, h);
  participant_to_entity_.insert(std::make_pair(participant_crypto, e));

  return h;
}

DatareaderCryptoHandle CryptoBuiltInImpl::register_matched_remote_datareader(
  DatawriterCryptoHandle local_datawriter_crypto_handle,
  ParticipantCryptoHandle remote_participant_crypto,
  SharedSecretHandle* shared_secret,
  bool /*relay_only*/,
  SecurityException& ex)
{
  if (DDS::HANDLE_NIL == local_datawriter_crypto_handle) {
    CommonUtilities::set_security_error(ex, -1, 0, "Invalid Local DataWriter Crypto Handle");
    return DDS::HANDLE_NIL;
  }
  if (DDS::HANDLE_NIL == remote_participant_crypto) {
    CommonUtilities::set_security_error(ex, -1, 0, "Invalid Remote Participant Crypto Handle");
    return DDS::HANDLE_NIL;
  }
  if (!shared_secret) {
    CommonUtilities::set_security_error(ex, -1, 0, "Invalid Shared Secret Handle");
    return DDS::HANDLE_NIL;
  }

  const DatareaderCryptoHandle h = generate_handle();
  ACE_Guard<ACE_Thread_Mutex> guard(mutex_);
  if (!keys_.count(local_datawriter_crypto_handle)) {
    CommonUtilities::set_security_error(ex, -1, 0, "Invalid Local DataWriter Crypto Handle");
    return DDS::HANDLE_NIL;
  }
  const KeySeq& dw_keys = keys_[local_datawriter_crypto_handle];

  if (dw_keys.length() == 1 && is_volatile_placeholder(dw_keys[0])) {
    // Create a key from SharedSecret and track it as if Key Exchange happened
    KeySeq dr_keys(1);
    dr_keys.length(1);
    dr_keys[0] = make_volatile_key(shared_secret->challenge1(),
                                   shared_secret->challenge2(),
                                   shared_secret->sharedSecret());
    if (!dr_keys[0].master_salt.length()
        || !dr_keys[0].master_sender_key.length()) {
      CommonUtilities::set_security_error(ex, -1, 0, "Couldn't create key for "
                                          "volatile remote reader");
      return DDS::HANDLE_NIL;
    }
    keys_[h] = dr_keys;
  }

  EntityInfo e(DATAREADER_SUBMESSAGE, h);
  participant_to_entity_.insert(std::make_pair(remote_participant_crypto, e));
  return h;
}

DatareaderCryptoHandle CryptoBuiltInImpl::register_local_datareader(
  ParticipantCryptoHandle participant_crypto,
  const DDS::PropertySeq& properties,
  const EndpointSecurityAttributes& security_attributes,
  SecurityException& ex)
{
  if (DDS::HANDLE_NIL == participant_crypto) {
    CommonUtilities::set_security_error(ex, -1, 0, "Invalid Participant Crypto Handle");
    return DDS::HANDLE_NIL;
  }

  const NativeCryptoHandle h = generate_handle();
  const PluginEndpointSecurityAttributesMask plugin_attribs =
    security_attributes.plugin_endpoint_attributes;
  KeySeq keys;

  if (is_builtin_volatile(properties)) {
    push_back(keys, make_volatile_placeholder());

  } else {
    if (security_attributes.is_submessage_protected) {
      push_back(keys,
                make_key(h, plugin_attribs & FLAG_IS_SUBMESSAGE_ENCRYPTED));
    }
  }

  ACE_Guard<ACE_Thread_Mutex> guard(mutex_);
  keys_[h] = keys;
  EntityInfo e(DATAREADER_SUBMESSAGE, h);
  participant_to_entity_.insert(std::make_pair(participant_crypto, e));

  return h;
}

DatawriterCryptoHandle CryptoBuiltInImpl::register_matched_remote_datawriter(
  DatareaderCryptoHandle local_datareader_crypto_handle,
  ParticipantCryptoHandle remote_participant_crypto,
  SharedSecretHandle* shared_secret,
  SecurityException& ex)
{
  if (DDS::HANDLE_NIL == local_datareader_crypto_handle) {
    CommonUtilities::set_security_error(ex, -1, 0, "Invalid Local DataWriter Crypto Handle");
    return DDS::HANDLE_NIL;
  }
  if (DDS::HANDLE_NIL == remote_participant_crypto) {
    CommonUtilities::set_security_error(ex, -1, 0, "Invalid Remote Participant Crypto Handle");
    return DDS::HANDLE_NIL;
  }
  if (!shared_secret) {
    CommonUtilities::set_security_error(ex, -1, 0, "Invalid Shared Secret Handle");
    return DDS::HANDLE_NIL;
  }

  const DatareaderCryptoHandle h = generate_handle();
  ACE_Guard<ACE_Thread_Mutex> guard(mutex_);
  if (!keys_.count(local_datareader_crypto_handle)) {
    CommonUtilities::set_security_error(ex, -1, 0, "Invalid Local DataReader Crypto Handle");
    return DDS::HANDLE_NIL;
  }
  const KeySeq& dr_keys = keys_[local_datareader_crypto_handle];

  if (dr_keys.length() == 1 && is_volatile_placeholder(dr_keys[0])) {
    // Create a key from SharedSecret and track it as if Key Exchange happened
    KeySeq dw_keys(1);
    dw_keys.length(1);
    dw_keys[0] = make_volatile_key(shared_secret->challenge1(),
                                   shared_secret->challenge2(),
                                   shared_secret->sharedSecret());
    if (!dw_keys[0].master_salt.length()
        || !dw_keys[0].master_sender_key.length()) {
      CommonUtilities::set_security_error(ex, -1, 0, "Couldn't create key for "
                                          "volatile remote writer");
      return DDS::HANDLE_NIL;
    }
    keys_[h] = dw_keys;
  }

  EntityInfo e(DATAWRITER_SUBMESSAGE, h);
  participant_to_entity_.insert(std::make_pair(remote_participant_crypto, e));
  return h;
}

bool CryptoBuiltInImpl::unregister_participant(
  ParticipantCryptoHandle participant_crypto_handle,
  SecurityException& ex)
{
  ACE_UNUSED_ARG(participant_crypto_handle);
  ACE_UNUSED_ARG(ex);

  // The stub will always succeed here
  return true;
}

bool CryptoBuiltInImpl::unregister_datawriter(
  DatawriterCryptoHandle datawriter_crypto_handle,
  SecurityException& ex)
{
  ACE_UNUSED_ARG(datawriter_crypto_handle);
  ACE_UNUSED_ARG(ex);

  // The stub will always succeed here
  return true;
}

bool CryptoBuiltInImpl::unregister_datareader(
  DatareaderCryptoHandle datareader_crypto_handle,
  SecurityException& ex)
{
  ACE_UNUSED_ARG(datareader_crypto_handle);
  ACE_UNUSED_ARG(ex);

  // The stub will always succeed here
  return true;
}


// Key Exchange

namespace {
  const char Crypto_Token_Class_Id[] = "DDS:Crypto:AES_GCM_GMAC";
  const char Token_KeyMat_Name[] = "dds.cryp.keymat";

  const char* to_mb(const unsigned char* buffer)
  {
    return reinterpret_cast<const char*>(buffer);
  }

  ParticipantCryptoTokenSeq
  keys_to_tokens(const KeyMaterial_AES_GCM_GMAC_Seq& keys)
  {
    ParticipantCryptoTokenSeq tokens;
    for (unsigned int i = 0; i < keys.length(); ++i) {
      CryptoToken t;
      t.class_id = Crypto_Token_Class_Id;
      t.binary_properties.length(1);
      DDS::BinaryProperty_t& p = t.binary_properties[0];
      p.name = Token_KeyMat_Name;
      p.propagate = true;
      size_t size = 0, padding = 0;
      DCPS::gen_find_size(keys[i], size, padding);
      p.value.length(static_cast<unsigned int>(size + padding));
      ACE_Message_Block mb(to_mb(p.value.get_buffer()), size + padding);
      Serializer ser(&mb, Serializer::SWAP_BE, Serializer::ALIGN_CDR);
      if (ser << keys[i]) {
        push_back(tokens, t);
      }
    }
    return tokens;
  }

  KeyMaterial_AES_GCM_GMAC_Seq
  tokens_to_keys(const ParticipantCryptoTokenSeq& tokens)
  {
    KeyMaterial_AES_GCM_GMAC_Seq keys;
    for (unsigned int i = 0; i < tokens.length(); ++i) {
      const CryptoToken& t = tokens[i];
      if (0 == std::strcmp(t.class_id, Crypto_Token_Class_Id)) {
        for (unsigned int j = 0; j < t.binary_properties.length(); ++j) {
          const DDS::BinaryProperty_t& p = t.binary_properties[j];
          if (0 == std::strcmp(p.name, Token_KeyMat_Name)) {
            ACE_Message_Block mb(to_mb(p.value.get_buffer()), p.value.length());
            mb.wr_ptr(p.value.length());
            Serializer ser(&mb, Serializer::SWAP_BE, Serializer::ALIGN_CDR);
            KeyMaterial_AES_GCM_GMAC key;
            if (ser >> key) {
              push_back(keys, key);
            }
            break;
          }
        }
      }
    }
    return keys;
  }
}

bool CryptoBuiltInImpl::create_local_participant_crypto_tokens(
  ParticipantCryptoTokenSeq& local_participant_crypto_tokens,
  ParticipantCryptoHandle local_participant_crypto,
  ParticipantCryptoHandle remote_participant_crypto,
  SecurityException& ex)
{
  if (DDS::HANDLE_NIL == local_participant_crypto) {
    CommonUtilities::set_security_error(ex, -1, 0,
                                        "Invalid local participant handle");
    return false;
  }
  if (DDS::HANDLE_NIL == remote_participant_crypto) {
    CommonUtilities::set_security_error(ex, -1, 0,
                                        "Invalid remote participant handle");
    return false;
  }

  ACE_Guard<ACE_Thread_Mutex> guard(mutex_);
  if (keys_.count(local_participant_crypto)) {
    local_participant_crypto_tokens =
      keys_to_tokens(keys_[local_participant_crypto]);
  } else {
    local_participant_crypto_tokens.length(0);
  }

  return true;
}

bool CryptoBuiltInImpl::set_remote_participant_crypto_tokens(
  ParticipantCryptoHandle local_participant_crypto,
  ParticipantCryptoHandle remote_participant_crypto,
  const ParticipantCryptoTokenSeq& remote_participant_tokens,
  SecurityException& ex)
{
  if (DDS::HANDLE_NIL == local_participant_crypto) {
    CommonUtilities::set_security_error(ex, -1, 0,
                                        "Invalid local participant handle");
    return false;
  }
  if (DDS::HANDLE_NIL == remote_participant_crypto) {
    CommonUtilities::set_security_error(ex, -1, 0,
                                        "Invalid remote participant handle");
    return false;
  }

  ACE_Guard<ACE_Thread_Mutex> guard(mutex_);
  keys_[remote_participant_crypto] = tokens_to_keys(remote_participant_tokens);
  return true;
}

bool CryptoBuiltInImpl::create_local_datawriter_crypto_tokens(
  DatawriterCryptoTokenSeq& local_datawriter_crypto_tokens,
  DatawriterCryptoHandle local_datawriter_crypto,
  DatareaderCryptoHandle remote_datareader_crypto,
  SecurityException& ex)
{
  if (DDS::HANDLE_NIL == local_datawriter_crypto) {
    CommonUtilities::set_security_error(ex, -1, 0, "Invalid local writer handle");
    return false;
  }
  if (DDS::HANDLE_NIL == remote_datareader_crypto) {
    CommonUtilities::set_security_error(ex, -1, 0, "Invalid remote reader handle");
    return false;
  }

  ACE_Guard<ACE_Thread_Mutex> guard(mutex_);
  if (keys_.count(local_datawriter_crypto)) {
    local_datawriter_crypto_tokens =
      keys_to_tokens(keys_[local_datawriter_crypto]);
  } else {
    local_datawriter_crypto_tokens.length(0);
  }

  return true;
}

bool CryptoBuiltInImpl::set_remote_datawriter_crypto_tokens(
  DatareaderCryptoHandle local_datareader_crypto,
  DatawriterCryptoHandle remote_datawriter_crypto,
  const DatawriterCryptoTokenSeq& remote_datawriter_tokens,
  SecurityException& ex)
{
  if (DDS::HANDLE_NIL == local_datareader_crypto) {
    CommonUtilities::set_security_error(ex, -1, 0,
                                        "Invalid local datareader handle");
    return false;
  }
  if (DDS::HANDLE_NIL == remote_datawriter_crypto) {
    CommonUtilities::set_security_error(ex, -1, 0,
                                        "Invalid remote datawriter handle");
    return false;
  }

  ACE_Guard<ACE_Thread_Mutex> guard(mutex_);
  keys_[remote_datawriter_crypto] = tokens_to_keys(remote_datawriter_tokens);
  return true;
}

bool CryptoBuiltInImpl::create_local_datareader_crypto_tokens(
  DatareaderCryptoTokenSeq& local_datareader_crypto_tokens,
  DatareaderCryptoHandle local_datareader_crypto,
  DatawriterCryptoHandle remote_datawriter_crypto,
  SecurityException& ex)
{
  if (DDS::HANDLE_NIL == local_datareader_crypto) {
    CommonUtilities::set_security_error(ex, -1, 0, "Invalid local reader handle");
    return false;
  }
  if (DDS::HANDLE_NIL == remote_datawriter_crypto) {
    CommonUtilities::set_security_error(ex, -1, 0, "Invalid remote writer handle");
    return false;
  }

  ACE_Guard<ACE_Thread_Mutex> guard(mutex_);
  if (keys_.count(local_datareader_crypto)) {
    local_datareader_crypto_tokens =
      keys_to_tokens(keys_[local_datareader_crypto]);
  } else {
    local_datareader_crypto_tokens.length(0);
  }

  return true;
}

bool CryptoBuiltInImpl::set_remote_datareader_crypto_tokens(
  DatawriterCryptoHandle local_datawriter_crypto,
  DatareaderCryptoHandle remote_datareader_crypto,
  const DatareaderCryptoTokenSeq& remote_datareader_tokens,
  SecurityException& ex)
{
  if (DDS::HANDLE_NIL == local_datawriter_crypto) {
    CommonUtilities::set_security_error(ex, -1, 0,
                                        "Invalid local datawriter handle");
    return false;
  }
  if (DDS::HANDLE_NIL == remote_datareader_crypto) {
    CommonUtilities::set_security_error(ex, -1, 0,
                                        "Invalid remote datareader handle");
    return false;
  }

  ACE_Guard<ACE_Thread_Mutex> guard(mutex_);
  keys_[remote_datareader_crypto] = tokens_to_keys(remote_datareader_tokens);
  return true;
}

bool CryptoBuiltInImpl::return_crypto_tokens(
  const CryptoTokenSeq& crypto_tokens,
  SecurityException& ex)
{
  ACE_UNUSED_ARG(crypto_tokens);
  ACE_UNUSED_ARG(ex);

  // The stub implementation will always succeed here
  bool results = true;
  return results;
}


// Transform
namespace {
  bool encrypts(const KeyMaterial_AES_GCM_GMAC& k)
  {
    const CryptoTransformKind& kind = k.transformation_kind;
    return kind[0] == 0 && kind[1] == 0 && kind[2] == 0
      && (kind[TransformKindIndex] == CRYPTO_TRANSFORMATION_KIND_AES128_GCM ||
          kind[TransformKindIndex] == CRYPTO_TRANSFORMATION_KIND_AES256_GCM);
  }

  struct CipherContext {
    EVP_CIPHER_CTX* ctx_;
    CipherContext() : ctx_(EVP_CIPHER_CTX_new()) {}
    operator EVP_CIPHER_CTX*() { return ctx_; }
    ~CipherContext() { EVP_CIPHER_CTX_free(ctx_); }
  };

  bool inc32(unsigned char* a)
  {
    for (int i = 0; i < 4; ++i) {
      if (a[i] != 0xff) {
        ++a[i];
        return false;
      }
    }
    std::fill(a, a + 4, 0);
    return true;
  }
}

bool CryptoBuiltInImpl::encode_serialized_payload(
  DDS::OctetSeq& encoded_buffer,
  DDS::OctetSeq& extra_inline_qos,
  const DDS::OctetSeq& plain_buffer,
  DatawriterCryptoHandle sending_datawriter_crypto,
  SecurityException& ex)
{
  ACE_UNUSED_ARG(extra_inline_qos);

  if (DDS::HANDLE_NIL == sending_datawriter_crypto) {
    CommonUtilities::set_security_error(ex, -1, 0, "Invalid datawriter handle");
    return false;
  }

  // Simple implementation wraps the plain_buffer back into the output
  // and adds no extra_inline_qos
  DDS::OctetSeq transformed_buffer(plain_buffer);
  encoded_buffer.swap(transformed_buffer);
  return true;
}

void CryptoBuiltInImpl::Session::create_key(const KeyMaterial& master)
{
  RAND_bytes(id_, sizeof id_);
  RAND_bytes(iv_suffix_, sizeof iv_suffix_);
  derive_key(master);
  counter_ = 0;
}

void CryptoBuiltInImpl::Session::next_id(const KeyMaterial& master)
{
  inc32(id_);
  RAND_bytes(iv_suffix_, sizeof iv_suffix_);
  key_.length(0);
  derive_key(master);
  counter_ = 0;
}

void CryptoBuiltInImpl::Session::inc_iv()
{
  if (inc32(iv_suffix_)) {
    inc32(iv_suffix_ + 4);
  }
}

bool CryptoBuiltInImpl::encrypt(const KeyMaterial& master, Session& sess,
                                const DDS::OctetSeq& plain,
                                CryptoHeader& header, CryptoFooter& footer,
                                DDS::OctetSeq& out, SecurityException& ex)
{
  const unsigned int blocks =
    (plain.length() + BLOCK_LEN_BYTES - 1) / BLOCK_LEN_BYTES;

  if (!sess.key_.length()) {
    sess.create_key(master);

  } else if (sess.counter_ + blocks > MAX_BLOCKS_PER_SESSION) {
    sess.next_id(master);

  } else {
    sess.inc_iv();
    sess.counter_ += blocks;
  }

  static const int IV_LEN = 12, IV_SUFFIX_IDX = 4;
  unsigned char iv[IV_LEN];
  std::memcpy(iv, &sess.id_, sizeof sess.id_);
  std::memcpy(iv + IV_SUFFIX_IDX, &sess.iv_suffix_, sizeof sess.iv_suffix_);

  std::memcpy(&header.transform_identifier.transformation_kind,
              &master.transformation_kind, sizeof master.transformation_kind);
  std::memcpy(&header.transform_identifier.transformation_key_id,
              &master.sender_key_id, sizeof master.sender_key_id);
  std::memcpy(&header.session_id, &sess.id_, sizeof sess.id_);
  std::memcpy(&header.initialization_vector_suffix, &sess.iv_suffix_,
              sizeof sess.iv_suffix_);

  CipherContext ctx;
  const unsigned char* key = sess.key_.get_buffer();
  if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), 0, key, iv) != 1) {
    CommonUtilities::set_security_error(ex, -1, 0, "EVP_EncryptInit_ex");
    return false;
  }

  int len;
  out.length(plain.length() + BLOCK_LEN_BYTES - 1);
  if (EVP_EncryptUpdate(ctx, out.get_buffer(), &len,
                        plain.get_buffer(), plain.length()) != 1) {
    CommonUtilities::set_security_error(ex, -1, 0, "EVP_EncryptUpdate");
    return false;
  }

  int padLen;
  if (EVP_EncryptFinal_ex(ctx, out.get_buffer() + len, &padLen) != 1) {
    CommonUtilities::set_security_error(ex, -1, 0, "EVP_EncryptFinal_ex");
    return false;
  }

  out.length(len + padLen);

  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, sizeof footer.common_mac,
                          &footer.common_mac) != 1) {
    CommonUtilities::set_security_error(ex, -1, 0, "EVP_CIPHER_CTX_ctrl");
    return false;
  }

  return true;
}

bool CryptoBuiltInImpl::authtag(const KeyMaterial& /*master*/, Session&/*sess*/,
                                const DDS::OctetSeq& /*plain*/,
                                CryptoHeader& /*header*/,
                                CryptoFooter& /*footer*/,
                                SecurityException& /*ex*/)
{
  // not yet implemented
  return false;
}

bool CryptoBuiltInImpl::encode_submessage(
  DDS::OctetSeq& encoded_rtps_submessage,
  const DDS::OctetSeq& plain_rtps_submessage,
  NativeCryptoHandle sender_handle,
  SecurityException& ex)
{
  ACE_Guard<ACE_Thread_Mutex> guard(mutex_);
  if (!keys_.count(sender_handle)) {
    encoded_rtps_submessage = plain_rtps_submessage;
    return true;
  }

  const KeySeq& keyseq = keys_[sender_handle];
  if (!keyseq.length()) {
    encoded_rtps_submessage = plain_rtps_submessage;
    return true;
  }

  bool ok;
  CryptoHeader header;
  CryptoFooter footer;
  DDS::OctetSeq out;
  const DDS::OctetSeq* pOut = &plain_rtps_submessage;
  static const int SUBMSG_KEY_IDX = 0;
  const KeyId_t sKey = std::make_pair(sender_handle, SUBMSG_KEY_IDX);

  if (encrypts(keyseq[SUBMSG_KEY_IDX])) {
    ok = encrypt(keyseq[SUBMSG_KEY_IDX], sessions_[sKey], plain_rtps_submessage,
                 header, footer, out, ex);
    pOut = &out;
  } else {
    ok = authtag(keyseq[SUBMSG_KEY_IDX], sessions_[sKey], plain_rtps_submessage,
                 header, footer, ex);
  }

  if (!ok) {
    return false;
  }

  size_t size = 0, padding = 0;
  size += 4; // prefix submessage header
  using DCPS::gen_find_size;
  gen_find_size(header, size, padding);
  const ACE_UINT16 hdrLen = static_cast<ACE_UINT16>(size + padding - 4);

  if (pOut != &plain_rtps_submessage) {
    size += 8; // body submessage header + seq len
  }

  size += pOut->length(); // submessage inside wrapper
  if ((size + padding) % 4) {
    padding += 4 - ((size + padding) % 4);
  }

  size += 4; // postfix submessage header
  size_t preFooter = size + padding;
  gen_find_size(footer, size, padding);

  encoded_rtps_submessage.length(size + padding);
  ACE_Message_Block mb(to_mb(encoded_rtps_submessage.get_buffer()),
                       size + padding);
  Serializer ser(&mb, Serializer::SWAP_BE, Serializer::ALIGN_CDR);
  RTPS::SubmessageHeader smHdr = {RTPS::SEC_PREFIX, 0, hdrLen};
  ser << smHdr;
  ser << header;
  if (pOut != &plain_rtps_submessage) {
    smHdr.submessageId = RTPS::SEC_BODY;
    smHdr.submessageLength = static_cast<ACE_UINT16>(4 + pOut->length());
    ser << smHdr;
    ser << pOut->length();
  }
  ser.write_octet_array(pOut->get_buffer(), pOut->length());
  smHdr.submessageId = RTPS::SEC_POSTFIX;
  smHdr.submessageLength = static_cast<ACE_UINT16>(size + padding - preFooter);
  ser << smHdr;
  ser << footer;

  return true;
}

bool CryptoBuiltInImpl::encode_datawriter_submessage(
  DDS::OctetSeq& encoded_rtps_submessage,
  const DDS::OctetSeq& plain_rtps_submessage,
  DatawriterCryptoHandle sending_datawriter_crypto,
  const DatareaderCryptoHandleSeq& receiving_datareader_crypto_list,
  CORBA::Long& receiving_datareader_crypto_list_index,
  SecurityException& ex)
{
  if (DDS::HANDLE_NIL == sending_datawriter_crypto) {
    CommonUtilities::set_security_error(ex, -1, 0, "Invalid DataWriter handle");
    return false;
  }

  if (receiving_datareader_crypto_list_index < 0) {
    CommonUtilities::set_security_error(ex, -1, 0, "Negative list index");
    return false;
  }

  const int len = static_cast<int>(receiving_datareader_crypto_list.length());
  // NOTE: as an extension to the spec, this plugin allows an empty list in the
  // case where the writer is sending to all associated readers.
  if (len && receiving_datareader_crypto_list_index >= len) {
    CommonUtilities::set_security_error(ex, -1, 0, "List index too large");
    return false;
  }

  for (unsigned int i = 0; i < receiving_datareader_crypto_list.length(); ++i) {
    if (receiving_datareader_crypto_list[i] == DDS::HANDLE_NIL) {
      CommonUtilities::set_security_error(ex, -1, 0,
                                          "Invalid DataReader handle in list");
      return false;
    }
  }

  NativeCryptoHandle encode_handle = sending_datawriter_crypto;
  if (receiving_datareader_crypto_list.length() == 1) {
    ACE_Guard<ACE_Thread_Mutex> guard(mutex_);
    if (keys_.count(encode_handle)) {
      const KeySeq& dw_keys = keys_[encode_handle];
      if (dw_keys.length() == 1 && is_volatile_placeholder(dw_keys[0])) {
        encode_handle = receiving_datareader_crypto_list[0];
      }
    }
  }

  const bool ok = encode_submessage(encoded_rtps_submessage,
                                    plain_rtps_submessage, encode_handle, ex);
  if (ok) {
    receiving_datareader_crypto_list_index = len;
  }
  return ok;
}

bool CryptoBuiltInImpl::encode_datareader_submessage(
  DDS::OctetSeq& encoded_rtps_submessage,
  const DDS::OctetSeq& plain_rtps_submessage,
  DatareaderCryptoHandle sending_datareader_crypto,
  const DatawriterCryptoHandleSeq& receiving_datawriter_crypto_list,
  SecurityException& ex)
{
  if (DDS::HANDLE_NIL == sending_datareader_crypto) {
    CommonUtilities::set_security_error(ex, -1, 0, "Invalid DataReader handle");
    return false;
  }

  for (unsigned int i = 0; i < receiving_datawriter_crypto_list.length(); ++i) {
    if (receiving_datawriter_crypto_list[i] == DDS::HANDLE_NIL) {
      CommonUtilities::set_security_error(ex, -1, 0,
                                          "Invalid DataWriter handle in list");
      return false;
    }
  }

  NativeCryptoHandle encode_handle = sending_datareader_crypto;
  if (receiving_datawriter_crypto_list.length() == 1) {
    ACE_Guard<ACE_Thread_Mutex> guard(mutex_);
    if (keys_.count(encode_handle)) {
      const KeySeq& dr_keys = keys_[encode_handle];
      if (dr_keys.length() == 1 && is_volatile_placeholder(dr_keys[0])) {
        encode_handle = receiving_datawriter_crypto_list[0];
      }
    }
  }

  return encode_submessage(encoded_rtps_submessage, plain_rtps_submessage,
                           encode_handle, ex);
}

bool CryptoBuiltInImpl::encode_rtps_message(
  DDS::OctetSeq& encoded_rtps_message,
  const DDS::OctetSeq& plain_rtps_message,
  ParticipantCryptoHandle sending_participant_crypto,
  const ParticipantCryptoHandleSeq& receiving_participant_crypto_list,
  CORBA::Long& receiving_participant_crypto_list_index,
  SecurityException& ex)
{
  // Perform sanity checking on input data
  if (DDS::HANDLE_NIL == sending_participant_crypto) {
    CommonUtilities::set_security_error(ex, -1, 0, "Invalid DataReader handle");
    return false;
  }
  if (0 == receiving_participant_crypto_list.length()) {
    CommonUtilities::set_security_error(ex, -1, 0, "No Datawriters specified");
    return false;
  }

  ParticipantCryptoHandle dest_handle = DDS::HANDLE_NIL;
  if (receiving_participant_crypto_list_index >= 0) {
    // Need to make this unsigned to get prevent warnings when comparing to length()
    CORBA::ULong index = static_cast<CORBA::ULong>(receiving_participant_crypto_list_index);
    if (index < receiving_participant_crypto_list.length()) {
      dest_handle = receiving_participant_crypto_list[receiving_participant_crypto_list_index];
    }
  }

  if (DDS::HANDLE_NIL == dest_handle) {
    CommonUtilities::set_security_error(ex, -1, 0, "Invalid receiver handle");
    return false;
  }

  // Simple implementation wraps the plain_buffer back into the output
  // and adds no extra_inline_qos
  DDS::OctetSeq transformed_buffer(plain_rtps_message);
  encoded_rtps_message.swap(transformed_buffer);

  // Advance the counter to indicate this reader has been handled
  ++receiving_participant_crypto_list_index;

  return true;
}

namespace {
  bool matches(const KeyMaterial_AES_GCM_GMAC& k, const CryptoHeader& h)
  {
    return 0 == std::memcmp(k.transformation_kind,
                            h.transform_identifier.transformation_kind,
                            sizeof(CryptoTransformKind))
      && 0 == std::memcmp(k.sender_key_id,
                          h.transform_identifier.transformation_key_id,
                          sizeof(CryptoTransformKeyId));
  }
}

bool CryptoBuiltInImpl::decode_rtps_message(
  DDS::OctetSeq& plain_buffer,
  const DDS::OctetSeq& encoded_buffer,
  ParticipantCryptoHandle receiving_participant_crypto,
  ParticipantCryptoHandle sending_participant_crypto,
  SecurityException& ex)
{
  // Perform sanity checking on input data
  if (DDS::HANDLE_NIL == receiving_participant_crypto) {
    CommonUtilities::set_security_error(ex, -1, 0, "Invalid Receiving Participant handle");
    return false;
  }
  if (DDS::HANDLE_NIL == sending_participant_crypto) {
    CommonUtilities::set_security_error(ex, -1, 0, "No Sending Participant handle");
    return false;
  }

  // For the stub, just supply the input as the output
  DDS::OctetSeq transformed_buffer(encoded_buffer);
  plain_buffer.swap(transformed_buffer);

  return true;
}

bool CryptoBuiltInImpl::preprocess_secure_submsg(
  DatawriterCryptoHandle& datawriter_crypto,
  DatareaderCryptoHandle& datareader_crypto,
  SecureSubmessageCategory_t& secure_submessage_category,
  const DDS::OctetSeq& encoded_rtps_submessage,
  ParticipantCryptoHandle receiving_participant_crypto,
  ParticipantCryptoHandle sending_participant_crypto,
  SecurityException& ex)
{
  if (DDS::HANDLE_NIL == receiving_participant_crypto) {
    CommonUtilities::set_security_error(ex, -1, 0, "Invalid Receiving Participant");
    return false;
  }
  if (DDS::HANDLE_NIL == sending_participant_crypto) {
    CommonUtilities::set_security_error(ex, -1, 0, "Invalid Sending Participant");
    return false;
  }

  ACE_Message_Block mb_in(to_mb(encoded_rtps_submessage.get_buffer()),
                          encoded_rtps_submessage.length());
  mb_in.wr_ptr(encoded_rtps_submessage.length());
  Serializer de_ser(&mb_in, false, Serializer::ALIGN_CDR);
  ACE_CDR::Octet type, flags;
  de_ser >> ACE_InputCDR::to_octet(type);
  de_ser >> ACE_InputCDR::to_octet(flags);
  de_ser.swap_bytes((flags & 1) != ACE_CDR_BYTE_ORDER);
  ACE_CDR::UShort octetsToNext;
  de_ser >> octetsToNext;
  CryptoHeader ch;
  de_ser.swap_bytes(Serializer::SWAP_BE);
  de_ser >> ch;

  ACE_Guard<ACE_Thread_Mutex> guard(mutex_);
  typedef std::multimap<ParticipantCryptoHandle, EntityInfo>::iterator iter_t;
  const std::pair<iter_t, iter_t> iters =
    participant_to_entity_.equal_range(sending_participant_crypto);
  for (iter_t iter = iters.first; iter != iters.second; ++iter) {
    const NativeCryptoHandle sending_entity_candidate = iter->second.handle_;
    if (keys_.count(sending_entity_candidate)) {
      const KeySeq& keyseq = keys_[sending_entity_candidate];
      for (unsigned int i = 0; i < keyseq.length(); ++i) {
        if (matches(keyseq[i], ch)) {
          secure_submessage_category = iter->second.category_;
          switch (secure_submessage_category) {
          case DATAWRITER_SUBMESSAGE:
            datawriter_crypto = iter->second.handle_;
            break;
          case DATAREADER_SUBMESSAGE:
            datareader_crypto = iter->second.handle_;
            break;
          default:
            break;
          }
          return true;
        }
      }
    }
  }
  CommonUtilities::set_security_error(ex, -2, 1, "Crypto Key not registered",
                                      ch.transform_identifier.transformation_kind,
                                      ch.transform_identifier.transformation_key_id);
  return false;
}

KeyOctetSeq
CryptoBuiltInImpl::Session::get_key(const KeyMaterial& master,
                                    const CryptoHeader& header)
{
  if (key_.length() && 0 == std::memcmp(&id_, &header.session_id, sizeof id_)) {
    return key_;
  }
  std::memcpy(&id_, &header.session_id, sizeof id_);
  key_.length(0);
  derive_key(master);
  return key_;
}

void CryptoBuiltInImpl::Session::derive_key(const KeyMaterial& master)
{
  PrivateKey pkey(master.master_sender_key);
  DigestContext ctx;
  const EVP_MD* md = EVP_get_digestbyname("SHA256");

  if (EVP_DigestInit_ex(ctx, md, 0) < 1) {
    return;
  }

  if (EVP_DigestSignInit(ctx, 0, md, 0, pkey) < 1) {
    return;
  }

  static const char cookie[] = "SessionKey"; // DDSSEC12-53
  if (EVP_DigestSignUpdate(ctx, cookie, std::strlen(cookie)) < 1) {
    return;
  }

  const KeyOctetSeq& salt = master.master_salt;
  if (EVP_DigestSignUpdate(ctx, salt.get_buffer(), salt.length()) < 1) {
    return;
  }

  if (EVP_DigestSignUpdate(ctx, id_, sizeof id_) < 1) {
    return;
  }

  size_t req = 0;
  if (EVP_DigestSignFinal(ctx, 0, &req) < 1) {
    return;
  }

  key_.length(static_cast<unsigned int>(req));
  if (EVP_DigestSignFinal(ctx, key_.get_buffer(), &req) < 1) {
    key_.length(0);
  }
}

bool CryptoBuiltInImpl::decrypt(const KeyMaterial& master, Session& sess,
                                const char* ciphertext, unsigned int n,
                                const CryptoHeader& header,
                                const CryptoFooter& footer, DDS::OctetSeq& out,
                                SecurityException& ex)

{
  const KeyOctetSeq sess_key = sess.get_key(master, header);
  if (!sess_key.length()) {
    CommonUtilities::set_security_error(ex, -1, 0, "no session key");
    return false;
  }

  if (master.transformation_kind[TransformKindIndex] !=
      CRYPTO_TRANSFORMATION_KIND_AES256_GCM) {
    CommonUtilities::set_security_error(ex, -1, 0,
                                        "unsupported transformation kind");
    ACE_ERROR((LM_ERROR, "(%P|%t) CryptoBuiltInImpl::decrypt - ERROR "
               "unsupported transformation kind %d\n",
               master.transformation_kind[TransformKindIndex]));
    return false;
  }

  CipherContext ctx;
  // session_id is start of IV contiguous bytes
  if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), 0, sess_key.get_buffer(),
                         header.session_id) != 1) {
    CommonUtilities::set_security_error(ex, -1, 0, "EVP_DecryptInit_ex");
    ACE_ERROR((LM_ERROR, "(%P|%t) CryptoBuiltInImpl::decrypt - ERROR "
               "EVP_DecryptInit_ex %Ld\n", ERR_peek_last_error()));
    return false;
  }

  out.length(n + KEY_LEN_BYTES);
  int len;
  if (EVP_DecryptUpdate(ctx, out.get_buffer(), &len,
                        reinterpret_cast<const unsigned char*>(ciphertext), n)
      != 1) {
    CommonUtilities::set_security_error(ex, -1, 0, "EVP_DecryptUpdate");
    ACE_ERROR((LM_ERROR, "(%P|%t) CryptoBuiltInImpl::decrypt - ERROR "
               "EVP_DecryptUpdate %Ld\n", ERR_peek_last_error()));
    return false;
  }

  void* tag = const_cast<void*>(static_cast<const void*>(footer.common_mac));
  if (!EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag)) {
    CommonUtilities::set_security_error(ex, -1, 0, "EVP_CIPHER_CTX_ctrl");
    ACE_ERROR((LM_ERROR, "(%P|%t) CryptoBuiltInImpl::decrypt - ERROR "
               "EVP_CIPHER_CTX_ctrl %Ld\n", ERR_peek_last_error()));
    return false;
  }

  int len2;
  if (EVP_DecryptFinal_ex(ctx, out.get_buffer() + len, &len2) == 1) {
    out.length(len + len2);
    return true;
  }
  CommonUtilities::set_security_error(ex, -1, 0, "EVP_DecryptFinal_ex");
  ACE_ERROR((LM_ERROR, "(%P|%t) CryptoBuiltInImpl::decrypt - ERROR "
             "EVP_DecryptFinal_ex %Ld\n", ERR_peek_last_error()));
  return false;
}

bool CryptoBuiltInImpl::verify(const KeyMaterial&, const char* ciphertext,
                               unsigned int n, const CryptoHeader& /*header*/,
                               const CryptoFooter&, DDS::OctetSeq& out,
                               SecurityException& /*ex*/)

{
  //TODO: verification of authenticated but unencrypted message
  out.length(n);
  std::memcpy(out.get_buffer(), ciphertext, n);
  return true;
}

bool CryptoBuiltInImpl::decode_submessage(
  DDS::OctetSeq& plain_rtps_submessage,
  const DDS::OctetSeq& encoded_rtps_submessage,
  NativeCryptoHandle sender_handle,
  SecurityException& ex)
{
  ACE_Message_Block mb_in(to_mb(encoded_rtps_submessage.get_buffer()),
                          encoded_rtps_submessage.length());
  mb_in.wr_ptr(encoded_rtps_submessage.length());
  Serializer de_ser(&mb_in, false, Serializer::ALIGN_CDR);
  ACE_CDR::Octet type, flags;
  // SEC_PREFIX
  de_ser >> ACE_InputCDR::to_octet(type);
  de_ser >> ACE_InputCDR::to_octet(flags);
  de_ser.swap_bytes((flags & 1) != ACE_CDR_BYTE_ORDER);
  ACE_CDR::UShort octetsToNext;
  de_ser >> octetsToNext;
  CryptoHeader ch;
  de_ser.swap_bytes(Serializer::SWAP_BE);
  de_ser >> ch;
  de_ser.skip(octetsToNext - 20);
  // Next submessage, SEC_BODY if encrypted
  de_ser >> ACE_InputCDR::to_octet(type);
  de_ser >> ACE_InputCDR::to_octet(flags);
  de_ser.swap_bytes((flags & 1) != ACE_CDR_BYTE_ORDER);
  de_ser >> octetsToNext;
  Message_Block_Ptr mb_footer(mb_in.duplicate());
  mb_footer->rd_ptr(octetsToNext);
  // SEC_POSTFIX
  Serializer post_ser(mb_footer.get(), false, Serializer::ALIGN_CDR);
  post_ser >> ACE_InputCDR::to_octet(type);
  post_ser >> ACE_InputCDR::to_octet(flags);
  post_ser.swap_bytes((flags & 1) != ACE_CDR_BYTE_ORDER);
  post_ser >> octetsToNext;
  CryptoFooter cf;
  post_ser.swap_bytes(Serializer::SWAP_BE);
  post_ser >> cf;

  ACE_Guard<ACE_Thread_Mutex> guard(mutex_);
  const KeySeq& keyseq = keys_[sender_handle];
  for (unsigned int i = 0; i < keyseq.length(); ++i) {
    if (matches(keyseq[i], ch)) {
      if (encrypts(keyseq[i])) {
        de_ser.swap_bytes(Serializer::SWAP_BE);
        ACE_CDR::ULong n;
        de_ser >> n;
        const KeyId_t sKey = std::make_pair(sender_handle, i);
        return decrypt(keyseq[i], sessions_[sKey], mb_in.rd_ptr(), n, ch, cf,
                       plain_rtps_submessage, ex);
      } else {
        return verify(keyseq[i], mb_in.rd_ptr() - 4, 4 + octetsToNext,
                      ch, cf, plain_rtps_submessage, ex);
      }
    }
  }

  CommonUtilities::set_security_error(ex, -2, 1, "Crypto Key not found");
  return false;
}

bool CryptoBuiltInImpl::decode_datawriter_submessage(
  DDS::OctetSeq& plain_rtps_submessage,
  const DDS::OctetSeq& encoded_rtps_submessage,
  DatareaderCryptoHandle /*receiving_datareader_crypto*/,
  DatawriterCryptoHandle sending_datawriter_crypto,
  SecurityException& ex)
{
  // Allowing Nil Handle for receiver since origin auth is not implemented:
  //  if (DDS::HANDLE_NIL == receiving_datareader_crypto) {
  //CommonUtilities::set_security_error(ex, -1, 0, "Invalid Datareader handle");
  //    return false;
  //  }
  if (DDS::HANDLE_NIL == sending_datawriter_crypto) {
    CommonUtilities::set_security_error(ex, -1, 0, "Invalid Datawriter handle");
    return false;
  }

  return decode_submessage(plain_rtps_submessage, encoded_rtps_submessage,
                           sending_datawriter_crypto, ex);
}

bool CryptoBuiltInImpl::decode_datareader_submessage(
  DDS::OctetSeq& plain_rtps_submessage,
  const DDS::OctetSeq& encoded_rtps_submessage,
  DatawriterCryptoHandle /*receiving_datawriter_crypto*/,
  DatareaderCryptoHandle sending_datareader_crypto,
  SecurityException& ex)
{
  if (DDS::HANDLE_NIL == sending_datareader_crypto) {
    CommonUtilities::set_security_error(ex, -1, 0, "Invalid Datareader handle");
    return false;
  }
  // Allowing Nil Handle for receiver since origin auth is not implemented:
  //  if (DDS::HANDLE_NIL == receiving_datawriter_crypto) {
  //CommonUtilities::set_security_error(ex, -1, 0, "Invalid Datawriter handle");
  //    return false;
  //  }

  return decode_submessage(plain_rtps_submessage, encoded_rtps_submessage,
                           sending_datareader_crypto, ex);
}

bool CryptoBuiltInImpl::decode_serialized_payload(
  DDS::OctetSeq& plain_buffer,
  const DDS::OctetSeq& encoded_buffer,
  const DDS::OctetSeq& /*inline_qos*/,
  DatareaderCryptoHandle /*receiving_datareader_crypto*/,
  DatawriterCryptoHandle sending_datawriter_crypto,
  SecurityException& ex)
{
  // Not currently requring a reader handle here, origin authentication
  // for data payloads is not supported.
  // if (DDS::HANDLE_NIL == receiving_datareader_crypto) {
  //   CommonUtilities::set_security_error(ex, -1, 0, "Invalid Datareader handle");
  //   return false;
  // }
  if (DDS::HANDLE_NIL == sending_datawriter_crypto) {
    CommonUtilities::set_security_error(ex, -1, 0, "Invalid Datawriter handle");
    return false;
  }

  // For the stub, just supply the input as the output
  DDS::OctetSeq transformed_buffer(encoded_buffer);
  plain_buffer.swap(transformed_buffer);

  return true;
}

}
}