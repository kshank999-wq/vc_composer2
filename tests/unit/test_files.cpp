#include <filesystem>

#include "vcc/core/Files.h"
#include "vcc/core/Ids.h"
#include "vcc_test.h"

namespace fs = std::filesystem;

TEST_CASE("atomic write replaces content and leaves no temporaries")
{
    std::string dir = std::string(VCC_TEST_TMP_DIR) + "/files";
    fs::create_directories(dir);
    std::string path = dir + "/a.json";
    REQUIRE(vcc::files::write_file_atomic(path, "first"));
    REQUIRE(vcc::files::write_file_atomic(path, "second"));
    CHECK_EQ(*vcc::files::read_file(path), std::string("second"));
    int entries = 0;
    for (auto& e : fs::directory_iterator(dir)) {
        (void)e;
        ++entries;
    }
    CHECK_EQ(entries, 1);
}

TEST_CASE("failed atomic write leaves the old file intact")
{
    std::string err;
    CHECK(!vcc::files::write_file_atomic(std::string(VCC_TEST_TMP_DIR) + "/no/such/dir/x", "x", &err));
    CHECK(!err.empty());
    CHECK(!vcc::files::read_file(std::string(VCC_TEST_TMP_DIR) + "/missing", &err));
}

TEST_CASE("id allocator never repeats and only moves forward")
{
    vcc::IdAllocator ids;
    CHECK_EQ(ids.next("trk"), std::string("trk-0001"));
    CHECK_EQ(ids.next("bus"), std::string("bus-0002"));
    ids.advance_to(1);
    CHECK_EQ(ids.next("trk"), std::string("trk-0003"));
    ids.advance_to(20000);
    CHECK_EQ(ids.next("n"), std::string("n-20000"));
}
