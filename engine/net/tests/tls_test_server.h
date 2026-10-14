// A TLS server for one loopback connection, for the HTTPS tests (ADR 0063).
//
// **Its certificate is made when the test runs**, never committed: a private
// key in a public repository is a key somebody will find. It is self-signed for
// `localhost` and `127.0.0.1`, which is the point -- no device trusts it, so a
// client that verifies refuses it.
//
// - **Windows**: a key in the user's key store under a name of its own, deleted
//   again, and a self-signed certificate over it; the server is Schannel. Only
//   the handshake is served: nothing a test can do on Windows makes the client
//   trust the certificate (adding a root asks the person at the machine), so
//   the one Windows case is the refusal.
// - **Linux**: `openssl req` writes the key and the certificate, and the server
//   is the same system OpenSSL the client loads. A test can trust the
//   certificate there (`testing::trustExtraRoot`), so the accepted request is
//   tested too.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "loopback_server.h"

#if defined(_WIN32)
#define SECURITY_WIN32
#include <ncrypt.h>
#include <schannel.h>
#include <security.h>
#include <vector>
#include <wincrypt.h>
#include <windows.h>
#else
#include <cstdlib>
#include <dlfcn.h>
#include <fstream>
#endif

namespace engine::net::testing {

#if defined(_WIN32)

class TlsIdentity
{
public:
    TlsIdentity()
    {
        static int counter = 0;
        m_keyName = L"engine-test-tls-" + std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(++counter);

        NCRYPT_PROV_HANDLE provider = 0;
        NCRYPT_KEY_HANDLE key = 0;
        if (::NCryptOpenStorageProvider(&provider, MS_KEY_STORAGE_PROVIDER, 0) != ERROR_SUCCESS)
            return;
        DWORD bits = 2048;
        const bool made = ::NCryptCreatePersistedKey(provider, &key, BCRYPT_RSA_ALGORITHM, m_keyName.c_str(), 0,
                                                     NCRYPT_OVERWRITE_KEY_FLAG) == ERROR_SUCCESS &&
                          ::NCryptSetProperty(key, NCRYPT_LENGTH_PROPERTY, reinterpret_cast<PBYTE>(&bits), sizeof(bits),
                                              0) == ERROR_SUCCESS &&
                          ::NCryptFinalizeKey(key, 0) == ERROR_SUCCESS;
        if (made) {
            BYTE encoded[256];
            DWORD encodedSize = sizeof(encoded);
            if (::CertStrToNameW(X509_ASN_ENCODING, L"CN=localhost", CERT_X500_NAME_STR, nullptr, encoded, &encodedSize,
                                 nullptr)) {
                CERT_NAME_BLOB subject{encodedSize, encoded};
                CRYPT_KEY_PROV_INFO info{};
                info.pwszContainerName = m_keyName.data();
                info.pwszProvName = const_cast<LPWSTR>(MS_KEY_STORAGE_PROVIDER);
                CRYPT_ALGORITHM_IDENTIFIER signature{};
                signature.pszObjId = const_cast<LPSTR>(szOID_RSA_SHA256RSA);
                SYSTEMTIME expires{};
                ::GetSystemTime(&expires);
                expires.wYear = static_cast<WORD>(expires.wYear + 1);
                m_certificate =
                    ::CertCreateSelfSignCertificate(key, &subject, 0, &info, &signature, nullptr, &expires, nullptr);
            }
        }
        if (key != 0)
            ::NCryptFreeObject(key);
        ::NCryptFreeObject(provider);
    }

    ~TlsIdentity()
    {
        if (m_certificate != nullptr)
            ::CertFreeCertificateContext(m_certificate);
        NCRYPT_PROV_HANDLE provider = 0;
        NCRYPT_KEY_HANDLE key = 0;
        if (::NCryptOpenStorageProvider(&provider, MS_KEY_STORAGE_PROVIDER, 0) == ERROR_SUCCESS) {
            if (::NCryptOpenKey(provider, &key, m_keyName.c_str(), 0, 0) == ERROR_SUCCESS)
                ::NCryptDeleteKey(key, 0);
            ::NCryptFreeObject(provider);
        }
    }

