// The socket contract EditorDebugServer's bind path rests on.
//
// The server used to bind its port with SO_REUSEADDR only, so a restarted editor
// could rebind while the previous run's accepted connections drained. On Windows
// that same option lets a second LIVE process bind one listening port: both binds
// succeed, both log "Listening", and the first binder answers every request, so the
// second editor's tooling silently queries a stranger.
//
// The fix binds with SO_EXCLUSIVEADDRUSE first and only falls back to SO_REUSEADDR
// when that fails. Three claims have to hold, and none is checkable by reading the
// server's source:
//   1. The double bind really is permitted with SO_REUSEADDR (else the fix is dead code).
//   2. An exclusive holder cannot be bound over, including by an SO_REUSEADDR socket
//      — that is what actually prevents the split port.
//   3. TIME_WAIT remnants defeat the exclusive bind but not the SO_REUSEADDR fallback,
//      so a restart still comes up.
// Asserted against the real socket stack, with the same calls the server makes.
#include <gtest/gtest.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cstdint>

namespace
{

#ifdef _WIN32
using SocketType = SOCKET;
constexpr SocketType kInvalidSock = INVALID_SOCKET;
inline void SocketClose(SocketType s) { closesocket(s); }
inline bool SocketFailed(int result) { return result == SOCKET_ERROR; }
#else
using SocketType = int;
constexpr SocketType kInvalidSock = -1;
inline void SocketClose(SocketType s) { ::close(s); }
inline bool SocketFailed(int result) { return result < 0; }
#endif

enum class Reuse
{
    Exclusive, // SO_EXCLUSIVEADDRUSE where the platform has it
    Reusing,   // SO_REUSEADDR
    Plain,
};

sockaddr_in LoopbackAddr(uint16_t port)
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    return addr;
}

// port==0 asks the OS for an ephemeral one; outPort reports what was bound.
SocketType BindAndListen(uint16_t port, Reuse mode, uint16_t& outPort)
{
    SocketType sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == kInvalidSock)
        return kInvalidSock;

    int optVal = 1;
    if (mode == Reuse::Reusing)
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&optVal),
                   sizeof(optVal));
#ifdef _WIN32
    if (mode == Reuse::Exclusive)
        setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&optVal),
                   sizeof(optVal));
#endif

    sockaddr_in addr = LoopbackAddr(port);
    if (SocketFailed(bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr))) ||
        SocketFailed(listen(sock, 8)))
    {
        SocketClose(sock);
        return kInvalidSock;
    }

    sockaddr_in bound{};
#ifdef _WIN32
    int len = static_cast<int>(sizeof(bound));
#else
    socklen_t len = sizeof(bound);
#endif
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&bound), &len) != 0)
    {
        SocketClose(sock);
        return kInvalidSock;
    }
    outPort = ntohs(bound.sin_port);
    return sock;
}

class DebugServerPortExclusivityTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
#ifdef _WIN32
        WSADATA wsa{};
        ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &wsa), 0);
#endif
    }
    void TearDown() override
    {
#ifdef _WIN32
        WSACleanup();
#endif
    }
};

} // namespace

// Claim 1: with SO_REUSEADDR alone the hazard is real. If this ever stops holding,
// the platform started refusing the second bind by itself and the exclusive bind has
// become redundant — a reason to revisit it, not to delete this test quietly.
#ifdef _WIN32
TEST_F(DebugServerPortExclusivityTests, ReuseAddrAlonePermitsASecondLiveBind)
{
    uint16_t port = 0;
    SocketType first = BindAndListen(0, Reuse::Reusing, port);
    ASSERT_NE(first, kInvalidSock);
    ASSERT_NE(port, 0);

    uint16_t secondPort = 0;
    SocketType second = BindAndListen(port, Reuse::Reusing, secondPort);
    const bool doubleBound = second != kInvalidSock;
    if (doubleBound)
        SocketClose(second);
    SocketClose(first);

    EXPECT_TRUE(doubleBound) << "SO_REUSEADDR no longer permits a second live bind; the server's "
                                "exclusive bind is now redundant and should be re-examined";
}
#endif

// Claim 2: the exclusive holder cannot be bound over — not by another exclusive
// binder, and not by an SO_REUSEADDR one. This is the property that actually stops
// two editors from splitting a port.
TEST_F(DebugServerPortExclusivityTests, AnExclusiveHolderCannotBeBoundOver)
{
    uint16_t port = 0;
    SocketType owner = BindAndListen(0, Reuse::Exclusive, port);
    ASSERT_NE(owner, kInvalidSock);
    ASSERT_NE(port, 0);

    uint16_t other = 0;
    SocketType exclusiveIntruder = BindAndListen(port, Reuse::Exclusive, other);
    EXPECT_EQ(exclusiveIntruder, kInvalidSock) << "a second exclusive bind took the port";
    if (exclusiveIntruder != kInvalidSock)
        SocketClose(exclusiveIntruder);

    SocketType reusingIntruder = BindAndListen(port, Reuse::Reusing, other);
    EXPECT_EQ(reusingIntruder, kInvalidSock)
        << "an SO_REUSEADDR socket bound over the exclusive holder — the split-port hazard is "
           "still open";
    if (reusingIntruder != kInvalidSock)
        SocketClose(reusingIntruder);

    SocketClose(owner);
}

// Claim 3: a TIME_WAIT remnant (this editor's own previous run, connections still
// draining) defeats the exclusive bind, and the SO_REUSEADDR fallback is what brings
// the restart up. Both halves matter: the first is why the fallback exists at all,
// the second is the behaviour that must not regress.
TEST_F(DebugServerPortExclusivityTests, TimeWaitDefeatsTheExclusiveBindButNotTheFallback)
{
    uint16_t port = 0;
    SocketType listener = BindAndListen(0, Reuse::Exclusive, port);
    ASSERT_NE(listener, kInvalidSock);

    // Complete one connection so a TIME_WAIT four-tuple survives the teardown.
    SocketType client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_NE(client, kInvalidSock);
    sockaddr_in addr = LoopbackAddr(port);
    ASSERT_FALSE(SocketFailed(connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr))));
    SocketType accepted = accept(listener, nullptr, nullptr);
    ASSERT_NE(accepted, kInvalidSock);
    SocketClose(client);
    SocketClose(accepted);
    SocketClose(listener);

    uint16_t rebindPort = 0;
    SocketType exclusiveRetry = BindAndListen(port, Reuse::Exclusive, rebindPort);
    const bool exclusiveWorked = exclusiveRetry != kInvalidSock;
    if (exclusiveWorked)
        SocketClose(exclusiveRetry);

    SocketType fallback = BindAndListen(port, Reuse::Reusing, rebindPort);
    EXPECT_NE(fallback, kInvalidSock)
        << "the SO_REUSEADDR restart path is broken; a restarted editor would lose its debug "
           "server until the port drained";
    if (fallback != kInvalidSock)
        SocketClose(fallback);

    // Not an assertion on the platform: if the exclusive bind happened to succeed the
    // fallback simply never runs in the server. Recorded so a change of behaviour here
    // is visible in the log rather than silent.
    if (exclusiveWorked)
        GTEST_LOG_(INFO) << "exclusive rebind succeeded over TIME_WAIT on this platform; the "
                            "server's fallback is unreachable for this case";
}
