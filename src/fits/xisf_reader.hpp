// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/fits/xisf_reader.hpp
// Purpose:       XISF header extraction by parsing the open XISF spec directly.
//                Produces the same RawHeader the CFITSIO path yields, so the
//                resolver, fingerprint, and frame store are format-agnostic. No
//                PixInsight code is involved (PCL License 2.0 is not GPL).
// Created:       2026-07-25
// Last Modified: 2026-07-25
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#pragma once

#include <string>

#include "fits_reader.hpp"

namespace starbase::fits {

// True if the file begins with the 8-byte monolithic XISF signature
// "XISF0100". Cheap: reads only the first 8 bytes. Never throws.
bool is_xisf(const std::string& path);

// Parse a monolithic XISF file's XML header into a RawHeader. Each <Image>
// element becomes one image HDU; its embedded <FITSKeyword> elements become
// Cards (FITS-style quotes and padding stripped, XML entities decoded), and its
// geometry / sampleFormat attributes fill NAXIS1/NAXIS2 and BITPIX. Pixel and
// attachment data are never read. Throws FitsError on a malformed signature,
// truncated header, or a header with no image.
RawHeader read_xisf(const std::string& path);

}  // namespace starbase::fits
