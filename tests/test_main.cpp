#include <string>
#include <vector>

#include "mini_test.h"

//   adserve_tests                 run every test
//   adserve_tests --list          print the test names, one per line
//   adserve_tests <substr>...     run only tests matching one of the substrings
int main(int argc, char** argv) {
    std::vector<std::string> filters;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--list") {
            mini_test::list_tests();
            return 0;
        }
        filters.push_back(arg);
    }
    return mini_test::run_all(filters);
}
