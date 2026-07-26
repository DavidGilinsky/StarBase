// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/pix/wbpp.hpp
// Purpose:       Render a PixInsight WBPP command-line automation invocation
//                from a staged job tree and a preprocessing profile. Pure and
//                testable: no database, no PixInsight, no filesystem writes.
// Created:       2026-07-25
// Last Modified: 2026-07-25
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
// WBPP ships a command-line automation API (BPP-Automation.js, WBPP 3.0.1):
//
//   /opt/PixInsight/bin/PixInsight.sh -n --automation-mode
//     -r="<WBPP.js>,automationMode=true,dir=<tree>/light,dir=<tree>/dark,
//         outputDirectory=<out>,keywords=FILTER prepost,..." --force-exit
//
// The one sharp edge: the -r argument is split on ',' then '=', so no path may
// contain either character. StarBase controls the staging tree names, so the
// dir= paths are always safe; the output directory is caller-supplied and is
// validated here (a violation becomes a warning, not a silent breakage).
// ---------------------------------------------------------------------------
#ifndef STARBASE_PIX_WBPP_HPP
#define STARBASE_PIX_WBPP_HPP

#include <string>
#include <vector>

namespace starbase::pix {

// A stored preprocessing profile: the knobs StarBase renders into a WBPP
// command line. Defaults suit a typical archive (top-down FITS, grouping
// by filter). Anything not modelled here is appended verbatim via extra_params
// as raw "key=value" tokens.
struct WbppProfile {
    std::string pixinsight_sh = "/opt/PixInsight/bin/PixInsight.sh";
    std::string wbpp_script =
        "/opt/PixInsight/src/scripts/BatchPreprocessing/WBPP.js";
    std::string output_dir;                        // outputDirectory=
    std::string keywords = "FILTER prepost";       // grouping keywords + mode
    bool grouping_enabled = true;                  // groupingKeywordsEnabled=
    int fits_convention = 1;                        // 1 = top-down (the archive)
    std::vector<std::string> extra_params;         // appended "key=value" tokens
};

// The rendered handoff.
struct WbppPlan {
    std::string staging_root;              // the job tree passed to WBPP
    std::string output_dir;                // where WBPP writes results
    std::string mode;                      // "automation" | "loadOnly"
    std::vector<std::string> dirs;         // per-type subdirs passed as dir=
    std::vector<std::string> params;       // the ordered -r tokens (for display)
    std::string command;                   // one-line shell command (copy-paste)
    std::string launcher;                  // self-contained #!/usr/bin/env bash
    std::vector<std::string> warnings;     // e.g. a path WBPP's split can't carry
};

// Render a WBPP invocation over a staged job tree. `image_types` are the
// subdirectories present under job_root (light, dark, flat, bias, darkflat);
// each becomes a recursive `dir=`. load_only opens the WBPP dialog
// pre-populated (the "send to my desktop" path) instead of running headless.
WbppPlan render(const std::string& job_root,
                const std::vector<std::string>& image_types,
                const WbppProfile& profile, bool load_only);

}  // namespace starbase::pix

#endif  // STARBASE_PIX_WBPP_HPP
