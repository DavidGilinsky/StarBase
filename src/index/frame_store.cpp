// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/index/frame_store.cpp
// Purpose:       Implementation of the file/frame/keyword persistence path.
// Created:       2026-07-24
// Last Modified: 2026-07-24
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include "frame_store.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace starbase::index {
namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Accumulates columns for an INSERT ... ON DUPLICATE KEY UPDATE. Values are
// emitted as literal SQL, so callers pass already-escaped strings or NULL.
class Upsert {
public:
    explicit Upsert(db::Database& db) : db_(db) {}

    void raw(const std::string& col, const std::string& sql_value) {
        cols_.push_back(col);
        vals_.push_back(sql_value);
    }
    void str(const std::string& col, const std::optional<std::string>& v) {
        raw(col, v ? "'" + db_.escape(*v) + "'" : "NULL");
    }
    void num(const std::string& col, const std::optional<long long>& v) {
        raw(col, v ? std::to_string(*v) : "NULL");
    }
    void inum(const std::string& col, const std::optional<int>& v) {
        raw(col, v ? std::to_string(*v) : "NULL");
    }
    void real(const std::string& col, const std::optional<double>& v) {
        raw(col, v ? std::to_string(*v) : "NULL");
    }
    void bit(const std::string& col, bool v) { raw(col, v ? "1" : "0"); }

    // INSERT ... VALUES ... ON DUPLICATE KEY UPDATE (excluding `keep` columns,
    // which are set only on insert, e.g. first_seen).
    std::string build(const std::string& table,
                      const std::vector<std::string>& insert_only = {}) const {
        std::ostringstream q;
        q << "INSERT INTO " << table << " (";
        for (size_t i = 0; i < cols_.size(); ++i) q << (i ? ", " : "") << cols_[i];
        q << ") VALUES (";
        for (size_t i = 0; i < vals_.size(); ++i) q << (i ? ", " : "") << vals_[i];
        q << ") ON DUPLICATE KEY UPDATE ";
        bool first = true;
        for (const auto& c : cols_) {
            if (std::find(insert_only.begin(), insert_only.end(), c) != insert_only.end())
                continue;
            q << (first ? "" : ", ") << c << " = VALUES(" << c << ")";
            first = false;
        }
        return q.str();
    }

private:
    db::Database& db_;
    std::vector<std::string> cols_, vals_;
};

std::optional<std::string> type_or_null(extract::ImageType t) {
    if (t == extract::ImageType::Unknown) return std::nullopt;
    return std::string(extract::to_string(t));
}

}  // namespace