    TlsIdentity(const TlsIdentity&) = delete;
    TlsIdentity& operator=(const TlsIdentity&) = delete;

    [[nodiscard]] bool ok() const noexcept { return m_certificate != nullptr; }
    [[nodiscard]] PCCERT_CONTEXT certificate() const noexcept { return m_certificate; }

private:
    std::wstring m_keyName;
    PCCERT_CONTEXT m_certificate = nullptr;
};

// The server's half of the handshake. True when it completed; false when the
// client gave up, which is what a client that refuses the certificate does.
// No request is read: see the file's opening.
[[nodiscard]] inline bool serveTls(const TlsIdentity& identity, RawSocket socket, std::string_view /*reply*/,
                                   std::string* /*request*/)
{
    PCCERT_CONTEXT certificate = identity.certificate();
    SCHANNEL_CRED settings{};
    settings.dwVersion = SCHANNEL_CRED_VERSION;
    settings.cCreds = 1;
    settings.paCred = &certificate;
    settings.grbitEnabledProtocols = SP_PROT_TLS1_2_SERVER;
    CredHandle credentials{};
    TimeStamp expiry{};
    if (::AcquireCredentialsHandleW(nullptr, const_cast<LPWSTR>(UNISP_NAME_W), SECPKG_CRED_INBOUND, nullptr, &settings,
                                    nullptr, nullptr, &credentials, &expiry) != SEC_E_OK)
        return false;

    CtxtHandle context{};
    bool haveContext = false;
    bool done = false;
    bool needMore = true;
    std::vector<char> incoming;
    while (!done) {
        if (needMore) {
            char chunk[4096];
            const int got = ::recv(socket, chunk, static_cast<int>(sizeof(chunk)), 0);
            if (got <= 0)
                break;
            incoming.insert(incoming.end(), chunk, chunk + got);
        }
        SecBuffer in[2]{};
        in[0] = {static_cast<unsigned long>(incoming.size()), SECBUFFER_TOKEN, incoming.data()};
        in[1] = {0, SECBUFFER_EMPTY, nullptr};
        SecBufferDesc inDesc{SECBUFFER_VERSION, 2, in};
        SecBuffer out[1]{};
        out[0] = {0, SECBUFFER_TOKEN, nullptr};
        SecBufferDesc outDesc{SECBUFFER_VERSION, 1, out};
        ULONG attributes = 0;
        const SECURITY_STATUS status =
            ::AcceptSecurityContext(&credentials, haveContext ? &context : nullptr, &inDesc,
                                    ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM | ASC_REQ_CONFIDENTIALITY, 0,
                                    haveContext ? nullptr : &context, &outDesc, &attributes, nullptr);
        if (out[0].cbBuffer > 0 && out[0].pvBuffer != nullptr) {
            (void)::send(socket, static_cast<const char*>(out[0].pvBuffer), static_cast<int>(out[0].cbBuffer), 0);
            ::FreeContextBuffer(out[0].pvBuffer);
        }
        if (status == SEC_E_INCOMPLETE_MESSAGE) {
            needMore = true;
            continue;
        }
        if (status != SEC_E_OK && status != SEC_I_CONTINUE_NEEDED)
            break;
        haveContext = true;
        // What the token did not use is the start of the next one.
        if (in[1].BufferType == SECBUFFER_EXTRA && in[1].cbBuffer > 0) {
            incoming.erase(incoming.begin(), incoming.end() - static_cast<std::ptrdiff_t>(in[1].cbBuffer));
            needMore = false;
        }
        else {
            incoming.clear();
            needMore = true;
        }
        done = status == SEC_E_OK;
    }
    if (haveContext)
        ::DeleteSecurityContext(&context);
    ::FreeCredentialsHandle(&credentials);
    return done;
}

#else

class TlsIdentity
{
public:
    TlsIdentity()
    {
        static int counter = 0;
        m_directory = std::filesystem::temp_directory_path() /
                      ("engine-tls-test-" + std::to_string(::getpid()) + "-" + std::to_string(++counter));
        std::filesystem::create_directories(m_directory);
        const std::string command = "openssl req -x509 -newkey rsa:2048 -nodes -keyout '" + keyFile() + "' -out '" +
                                    certificateFile() +
                                    "' -days 2 -subj /CN=localhost"
                                    " -addext subjectAltName=DNS:localhost,IP:127.0.0.1 >/dev/null 2>&1";
        m_ok = std::system(command.c_str()) == 0 && std::filesystem::exists(certificateFile());
    }

