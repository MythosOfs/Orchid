// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <memory>
#include <string_view>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "common/scope_exit.h"
#include "core/hle/service/ssl/ssl_backend.h"
#include "core/internal_network/network.h"
#include "core/internal_network/sockets.h"

namespace {

constexpr std::string_view Message = "queued by the server";

// A throwaway self-signed server context; the client skips verification.
SSL_CTX* MakeServerContext() {
    EVP_PKEY_CTX* key_ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    EVP_PKEY* key = nullptr;
    REQUIRE(EVP_PKEY_keygen_init(key_ctx) == 1);
    REQUIRE(EVP_PKEY_CTX_set_ec_paramgen_curve_nid(key_ctx, NID_X9_62_prime256v1) == 1);
    REQUIRE(EVP_PKEY_keygen(key_ctx, &key) == 1);
    EVP_PKEY_CTX_free(key_ctx);
    X509* cert = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    X509_gmtime_adj(X509_getm_notBefore(cert), 0);
    X509_gmtime_adj(X509_getm_notAfter(cert), 3600);
    X509_set_pubkey(cert, key);
    X509_NAME* name = X509_NAME_new();
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
    X509_set_subject_name(cert, name);
    X509_set_issuer_name(cert, name);
    X509_NAME_free(name);
    REQUIRE(X509_sign(cert, key, EVP_sha256()) > 0);
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    REQUIRE(SSL_CTX_use_certificate(ctx, cert) == 1);
    REQUIRE(SSL_CTX_use_PrivateKey(ctx, key) == 1);
    X509_free(cert);
    EVP_PKEY_free(key);
    return ctx;
}

} // Anonymous namespace

TEST_CASE("SSL Peek returns the bytes waiting and Read still gets them", "[core][ssl]") {
    Network::NetworkInstance network_instance;
    SSL_CTX* server_ctx = MakeServerContext();
    SCOPE_EXIT {
        SSL_CTX_free(server_ctx);
    };

    Network::Socket listener;
    REQUIRE(listener.Initialize(Network::Domain::INET, Network::Type::STREAM,
                                Network::Protocol::TCP) == Network::Errno::SUCCESS);
    REQUIRE(listener.Bind({Network::Domain::INET, {127, 0, 0, 1}, 0}) == Network::Errno::SUCCESS);
    REQUIRE(listener.Listen(1) == Network::Errno::SUCCESS);
    const auto [address, name_error] = listener.GetSockName();
    REQUIRE(name_error == Network::Errno::SUCCESS);

    // Connected and accepted before the server thread starts, and every read times out: a failed
    // step ends the test instead of leaving a thread blocked.
    auto socket = std::make_shared<Network::Socket>();
    REQUIRE(socket->Initialize(Network::Domain::INET, Network::Type::STREAM,
                               Network::Protocol::TCP) == Network::Errno::SUCCESS);
    REQUIRE(socket->Connect(address) == Network::Errno::SUCCESS);
    REQUIRE(socket->SetRcvTimeo(5000) == Network::Errno::SUCCESS);
    auto accepted = listener.Accept();
    REQUIRE(accepted.second == Network::Errno::SUCCESS);
    const auto& server_socket = accepted.first.socket;
    REQUIRE(server_socket->SetRcvTimeo(5000) == Network::Errno::SUCCESS);

    std::jthread server{[&] {
        SSL* ssl = SSL_new(server_ctx);
        SSL_set_fd(ssl, static_cast<int>(server_socket->GetFD()));
        if (SSL_accept(ssl) == 1) {
            SSL_write(ssl, Message.data(), static_cast<int>(Message.size()));
            // Holds the connection until the client closes it.
            std::array<char, 1> byte;
            SSL_read(ssl, byte.data(), 1);
        }
        SSL_free(ssl);
    }};
    SCOPE_EXIT {
        socket->Shutdown(Network::ShutdownHow::RDWR);
    };
    std::unique_ptr<Service::SSL::SSLConnectionBackend> backend;
    REQUIRE(Service::SSL::CreateSSLConnectionBackend(&backend).IsSuccess());
    backend->SetVerifyOption(0);
    backend->SetSocket(socket);
    REQUIRE(backend->DoHandshake().IsSuccess());

    std::array<u8, 64> peeked{};
    size_t peeked_size = 0;
    REQUIRE(backend->Peek(&peeked_size, peeked).IsSuccess());
    CHECK(std::string_view{reinterpret_cast<const char*>(peeked.data()), peeked_size} == Message);

    std::array<u8, 64> read{};
    size_t read_size = 0;
    REQUIRE(backend->Read(&read_size, read).IsSuccess());
    CHECK(std::string_view{reinterpret_cast<const char*>(read.data()), read_size} == Message);
}
