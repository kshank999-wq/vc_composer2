// SHA-256 against the FIPS 180-4 / NIST test vectors, incrementally and from a file.
#include <fstream>
#include <string>

#include "vcc/core/Hash.h"
#include "vcc_test.h"

using namespace vcc;

TEST_CASE("sha256: the standard vectors")
{
    CHECK(hash::sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hash::sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(hash::sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(hash::sha256_hex(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("sha256: in pieces of any size, the same digest")
{
    std::string data;
    for (int i = 0; i < 5000; ++i) data += static_cast<char>((i * 31 + 7) & 0xff);
    const auto whole = hash::sha256_hex(data);
    for (std::size_t piece : {1u, 3u, 63u, 64u, 65u, 1000u}) {
        hash::Sha256 s;
        for (std::size_t at = 0; at < data.size(); at += piece) s.update(data.data() + at, std::min(piece, data.size() - at));
        CHECK(s.hex() == whole);
    }
}

TEST_CASE("sha256: a file, streamed, with progress that can stop it")
{
    const std::string path = std::string(VCC_TEST_TMP_DIR) + "/hash-test.bin";
    const std::string data(3 * 1024 * 1024 + 17, 'x');
    {
        std::ofstream(path, std::ios::binary) << data;
    }
    std::uint64_t seen = 0;
    const auto digest = hash::sha256_file(path, [&](std::uint64_t n) {
        seen = n;
        return true;
    });
    REQUIRE(digest.has_value());
    CHECK(*digest == hash::sha256_hex(data));
    CHECK(seen == data.size());
    CHECK(!hash::sha256_file(path, [](std::uint64_t) { return false; }).has_value());
    CHECK(!hash::sha256_file(path + ".absent").has_value());
}
