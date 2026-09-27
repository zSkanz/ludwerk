// `https://` on macOS: `NSURLSession` (ADR 0063).
//
// The TLS and the HTTP in one API, verifying against the keychain's trust
// settings, with connections kept alive inside the one session. An ephemeral
// configuration: no cookies, no cache and no credentials are kept between
// requests, which is what the socket client does too. Redirects are refused by
// the delegate because `performHttp` follows them itself, by one rule for every
// platform.
//
// Compiled with ARC (`-fobjc-arc`, set on this file in the module's CMake).

#import <Foundation/Foundation.h>
#include <string>

#include "engine/core/i18n.h"
#include "http_internal.h"

@interface EngineHttpsNoRedirect : NSObject <NSURLSessionTaskDelegate>
@end

@implementation EngineHttpsNoRedirect
- (void)URLSession:(NSURLSession*)session
                          task:(NSURLSessionTask*)task
    willPerformHTTPRedirection:(NSHTTPURLResponse*)response
                    newRequest:(NSURLRequest*)request
             completionHandler:(void (^)(NSURLRequest*))completionHandler
{
    (void)session;
    (void)task;
    (void)response;
    (void)request;
    completionHandler(nil);
}
@end

namespace engine::net {
namespace {

using core::I18nArg;

[[nodiscard]] NSURLSession* session()
{
    static NSURLSession* made = [] {
        NSURLSessionConfiguration* configuration = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        return [NSURLSession sessionWithConfiguration:configuration
                                             delegate:[[EngineHttpsNoRedirect alloc] init]
                                        delegateQueue:nil];
    }();
    return made;
}

[[nodiscard]] NSString* string(const std::string& text)
{
    return [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding];
}

[[nodiscard]] std::string text(NSString* value)
{
    if (value == nil)
        return {};
    const char* chars = [value UTF8String];
    return chars == nullptr ? std::string{} : std::string(chars);
}

[[nodiscard]] core::EngineError failure(NSError* error, const std::string& url)
{
    if ([[error domain] isEqualToString:NSURLErrorDomain]) {
        switch ([error code]) {
        case NSURLErrorServerCertificateUntrusted:
        case NSURLErrorServerCertificateHasUnknownRoot:
        case NSURLErrorServerCertificateHasBadDate:
        case NSURLErrorServerCertificateNotYetValid:
        case NSURLErrorSecureConnectionFailed: {
            const I18nArg args[] = {{"url", url}};
            return core::makeError(ENG_TR("net.err.https_untrusted"), args);
        }
        case NSURLErrorTimedOut:
            return core::makeError(ENG_TR("net.err.http_response_timeout"));
        default:
            break;
        }
    }
    const I18nArg args[] = {{"url", url}, {"code", static_cast<core::i64>([error code])}};
    return core::makeError(ENG_TR("net.err.https_failed"), args);
}

} // namespace

std::optional<core::EngineError> performHttps(const HttpRequest& request, const ParsedUrl&, HttpResponse& response)
{
    for (const HttpHeader& header : request.headers) {
        if (auto error = checkHeader(header); error.has_value())
            return error;
    }

    @autoreleasepool {
        NSURL* target = [NSURL URLWithString:string(request.url)];
        if (target == nil) {
            const I18nArg args[] = {{"url", request.url}};
            return core::makeError(ENG_TR("net.err.http_url_malformed"), args);
        }
        NSMutableURLRequest* outgoing = [NSMutableURLRequest requestWithURL:target];
        [outgoing setHTTPMethod:string(request.method)];
        [outgoing setTimeoutInterval:static_cast<double>(request.timeoutMs) / 1000.0];
        for (const HttpHeader& header : request.headers) {
            // The host is the URL's, for the reason the socket client gives.
            if ([string(header.name) caseInsensitiveCompare:@"host"] == NSOrderedSame)
                continue;
            [outgoing setValue:string(header.value) forHTTPHeaderField:string(header.name)];
        }
        if (!request.body.empty())
            [outgoing setHTTPBody:[NSData dataWithBytes:request.body.data() length:request.body.size()]];

        __block NSData* received = nil;
        __block NSURLResponse* answered = nil;
        __block NSError* failed = nil;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        NSURLSessionDataTask* task =
            [session() dataTaskWithRequest:outgoing
                         completionHandler:^(NSData* data, NSURLResponse* reply, NSError* problem) {
                           received = data;
                           answered = reply;
                           failed = problem;
                           dispatch_semaphore_signal(done);
                         }];
        [task resume];
        // The session's own timeout ends the task; this is a backstop, a
        // second past it, against a completion that never comes.
        const auto limit = static_cast<int64_t>(request.timeoutMs + 1000) * static_cast<int64_t>(NSEC_PER_MSEC);
        if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, limit)) != 0) {
            [task cancel];
            return core::makeError(ENG_TR("net.err.http_response_timeout"));
        }
        if (failed != nil)
            return failure(failed, request.url);

        auto* http = static_cast<NSHTTPURLResponse*>(answered);
        response.statusCode = static_cast<u16>([http statusCode]);
        response.statusMessage = text([NSHTTPURLResponse localizedStringForStatusCode:[http statusCode]]);
        NSDictionary* fields = [http allHeaderFields];
        for (id key in fields) {
            id value = [fields objectForKey:key];
            response.headers.push_back({text([key description]), text([value description])});
        }
        if (received != nil && [received length] > request.maxBodyBytes) {
            const I18nArg args[] = {{"limit", static_cast<core::i64>(request.maxBodyBytes)}};
            return core::makeError(ENG_TR("net.err.http_body_too_large"), args);
        }
        if (received != nil)
            response.body.assign(static_cast<const char*>([received bytes]), [received length]);
    }

    response.ok = true;
    return std::nullopt;
}

} // namespace engine::net
