// `https://` on Android: the Java platform's own HTTPS stack (ADR 0063, an
// amendment: the ADR predates the platform).
//
// **Through the framework, not the NDK**: the NDK offers no TLS, and the
// BoringSSL inside the system is a private library an app may not load. What
// every app is given is `java.net.HttpURLConnection`, which verifies against
// the device's trust store -- including the roots a user or an administrator
// added -- and keeps connections to a host alive between requests. Only
// framework classes are named, so the calling thread's class loader finds them
// whichever thread asks. Redirects are turned off here because `performHttp`
// follows them itself, by one rule for every platform.

#include <cctype>
#include <jni.h>
#include <string>

#include "engine/core/i18n.h"
#include "engine/platform/platform.h"
#include "http_internal.h"

namespace engine::net {
namespace {

using core::I18nArg;

// The exception a call raised, cleared, as its class name; empty for none.
[[nodiscard]] std::string takeException(JNIEnv* env)
{
    if (!env->ExceptionCheck())
        return {};
    jthrowable thrown = env->ExceptionOccurred();
    env->ExceptionClear();
    jclass objectClass = env->FindClass("java/lang/Object");
    jmethodID getClass = env->GetMethodID(objectClass, "getClass", "()Ljava/lang/Class;");
    jobject type = env->CallObjectMethod(thrown, getClass);
    jclass classClass = env->FindClass("java/lang/Class");
    jmethodID getName = env->GetMethodID(classClass, "getName", "()Ljava/lang/String;");
    auto name = static_cast<jstring>(env->CallObjectMethod(type, getName));
    env->ExceptionClear();
    std::string out = "java.lang.Throwable";
    if (name != nullptr) {
        const char* text = env->GetStringUTFChars(name, nullptr);
        out = text;
        env->ReleaseStringUTFChars(name, text);
    }
    return out;
}

[[nodiscard]] core::EngineError failure(const std::string& exception, const std::string& url)
{
    // A certificate the device does not trust arrives as a handshake failure
    // whose cause is a path that did not validate.
    if (exception.find("SSLHandshakeException") != std::string::npos ||
        exception.find("CertPathValidatorException") != std::string::npos ||
        exception.find("SSLPeerUnverifiedException") != std::string::npos) {
        const I18nArg args[] = {{"url", url}};
        return core::makeError(ENG_TR("net.err.https_untrusted"), args);
    }
    if (exception.find("SocketTimeoutException") != std::string::npos)
        return core::makeError(ENG_TR("net.err.http_response_timeout"));
    const I18nArg args[] = {{"url", url}, {"reason", exception}};
    return core::makeError(ENG_TR("net.err.https_failed_named"), args);
}

[[nodiscard]] std::string text(JNIEnv* env, jstring value)
{
    if (value == nullptr)
        return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string out = chars;
    env->ReleaseStringUTFChars(value, chars);
    return out;
}

// Frees every local reference made while it lived: a request makes a few
// dozen, and a worker thread that is never detached would otherwise keep them.
struct LocalFrame
{
    JNIEnv* env;
    explicit LocalFrame(JNIEnv* environment) : env(environment) { (void)env->PushLocalFrame(128); }
    ~LocalFrame() { (void)env->PopLocalFrame(nullptr); }
    LocalFrame(const LocalFrame&) = delete;
    LocalFrame& operator=(const LocalFrame&) = delete;
};

} // namespace

std::optional<core::EngineError> performHttps(const HttpRequest& request, const ParsedUrl&, HttpResponse& response)
{
    auto* env = static_cast<JNIEnv*>(platform::androidJavaEnv());
    if (env == nullptr) {
        const I18nArg args[] = {{"url", request.url}};
        return core::makeError(ENG_TR("net.err.http_tls_unsupported"), args);
    }
    for (const HttpHeader& header : request.headers) {
        if (auto error = checkHeader(header); error.has_value())
            return error;
    }

    const LocalFrame frame(env);
    jclass urlClass = env->FindClass("java/net/URL");
    jclass connectionClass = env->FindClass("java/net/HttpURLConnection");
    jclass inputClass = env->FindClass("java/io/InputStream");
    jclass outputClass = env->FindClass("java/io/OutputStream");
    if (urlClass == nullptr || connectionClass == nullptr || inputClass == nullptr || outputClass == nullptr)
        return failure(takeException(env), request.url);

    jobject target = env->NewObject(urlClass, env->GetMethodID(urlClass, "<init>", "(Ljava/lang/String;)V"),
                                    env->NewStringUTF(request.url.c_str()));
    if (std::string thrown = takeException(env); !thrown.empty())
        return failure(thrown, request.url);
    jobject connection =
        env->CallObjectMethod(target, env->GetMethodID(urlClass, "openConnection", "()Ljava/net/URLConnection;"));
    if (std::string thrown = takeException(env); !thrown.empty())
        return failure(thrown, request.url);

    const auto method = [&](const char* name, const char* signature) {
        return env->GetMethodID(connectionClass, name, signature);
    };
    env->CallVoidMethod(connection, method("setRequestMethod", "(Ljava/lang/String;)V"),
                        env->NewStringUTF(request.method.c_str()));
    env->CallVoidMethod(connection, method("setInstanceFollowRedirects", "(Z)V"), JNI_FALSE);
    env->CallVoidMethod(connection, method("setConnectTimeout", "(I)V"), static_cast<jint>(request.timeoutMs));
    env->CallVoidMethod(connection, method("setReadTimeout", "(I)V"), static_cast<jint>(request.timeoutMs));
    env->CallVoidMethod(connection, method("setUseCaches", "(Z)V"), JNI_FALSE);
    for (const HttpHeader& header : request.headers) {
        // The host is the URL's, for the reason the socket client gives.
        if (header.name.size() == 4 && std::tolower(static_cast<unsigned char>(header.name[0])) == 'h' &&
            std::tolower(static_cast<unsigned char>(header.name[1])) == 'o' &&
            std::tolower(static_cast<unsigned char>(header.name[2])) == 's' &&
            std::tolower(static_cast<unsigned char>(header.name[3])) == 't')
            continue;
        env->CallVoidMethod(connection, method("setRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V"),
                            env->NewStringUTF(header.name.c_str()), env->NewStringUTF(header.value.c_str()));
    }
    if (std::string thrown = takeException(env); !thrown.empty())
        return failure(thrown, request.url);

    if (!request.body.empty()) {
        env->CallVoidMethod(connection, method("setDoOutput", "(Z)V"), JNI_TRUE);
        jobject out = env->CallObjectMethod(connection, method("getOutputStream", "()Ljava/io/OutputStream;"));
        if (std::string thrown = takeException(env); !thrown.empty())
            return failure(thrown, request.url);
        jbyteArray bytes = env->NewByteArray(static_cast<jsize>(request.body.size()));
        env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(request.body.size()),
                                reinterpret_cast<const jbyte*>(request.body.data()));
        env->CallVoidMethod(out, env->GetMethodID(outputClass, "write", "([B)V"), bytes);
        env->CallVoidMethod(out, env->GetMethodID(outputClass, "close", "()V"));
        if (std::string thrown = takeException(env); !thrown.empty())
            return failure(thrown, request.url);
    }

