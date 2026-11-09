#include <iostream>

void DumpExternalProfile();

namespace {
    struct ProfileExit final {
        ~ProfileExit() {
            std::cout << "{\"ProfileTimingInstrumented\":true,"
                         "\"Scope\":\"Whole-process nested native work including warmup; no timing ranking\"}\n";
            DumpExternalProfile();
        }
    };
    ProfileExit ExitProfile;
}
