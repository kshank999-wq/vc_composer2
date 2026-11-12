#include <cstring>

#include "vcc_test.h"

int main(int argc, char** argv)
{
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int ran = 0, failed_cases = 0;
    for (const auto& c : vcc_test::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        ++ran;
        int before = vcc_test::state().failures;
        try {
            c.fn();
        } catch (const vcc_test::RequireFailed&) {
        } catch (const std::exception& e) {
            vcc_test::fail(c.file, c.line, std::string("unexpected exception: ") + e.what());
        } catch (...) {
            vcc_test::fail(c.file, c.line, "unexpected non-standard exception");
        }
        if (vcc_test::state().failures != before) {
            ++failed_cases;
            std::fprintf(stderr, "[FAIL] %s\n", c.name);
        }
    }
    std::printf("%d test cases, %d failed, %d checks\n", ran, failed_cases, vcc_test::state().checks);
    return failed_cases == 0 && ran > 0 ? 0 : 1;
}