    // The handshake, the request and the status line all happen here.
    const jint code = env->CallIntMethod(connection, method("getResponseCode", "()I"));
    if (std::string thrown = takeException(env); !thrown.empty())
        return failure(thrown, request.url);
    response.statusCode = static_cast<u16>(code);
    response.statusMessage = text(env, static_cast<jstring>(env->CallObjectMethod(
                                           connection, method("getResponseMessage", "()Ljava/lang/String;"))));
    env->ExceptionClear();

    // Header 0 is the status line, with no name; the list ends at a null value.
    const jmethodID fieldKey = method("getHeaderFieldKey", "(I)Ljava/lang/String;");
    const jmethodID field = method("getHeaderField", "(I)Ljava/lang/String;");
    for (jint index = 0; index < 1000; ++index) {
        auto value = static_cast<jstring>(env->CallObjectMethod(connection, field, index));
        if (value == nullptr)
            break;
        auto name = static_cast<jstring>(env->CallObjectMethod(connection, fieldKey, index));
        if (name != nullptr)
            response.headers.push_back({text(env, name), text(env, value)});
        env->DeleteLocalRef(value);
        if (name != nullptr)
            env->DeleteLocalRef(name);
    }
    env->ExceptionClear();

    // An error status's body is on the error stream, and may not exist.
    jobject in = env->CallObjectMethod(
        connection, method(code >= 400 ? "getErrorStream" : "getInputStream", "()Ljava/io/InputStream;"));
    if (std::string thrown = takeException(env); !thrown.empty())
        return failure(thrown, request.url);
    if (in != nullptr) {
        const jmethodID read = env->GetMethodID(inputClass, "read", "([B)I");
        jbyteArray chunk = env->NewByteArray(8192);
        while (true) {
            const jint got = env->CallIntMethod(in, read, chunk);
            if (std::string thrown = takeException(env); !thrown.empty())
                return failure(thrown, request.url);
            if (got < 0)
                break;
            if (response.body.size() + static_cast<std::size_t>(got) > request.maxBodyBytes) {
                const I18nArg args[] = {{"limit", static_cast<core::i64>(request.maxBodyBytes)}};
                return core::makeError(ENG_TR("net.err.http_body_too_large"), args);
            }
            const std::size_t at = response.body.size();
            response.body.resize(at + static_cast<std::size_t>(got));
            env->GetByteArrayRegion(chunk, 0, got, reinterpret_cast<jbyte*>(response.body.data() + at));
        }
        env->CallVoidMethod(in, env->GetMethodID(inputClass, "close", "()V"));
        env->ExceptionClear();
    }

    response.ok = true;
    return std::nullopt;
}

} // namespace engine::net
