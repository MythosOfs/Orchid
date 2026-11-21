// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/internal_network/network.h"
#include "core/internal_network/sockets.h"

TEST_CASE("Network::Errors", "[core]") {
    Network::NetworkInstance network_instance; // initialize network

    Network::Socket socks[2];
    for (Network::Socket& sock : socks) {
        REQUIRE(sock.Initialize(Network::Domain::INET, Network::Type::STREAM,
                                Network::Protocol::TCP) == Network::Errno::SUCCESS);
    }

    Network::SockAddrIn addr{
        Network::Domain::INET,
        {127, 0, 0, 1},
        1, // hopefully nobody running this test has something listening on port 1
    };
    REQUIRE(socks[0].Connect(addr) == Network::Errno::CONNREFUSED);

    std::vector<u8> message{1, 2, 3, 4};
    REQUIRE(socks[1].Recv(0, message).second == Network::Errno::NOTCONN);
}

TEST_CASE("Network::DefaultProtocol", "[core][sockets]") {
    // Protocol 0 asks the host for its default. A host list without IP sent it through the
    // unimplemented branch, whose assert stops this test with debug asserts on.
    Network::NetworkInstance network_instance;
    const bool previous_debug_asserts = Settings::values.use_debug_asserts.GetValue();
    Settings::values.use_debug_asserts.SetValue(true);
    SCOPE_EXIT {
        Settings::values.use_debug_asserts.SetValue(previous_debug_asserts);
    };
    for (const auto type : {Network::Type::DGRAM, Network::Type::STREAM}) {
        Network::Socket socket;
        CHECK(socket.Initialize(Network::Domain::INET, type, Network::Protocol::IP) ==
              Network::Errno::SUCCESS);
    }
}