    ~TlsIdentity()
    {
        std::error_code ignored;
        std::filesystem::remove_all(m_directory, ignored);
    }

    TlsIdentity(const TlsIdentity&) = delete;
    TlsIdentity& operator=(const TlsIdentity&) = delete;

    [[nodiscard]] bool ok() const noexcept { return m_ok; }
    [[nodiscard]] std::string certificateFile() const { return (m_directory / "certificate.pem").string(); }
    [[nodiscard]] std::string keyFile() const { return (m_directory / "key.pem").string(); }

private:
    std::filesystem::path m_directory;
    bool m_ok = false;
};

// The server's half: the handshake, then one request read to its blank line
// (into `request`) and `reply` written. True when the handshake completed.
[[nodiscard]] inline bool serveTls(const TlsIdentity& identity, RawSocket socket, std::string_view reply,
                                   std::string* request)
{
    void* library = ::dlopen("libssl.so.3", RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr)
        return false;
    const auto symbol = [library](const char* name) { return ::dlsym(library, name); };
    const auto serverMethod = reinterpret_cast<const void* (*)()>(symbol("TLS_server_method"));
    const auto ctxNew = reinterpret_cast<void* (*)(const void*)>(symbol("SSL_CTX_new"));
    const auto ctxFree = reinterpret_cast<void (*)(void*)>(symbol("SSL_CTX_free"));
    const auto useCertificate =
        reinterpret_cast<int (*)(void*, const char*, int)>(symbol("SSL_CTX_use_certificate_file"));
    const auto useKey = reinterpret_cast<int (*)(void*, const char*, int)>(symbol("SSL_CTX_use_PrivateKey_file"));
    const auto sslNew = reinterpret_cast<void* (*)(void*)>(symbol("SSL_new"));
    const auto sslFree = reinterpret_cast<void (*)(void*)>(symbol("SSL_free"));
    const auto setFd = reinterpret_cast<int (*)(void*, int)>(symbol("SSL_set_fd"));
    const auto accept = reinterpret_cast<int (*)(void*)>(symbol("SSL_accept"));
    const auto read = reinterpret_cast<int (*)(void*, void*, int)>(symbol("SSL_read"));
    const auto write = reinterpret_cast<int (*)(void*, const void*, int)>(symbol("SSL_write"));
    const auto shutdown = reinterpret_cast<int (*)(void*)>(symbol("SSL_shutdown"));
    if (serverMethod == nullptr || ctxNew == nullptr || useCertificate == nullptr || accept == nullptr)
        return false;

    constexpr int PemFile = 1;
    void* context = ctxNew(serverMethod());
    bool done = false;
    if (context != nullptr && useCertificate(context, identity.certificateFile().c_str(), PemFile) == 1 &&
        useKey(context, identity.keyFile().c_str(), PemFile) == 1) {
        void* connection = sslNew(context);
        if (connection != nullptr && setFd(connection, socket) == 1 && accept(connection) == 1) {
            done = true;
            std::string received;
            char chunk[2048];
            while (received.find("\r\n\r\n") == std::string::npos) {
                const int got = read(connection, chunk, static_cast<int>(sizeof(chunk)));
                if (got <= 0)
                    break;
                received.append(chunk, static_cast<std::size_t>(got));
            }
            if (request != nullptr)
                *request = received;
            (void)write(connection, reply.data(), static_cast<int>(reply.size()));
            (void)shutdown(connection);
        }
        if (connection != nullptr)
            sslFree(connection);
    }
    if (context != nullptr)
        ctxFree(context);
    return done;
}

#endif

} // namespace engine::net::testing
