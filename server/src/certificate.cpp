#include "certificate.h"

#include <windows.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <cstdio>
#include <io.h>
#include <sys/stat.h>
#include <vector>

namespace {

std::string ExecutableDirectory() {
    std::vector<char> path(32768);
    const DWORD length = GetModuleFileNameA(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return ".";
    }
    std::string result(path.data(), length);
    const size_t separator = result.find_last_of("\\/");
    return separator == std::string::npos ? "." : result.substr(0, separator);
}

bool FileExists(const std::string& path) {
    return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool CertificateIsUsable(const std::string& path) {
    FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), "rb") != 0 || !file) return false;
    X509* certificate = PEM_read_X509(file, nullptr, nullptr, nullptr);
    fclose(file);
    if (!certificate) return false;
    const bool usable = X509_cmp_current_time(X509_get0_notBefore(certificate)) <= 0 &&
        X509_cmp_current_time(X509_get0_notAfter(certificate)) >= 0;
    X509_free(certificate);
    return usable;
}

bool GenerateCertificate(const std::string& cert_path, const std::string& key_path, std::string& error) {
    EVP_PKEY_CTX* key_context = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    EVP_PKEY* key = nullptr;
    X509* certificate = nullptr;
    BIGNUM* serial = nullptr;
    FILE* cert_file = nullptr;
    FILE* key_file = nullptr;
    bool success = false;

    if (!key_context || EVP_PKEY_keygen_init(key_context) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(key_context, 2048) <= 0 ||
        EVP_PKEY_keygen(key_context, &key) <= 0 || !key) {
        error = "OpenSSL could not generate a 2048-bit RSA key";
        goto cleanup;
    }

    certificate = X509_new();
    serial = BN_new();
    if (!certificate || !serial || BN_rand(serial, 64, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY) != 1 ||
        !BN_to_ASN1_INTEGER(serial, X509_get_serialNumber(certificate))) {
        error = "OpenSSL could not initialize the certificate";
        goto cleanup;
    }

    X509_set_version(certificate, 2);
    X509_gmtime_adj(X509_getm_notBefore(certificate), -60);
    X509_gmtime_adj(X509_getm_notAfter(certificate), 30L * 24 * 60 * 60);
    X509_set_pubkey(certificate, key);

    {
        X509_NAME* name = const_cast<X509_NAME*>(X509_get_subject_name(certificate));
        if (!name || X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                reinterpret_cast<const unsigned char*>("Local Remote Desktop"), -1, -1, 0) != 1 ||
            X509_set_issuer_name(certificate, name) != 1 ||
            X509_sign(certificate, key, EVP_sha256()) == 0) {
            error = "OpenSSL could not sign the certificate";
            goto cleanup;
        }
    }

    if (fopen_s(&key_file, key_path.c_str(), "wb") != 0 || !key_file ||
        PEM_write_PrivateKey(key_file, key, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
        error = "Could not write the runtime private key";
        goto cleanup;
    }
    fclose(key_file);
    key_file = nullptr;
    _chmod(key_path.c_str(), _S_IREAD | _S_IWRITE);

    if (fopen_s(&cert_file, cert_path.c_str(), "wb") != 0 || !cert_file ||
        PEM_write_X509(cert_file, certificate) != 1) {
        error = "Could not write the runtime certificate";
        goto cleanup;
    }

    success = true;

cleanup:
    if (cert_file) fclose(cert_file);
    if (key_file) fclose(key_file);
    if (certificate) X509_free(certificate);
    if (serial) BN_free(serial);
    if (key) EVP_PKEY_free(key);
    if (key_context) EVP_PKEY_CTX_free(key_context);
    return success;
}

} // namespace

bool EnsureCertificate(std::string& cert_path, std::string& key_path, std::string& error) {
    const std::string runtime_directory = ExecutableDirectory() + "\\runtime";
    if (!CreateDirectoryA(runtime_directory.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        error = "Could not create the runtime data directory";
        return false;
    }

    cert_path = runtime_directory + "\\server_cert.pem";
    key_path = runtime_directory + "\\server_key.pem";
    if (FileExists(cert_path) && FileExists(key_path) && CertificateIsUsable(cert_path)) {
        return true;
    }
    return GenerateCertificate(cert_path, key_path, error);
}
