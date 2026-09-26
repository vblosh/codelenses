#include <string>

namespace Core::Diagnostics {

class Logger {
public:
    void info(const std::string& msg);
    void warn(const std::string& msg);
};

void Logger::info(const std::string& msg) {}

void Logger::warn(const std::string& msg) {}

} // namespace Core::Diagnostics

inline namespace V1 {
void legacy_init() {}
} // namespace V1

namespace diag = Core::Diagnostics;

namespace {
const int INTERNAL_BUFFER_SIZE = 256;
}

void test_namespaces() {
    using namespace Core::Diagnostics;
    using std::string;

    Logger log;
    log.info("Starting");
    legacy_init();

    diag::Logger diag_log;
    diag_log.warn("Warning");
}
