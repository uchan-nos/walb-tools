#include <cstdio>
#include <string>
#include "cybozu/test.hpp"
#include "process.hpp"

CYBOZU_TEST_AUTO(call)
{
    std::string s = cybozu::process::call("/usr/bin/basename", {"/usr/bin/test"});
    ::printf("%zu '%s'\n", s.size(), s.c_str());
    CYBOZU_TEST_EQUAL(s, "test\n");
}