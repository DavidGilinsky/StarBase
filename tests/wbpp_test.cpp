// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          tests/wbpp_test.cpp
// Purpose:       Tests for the WBPP command renderer: per-type dir= tokens,
//                automation vs loadOnly, the comma/equals sharp edge, and
//                shell quoting of the -r argument. No database, no PixInsight.
// Created:       2026-07-25
// Last Modified: 2026-07-25
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include <iostream>
#include <string>

#include "wbpp.hpp"

namespace {

namespace pix = starbase::pix;

int g_failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::cerr << "FAIL: " << what << "\n"; ++g_failures; }
}
bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

int main() {
    const std::string job = "/srv/staging/job-42";

    // ---- headless automation over a light+dark+flat tree ----
    {
        pix::WbppProfile prof;
        prof.output_dir = "/srv/out/job-42";
        auto p = pix::render(job, {"light", "dark", "flat"}, prof, /*load_only=*/false);

        check(p.mode == "automation", "automation mode");
        check(p.dirs.size() == 3, "one dir per image type");
        check(has(p.command, "dir=" + job + "/light"), "light dir= present");
        check(has(p.command, "dir=" + job + "/dark"), "dark dir= present");
        check(has(p.command, "dir=" + job + "/flat"), "flat dir= present");
        check(has(p.command, "automationMode=true"), "automationMode set");
        check(has(p.command, "outputDirectory=/srv/out/job-42"), "output dir set");
        check(has(p.command, "groupingKeywordsEnabled=true"), "grouping on by default");
        check(has(p.command, "keywords=FILTER prepost"), "default grouping keywords");
        check(has(p.command, "fitsCoordinateConvention=1"), "top-down convention");
        check(has(p.command, "--force-exit"), "headless run forces exit");
        check(!has(p.command, "loadOnly"), "automation is not loadOnly");
        check(has(p.command, "-r='"), "-r argument is single-quoted");
        check(p.warnings.empty(), "clean tree yields no warnings");
        check(has(p.launcher, "#!/usr/bin/env bash"), "launcher is a bash script");
        check(has(p.launcher, "mkdir -p '/srv/out/job-42'"), "launcher makes the output dir");
        check(has(p.launcher, "exec "), "launcher exec's PixInsight");
    }

    // ---- loadOnly handoff: opens the dialog, no --force-exit ----
    {
        pix::WbppProfile prof;
        prof.output_dir = "/srv/out/job-7";
        auto p = pix::render(job, {"light"}, prof, /*load_only=*/true);
        check(p.mode == "loadOnly", "loadOnly mode");
        check(has(p.command, "loadOnly"), "loadOnly token present");
        check(!has(p.command, "--force-exit"), "loadOnly keeps the GUI open");
    }

    // ---- the sharp edge: a path WBPP's split cannot carry ----
    {
        pix::WbppProfile prof;
        prof.output_dir = "/srv/out/a=b,c";  // both delimiters
        auto p = pix::render(job, {"light"}, prof, false);
        bool warned = false;
        for (const auto& w : p.warnings) if (has(w, "cannot parse")) warned = true;
        check(warned, "comma/equals output dir raises a warning");
    }

    // ---- grouping disabled drops the keywords token ----
    {
        pix::WbppProfile prof;
        prof.grouping_enabled = false;
        auto p = pix::render(job, {"light"}, prof, false);
        check(has(p.command, "groupingKeywordsEnabled=false"), "grouping disabled emitted");
        check(!has(p.command, "keywords="), "no keywords token when grouping is off");
    }

    // ---- extra_params are appended verbatim ----
    {
        pix::WbppProfile prof;
        prof.extra_params = {"lightExposureTolerance=5", "generateRejectionMaps=true"};
        auto p = pix::render(job, {"light"}, prof, false);
        check(has(p.command, "lightExposureTolerance=5"), "extra param 1 appended");
        check(has(p.command, "generateRejectionMaps=true"), "extra param 2 appended");
    }

    // ---- empty tree is a warning, not a crash ----
    {
        pix::WbppProfile prof;
        auto p = pix::render(job, {}, prof, false);
        bool warned = false;
        for (const auto& w : p.warnings) if (has(w, "nothing to hand off")) warned = true;
        check(warned, "empty tree warns");
    }

    if (g_failures == 0) { std::cout << "wbpp_test: all checks passed\n"; return 0; }
    std::cerr << "wbpp_test: " << g_failures << " check(s) failed\n";
    return 1;
}
