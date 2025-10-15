#include "common.h"

#include <catch2/catch_session.hpp>

int main(int argc, char* argv[])
{
    Catch::Session session;

    using namespace Catch::Clara;
    std::string log_level = "critical", log_file = "stderr";

    auto cli =
            session.cli() | Opt(log_level, "level")["--log-level"]("oxen-logging log level to apply to the test run") |
            Opt(log_file, "file")["--log-file"](
                    "oxen-logging log file to output logs to, or one of  or one of stdout/-/stderr/syslog.");

    session.cli(cli);

    namespace log = oxen::log;
    if (int rc = session.applyCommandLine(argc, argv); rc != 0)
        return rc;

    log::Level lvl = log::level_from_string(log_level);

    constexpr std::array print_vals = {"stdout", "-", "", "stderr", "nocolor", "stdout-nocolor", "stderr-nocolor"};
    log::Type type;
    if (std::count(print_vals.begin(), print_vals.end(), log_file))
        type = log::Type::Print;
    else if (log_file == "syslog")
        type = log::Type::System;
    else
        type = log::Type::File;

    oxen::log::add_sink(type, log_file, "[%T.%f] [%*] [\x1b[1m%n\x1b[0m:%^%l%$|\x1b[3m%g:%#\x1b[0m] %v");
    oxen::log::reset_level(lvl);

    return session.run();
}
