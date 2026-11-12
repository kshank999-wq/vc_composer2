// Replacing a file without destroying it first (spec 02 section 12).
//
// The property here is one the old writers did not have and said they did.
// All three opened the target with `std::ios::trunc` and then wrote, so the
// previous version was gone before the first byte of the new one arrived. A
// write that failed part-way - a full disk, an I/O error, a volume that went
// away - left the composer with neither, and the code then removed the
// remains under a comment about leaving no half-written file. True, and
// beside the point: the file it was protecting had already gone.
//
// So what is checked is not that a good write works. It is that a *failed*
// one changes nothing.
#include "vcc/core/Files.h"

#include <csignal>
#include <filesystem>
#include <fstream>
#include <string>

#if !defined(_WIN32)
#include <sys/resource.h>
#endif

#include "vcc_test.h"
#include "vcc_test_files.h"

using namespace vcc::files;

namespace {

std::string read_back(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

bool exists(const std::string& path)
{
    std::error_code code;
    return std::filesystem::exists(path, code);
}

/// Any leftover scratch file for this target, whatever number it carried.
///
/// The name ends in a counter, so this looks for the prefix: a writer that
/// leaves its scratch behind fills a composer's score folder with rubbish,
/// and a test that only knew one exact name would miss it.
bool any_partial_left(const std::string& path)
{
    const std::string prefix = temporary_path_for(path);
    const std::filesystem::path directory =
        std::filesystem::path(prefix).parent_path();

    std::error_code code;
    for (const auto& entry : std::filesystem::directory_iterator(directory, code)) {
        const std::string name = entry.path().string();
        if (name.rfind(prefix, 0) == 0)
            return true;
    }
    return false;
}

}  // namespace

VCC_TEST(a_file_is_written_and_read_back_whole)
{
    const std::string path = vcc::test::unique_temp_path("atomic", ".txt");

    const FileWriteResult result = write_file_atomically(path, "the first version\n");
    VCC_CHECK(result.ok);
    VCC_CHECK(result.error.empty());
    VCC_CHECK_EQ(result.byte_count, std::size_t{18});
    VCC_CHECK(read_back(path) == "the first version\n");

    std::remove(path.c_str());
}

VCC_TEST(replacing_a_file_leaves_the_new_contents_and_nothing_else)
{
    const std::string path = vcc::test::unique_temp_path("atomic-replace", ".txt");

    VCC_CHECK(write_file_atomically(path, "old\n").ok);
    VCC_CHECK(write_file_atomically(path, "new and longer\n").ok);

    VCC_CHECK(read_back(path) == "new and longer\n");

    // The scratch file is not a thing to leave lying beside a composer's
    // score.
    VCC_CHECK(!any_partial_left(path));

    std::remove(path.c_str());
}

VCC_TEST(a_write_that_cannot_be_put_in_place_leaves_the_previous_file_alone)
{
    // The whole point, and the case the old code got wrong.
    //
    // The failure is forced by aiming at a path that already holds a
    // *directory*: the scratch file beside it is written perfectly well, and
    // the rename onto a non-empty directory is what fails. That is the same
    // shape as a rename failing for any other reason, and unlike a full disk
    // it happens the same way on every machine.
    const std::string path = vcc::test::unique_temp_path("atomic-blocked", "");

    std::error_code code;
    std::filesystem::create_directories(std::filesystem::path(path) / "inside", code);
    VCC_CHECK(exists(path));

    const FileWriteResult result = write_file_atomically(path, "this cannot land\n");

    VCC_CHECK(!result.ok);
    VCC_CHECK(!result.error.empty());

    // Untouched: still there, and still a directory with its contents.
    VCC_CHECK(exists(path));
    VCC_CHECK(exists(path + "/inside"));

    // And the scratch file was cleaned up rather than left behind.
    VCC_CHECK(!any_partial_left(path));

    std::filesystem::remove_all(path, code);
}

VCC_TEST(a_failure_says_why_without_ever_naming_the_path)
{
    // These strings reach the log, and a path carries the composer's
    // directory layout (spec 01 sections 9, 10).
    const std::string path =
        vcc::test::unique_temp_path("atomic-secret-film-cue-07", ".txt");

    std::error_code code;
    std::filesystem::create_directories(path, code);

    const FileWriteResult result = write_file_atomically(path, "no\n");
    VCC_CHECK(!result.ok);
    VCC_CHECK(result.error.find(path) == std::string::npos);
    VCC_CHECK(result.error.find("secret-film") == std::string::npos);
    VCC_CHECK(result.error.find('/') == std::string::npos);

    std::filesystem::remove_all(path, code);
}

VCC_TEST(a_folder_that_is_not_there_is_refused_without_creating_anything)
{
    const std::string path =
        vcc::test::temp_directory() + "/vcc-no-such-folder-here/part.vcs";

    const FileWriteResult result = write_file_atomically(path, "hello\n");
    VCC_CHECK(!result.ok);
    VCC_CHECK(!exists(path));
    VCC_CHECK_EQ(result.byte_count, std::size_t{0});

    // This is the *open* failure, a different branch from the rename one
    // below, and it has its own error string to keep the path out of.
    VCC_CHECK(result.error.find("vcc-no-such-folder-here") == std::string::npos);
    VCC_CHECK(result.error.find('/') == std::string::npos);
}

VCC_TEST(an_empty_path_is_refused_rather_than_written_somewhere)
{
    const FileWriteResult result = write_file_atomically("", "hello\n");
    VCC_CHECK(!result.ok);
    VCC_CHECK(!result.error.empty());
}

VCC_TEST(an_empty_file_is_a_file_and_not_a_failure)
{
    // A part with nothing in it is still a part, and a settings file with
    // every value at its default is still a settings file.
    const std::string path = vcc::test::unique_temp_path("atomic-empty", ".txt");

    const FileWriteResult result = write_file_atomically(path, "");
    VCC_CHECK(result.ok);
    VCC_CHECK_EQ(result.byte_count, std::size_t{0});
    VCC_CHECK(exists(path));
    VCC_CHECK(read_back(path).empty());

    std::remove(path.c_str());
}

VCC_TEST(two_writes_to_one_target_do_not_share_a_scratch_file)
{
    // A part and its settings are saved together, and two saves in flight at
    // once must not write into the same scratch name.
    const std::string first = vcc::test::unique_temp_path("atomic-a", ".txt");
    const std::string second = vcc::test::unique_temp_path("atomic-b", ".txt");

    VCC_CHECK(temporary_path_for(first) != temporary_path_for(second));

    VCC_CHECK(write_file_atomically(first, "one\n").ok);
    VCC_CHECK(write_file_atomically(second, "two\n").ok);
    VCC_CHECK(read_back(first) == "one\n");
    VCC_CHECK(read_back(second) == "two\n");

    std::remove(first.c_str());
    std::remove(second.c_str());
}

#if !defined(_WIN32)
VCC_TEST(a_disk_that_fills_up_mid_write_does_not_cost_the_previous_file)
{
    // The failure this module exists for, produced for real rather than
    // simulated. `RLIMIT_FSIZE` makes the filesystem refuse a write past a
    // size, which is what a full disk does, and unlike a full disk it needs
    // no privileges and happens identically every run.
    //
    // It is also the only shape that separates a scratch-file write from the
    // old truncate-in-place one. Aiming at a directory fails at the *open*,
    // before either could have destroyed anything; this fails afterwards,
    // which is exactly when the old code had already thrown the previous
    // version away.
    const std::string path = vcc::test::unique_temp_path("atomic-fullup", ".txt");

    const std::string original = "the version worth keeping\n";
    VCC_CHECK(write_file_atomically(path, original).ok);

    // Without this, exceeding the limit raises SIGXFSZ and takes the whole
    // test process with it.
    auto previous_handler = std::signal(SIGXFSZ, SIG_IGN);

    rlimit limit{};
    VCC_CHECK(getrlimit(RLIMIT_FSIZE, &limit) == 0);
    const rlim_t previous_soft = limit.rlim_cur;

    limit.rlim_cur = 512;
    if (setrlimit(RLIMIT_FSIZE, &limit) != 0) {
        std::signal(SIGXFSZ, previous_handler);
        std::printf("[  skip ] this system will not lower RLIMIT_FSIZE\n");
        std::remove(path.c_str());
        return;
    }

    const FileWriteResult result = write_file_atomically(path, std::string(8192, 'x'));

    limit.rlim_cur = previous_soft;
    (void)setrlimit(RLIMIT_FSIZE, &limit);
    std::signal(SIGXFSZ, previous_handler);

    VCC_CHECK(!result.ok);
    VCC_CHECK(!result.error.empty());

    // The whole point: what was on disk is still what was on disk.
    VCC_CHECK(read_back(path) == original);
    VCC_CHECK(!any_partial_left(path));

    std::remove(path.c_str());
}
#endif

VCC_TEST_MAIN("files/AtomicWrite")
