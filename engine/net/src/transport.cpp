// What `ITransport` answers where a transport has no relay to speak of (ADR
// 0178): the memory one, and whatever comes after ENet until it learns.
#include "engine/net/transport.h"

#include "engine/core/i18n.h"

namespace engine::net {

std::optional<core::EngineError> ITransport::useRelay(std::string_view relay)
{
    (void)relay;
    return core::makeError(ENG_TR("net.err.relay_unsupported"));
}

std::optional<core::EngineError> ITransport::connectByCode(std::string_view relay, std::string_view code, bool direct,
                                                           PeerId& outPeer)
{
    (void)relay;
    (void)code;
    (void)direct;
    outPeer = PeerId{};
    return core::makeError(ENG_TR("net.err.relay_unsupported"));
}

} // namespace engine::net