StoreResult store_file(db::Database& db, const FileInfo& info,
                       const fits::RawHeader& header,
                       const extract::HeaderMapping& mapping,
                       const extract::SiteContext& site) {
    StoreResult result;

    // Case-folded path for the hash when the filesystem does not distinguish
    // case, so 'Lights/M31' and 'lights/M31' cannot become two rows.
    const std::string hash_path = info.case_sensitive ? info.rel_path : lower(info.rel_path);
    const std::string hash_sql = "UNHEX(MD5('" + db.escape(hash_path) + "'))";

    db.exec("START TRANSACTION");
    try {
        // ---- files ----
        Upsert f(db);
        f.inum("root_id", std::optional<int>(info.root_id));
        f.str("rel_path", std::optional<std::string>(info.rel_path));
        f.raw("rel_path_hash", hash_sql);
        f.str("filename", std::optional<std::string>(info.filename));
        f.str("ext", info.ext.empty() ? std::nullopt : std::optional<std::string>(info.ext));
        f.raw("format", "'" + db.escape(info.format) + "'");
        f.raw("bucket", "'" + db.escape(info.bucket) + "'");
        f.num("size_bytes", info.size_bytes);
        f.str("mtime_utc", info.mtime_utc);
        f.num("inode", info.inode);
        f.raw("status", "'ok'");
        f.raw("last_seen_utc", "UTC_TIMESTAMP()");
        f.raw("last_indexed_utc", "UTC_TIMESTAMP()");
        f.raw("first_seen_utc", "UTC_TIMESTAMP()");
        f.raw("error", "NULL");
        // first_seen stays put on re-index; a re-indexed file keeps its history.
        db.exec(f.build("files", {"first_seen_utc"}));

        // The upsert may not report an insert id on UPDATE, so read it back by
        // the unique key rather than trusting last_insert_id.
        auto rows = db.query(
            "SELECT id FROM files WHERE root_id = " + std::to_string(info.root_id) +
            " AND rel_path_hash = " + hash_sql + " LIMIT 1");
        if (rows.empty() || !rows[0][0])
            throw db::DbError("store_file: could not read back file id");
        const long long file_id = std::stoll(*rows[0][0]);
        result.file_id = file_id;

        // ---- frames + frame_keywords, one per image HDU ----
        for (const fits::Hdu* hdu : header.image_hdus()) {
            const auto rf = extract::resolve(*hdu, mapping, site);
            const std::string fp = fits::to_hex(fits::fingerprint(*hdu));

            Upsert fr(db);
            fr.num("file_id", std::optional<long long>(file_id));
            fr.inum("hdu", std::optional<int>(hdu->index));
            fr.raw("fingerprint", "UNHEX('" + fp + "')");
            fr.raw("image_type", type_or_null(rf.image_type)
                                     ? "'" + std::string(extract::to_string(rf.image_type)) + "'"
                                     : "'unknown'");
            fr.str("object", rf.object);
            fr.real("ra_deg", rf.ra_deg);
            fr.real("dec_deg", rf.dec_deg);
            fr.str("date_obs_utc", rf.date_obs_utc);
            fr.str("session_night", rf.session_night);
            fr.real("exposure_s", rf.exposure_s);
            fr.str("filter_raw", rf.filter_raw);
            fr.bit("filter_defaulted", rf.filter_defaulted);
            fr.str("instrume_raw", rf.instrume_raw);
            fr.str("telescope_raw", rf.telescope_raw);
            fr.real("focal_len_mm", rf.focal_len_mm);
            fr.inum("gain", rf.gain);
            fr.inum("offset_adu", rf.offset_adu);
            fr.inum("binx", rf.binx);
            fr.inum("biny", rf.biny);
            fr.inum("naxis1", rf.naxis1);
            fr.inum("naxis2", rf.naxis2);
            fr.real("ccd_temp_c", rf.ccd_temp_c);
            fr.real("set_temp_c", rf.set_temp_c);
            fr.real("airmass", rf.airmass);
            fr.str("pier_side", rf.pier_side);
            fr.str("row_order", rf.row_order);
            // A frame with no DATE-OBS or an unrecognized type is flagged so the
            // UI can surface it, rather than being silently mis-shelved.
            fr.str("quarantine_reason",
                   !rf.has_date ? std::optional<std::string>("no-dateobs")
                   : rf.image_type == extract::ImageType::Unknown
                       ? std::optional<std::string>("unknown-type")
                       : std::nullopt);
            db.exec(fr.build("frames"));

            auto frow = db.query("SELECT id FROM frames WHERE file_id = " +
                                 std::to_string(file_id) + " AND hdu = " +
                                 std::to_string(hdu->index) + " LIMIT 1");
            if (frow.empty() || !frow[0][0])
                throw db::DbError("store_file: could not read back frame id");
            const long long frame_id = std::stoll(*frow[0][0]);
            ++result.frames_written;

            // Replace the header cards: clear and reinsert, so a re-index tracks
            // any header change (e.g. a later SQM stamp) exactly.
            db.exec("DELETE FROM frame_keywords WHERE frame_id = " + std::to_string(frame_id));
            if (!hdu->cards.empty()) {
                std::ostringstream ins;
                ins << "INSERT INTO frame_keywords (frame_id, ord, keyword, value, comment) VALUES ";
                int ord = 0;
                bool first = true;
                for (const auto& c : hdu->cards) {
                    // ord is the primary key with frame_id; keep it unique even
                    // if a keyword repeats (HISTORY/COMMENT do, many times).
                    ins << (first ? "" : ", ") << "(" << frame_id << ", " << ord << ", '"
                        << db.escape(c.keyword.substr(0, 72)) << "', "
                        << (c.value.empty() ? "NULL" : "'" + db.escape(c.value.substr(0, 256)) + "'")
                        << ", "
                        << (c.comment.empty() ? "NULL" : "'" + db.escape(c.comment.substr(0, 256)) + "'")
                        << ")";
                    first = false;
                    ++ord;
                    ++result.keywords_written;
                }
                db.exec(ins.str());
            }
        }

        db.exec("COMMIT");
    } catch (...) {
        db.exec("ROLLBACK");
        throw;
    }

    return result;
}

}  // namespace starbase::index
