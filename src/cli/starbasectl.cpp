// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/cli/starbasectl.cpp
// Purpose:       Command-line administration for StarBase: schema setup, root
//                management, scans, queries, and exports.
// Created:       2026-07-23
// Last Modified: 2026-07-23
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include <iostream>
#include <string>
#include <vector>

#include "config.hpp"
#include "logging.hpp"
#include "starbase/version.hpp"

namespace {

void print_help(const char* argv0) {
    std::cout
        << "starbasectl " << STARBASE_VERSION << " - StarBase administration\n\n"
        << "Usage: " << argv0 << " [--config <path>] <command> [args]\n\n"
        << "Commands:\n"
        << "  db-init                 Create any missing tables from schema.sql\n"
        << "  db-seed                 Load the default header mapping (seed.sql)\n"
        << "  add-root <label> <path> Register a directory tree to index\n"
        << "  list-roots              Show registered roots and scan status\n"
        << "  scan [<label>]          Scan now (all roots, or one)\n"
        << "  version                 Print version and exit\n\n"
        << "The database password is read from SB_DB_PASSWORD.\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    std::string config_path = "/etc/starbase/starbase.conf";

    for (size_t i = 0; i < args.size();) {
        if ((args[i] == "-c" || args[i] == "--config") && i + 1 < args.size()) {
            config_path = args[i + 1];
            args.erase(args.begin() + static_cast<long>(i),
                       args.begin() + static_cast<long>(i) + 2);
            continue;
        }
        ++i;
    }

    if (args.empty() || args[0] == "-h" || args[0] == "--help" || args[0] == "help") {
        print_help(argv[0]);
        return args.empty() ? 2 : 0;
    }
    if (args[0] == "version" || args[0] == "-v" || args[0] == "--version") {
        std::cout << "starbasectl " << STARBASE_VERSION << "\n";
        return 0;
    }

    // M1 implements these against the repository layer; until then the command
    // surface exists so the CLI, packaging, and docs can be built against it.
    const std::string& cmd = args[0];
    if (cmd == "db-init" || cmd == "db-seed" || cmd == "add-root" ||
        cmd == "list-roots" || cmd == "scan") {
        std::cerr << "starbasectl: '" << cmd
                  << "' is not implemented yet (M1); config would be read from "
                  << config_path << "\n";
        return 3;
    }

    std::cerr << "starbasectl: unknown command '" << cmd << "'\n";
    print_help(argv[0]);
    return 2;
}
