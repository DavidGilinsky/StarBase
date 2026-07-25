// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/query/query.cpp
// Purpose:       Implementation of the filter-AST to SQL compiler.
// Created:       2026-07-24
// Last Modified: 2026-07-24
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include "query.hpp"

#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace starbase::query {
namespace {

using json = nlohmann::json;

enum class Type { Str, Num, Date };

// The queryable columns of v_frames and their types. A field not in this map
// cannot be filtered, which is what keeps arbitrary column names and injection
// out of the compiled SQL.
const std::map<std::string, Type>& fields() {
    static const std::map<std::string, Type> m = {
        {"image_type", Type::Str},  {"object", Type::Str},   {"filter", Type::Str},
        {"rig", Type::Str},         {"camera", Type::Str},   {"site", Type::Str},
        {"bucket", Type::Str},      {"file_status", Type::Str}, {"root_label", Type::Str},
        {"pier_side", Type::Str},   {"row_order", Type::Str},
        {"session_night", Type::Date}, {"date_obs_utc", Type::Date},
        {"exposure_s", Type::Num},  {"gain", Type::Num},     {"offset_adu", Type::Num},
        {"binx", Type::Num},        {"biny", Type::Num},     {"ccd_temp_c", Type::Num},
        {"set_temp_c", Type::Num},  {"airmass", Type::Num},  {"sqm_mag_arcsec2", Type::Num},
        {"ra_deg", Type::Num},      {"dec_deg", Type::Num},  {"focus_pos", Type::Num},
        {"rotator_deg", Type::Num}, {"guide_rms_arcsec", Type::Num},
        {"naxis1", Type::Num},      {"naxis2", Type::Num},
    };
    return m;
}

// Binary comparison operators the AST may name -> SQL.
const std::map<std::string, std::string>& binops() {
    static const std::map<std::string, std::string> m = {
        {"eq", "="}, {"ne", "<>"}, {"lt", "<"}, {"lte", "<="},
        {"gt", ">"}, {"gte", ">="}, {"like", "LIKE"}};
    return m;
}

std::string require_string(const json& v, const std::string& what) {
    if (!v.is_string()) throw QueryError(what + " must be a string");
    return v.get<std::string>();
}

// Render one scalar value for a column of the given type. Numbers are validated
// and emitted as literals; strings and dates are escaped and quoted.
std::string literal(db::Database& db, Type t, const json& v) {
    if (t == Type::Num) {
        if (v.is_number()) {
            double d = v.get<double>();
            // Emit an integer without a trailing .0 when it is whole.
            if (d == std::floor(d) && std::abs(d) < 1e15)
                return std::to_string(static_cast<long long>(d));
            return std::to_string(d);
        }
        if (v.is_string()) {  // accept numeric strings from the UI
            const std::string s = v.get<std::string>();
            try { size_t p = 0; (void)std::stod(s, &p); if (p == s.size()) return s; }
            catch (const std::exception&) {}
            throw QueryError("numeric field needs a number, got '" + s + "'");
        }
        throw QueryError("numeric field needs a number");
    }
    // Str / Date
    if (v.is_string()) return "'" + db.escape(v.get<std::string>()) + "'";
    if (v.is_number()) return "'" + db.escape(std::to_string(v.get<double>())) + "'";
    throw QueryError("value must be a string for this field");
}

std::string compile_node(const json& node, db::Database& db);

// A header-card predicate: keyword:KW compiles to an EXISTS against
// frame_keywords correlated on the row's frame_id. keyword and value are
// escaped; the operator is from the fixed map. Numeric comparisons cast the
// stored text value so "< -5" works on FOCTEMP etc.
std::string compile_keyword(const std::string& field, const json& node, db::Database& db) {
    const std::string kw = field.substr(std::string("keyword:").size());
    if (kw.empty()) throw QueryError("empty keyword name");
    const std::string op = node.value("op", "eq");

    std::string cond;
    if (op == "isnull" || op == "notnull") {
        // Presence of the card, rather than a value comparison.
        const std::string ex = "EXISTS (SELECT 1 FROM frame_keywords k WHERE "
                               "k.frame_id = v_frames.frame_id AND k.keyword = '" +
                               db.escape(kw) + "')";
        return op == "isnull" ? "NOT " + ex : ex;
    }
    auto it = binops().find(op);
    if (it == binops().end()) throw QueryError("unsupported keyword operator '" + op + "'");
    if (!node.contains("value")) throw QueryError("keyword predicate needs a value");
    const json& v = node["value"];

    std::string rhs;
    if (v.is_number()) {
        // Compare numerically against the text-stored value.
        rhs = "CAST(k.value AS DECIMAL(20,6)) " + it->second + " " + literal(db, Type::Num, v);
    } else {
        rhs = "k.value " + it->second + " " + literal(db, Type::Str, v);
    }
    return "EXISTS (SELECT 1 FROM frame_keywords k WHERE k.frame_id = v_frames.frame_id "
           "AND k.keyword = '" + db.escape(kw) + "' AND " + rhs + ")";
}

// Great-circle cone: an indexed bounding-box prefilter (dec first, the tighter
// and non-wrapping constraint) AND an exact haversine distance test.
std::string compile_cone(const json& node, db::Database& db) {
    (void)db;
    if (!node.contains("ra") || !node.contains("dec") || !node.contains("radius_deg"))
        throw QueryError("cone needs ra, dec, and radius_deg");
    const double ra = node["ra"].get<double>();
    const double dec = node["dec"].get<double>();
    const double r = node["radius_deg"].get<double>();
    if (r <= 0 || r > 180) throw QueryError("cone radius_deg out of range");

    // Bounding box. RA half-width grows with declination; clamp near the poles.
    const double dec_lo = dec - r, dec_hi = dec + r;
    const double cosd = std::cos(dec * M_PI / 180.0);
    const double ra_half = (std::abs(cosd) < 1e-6 || std::abs(dec) + r >= 90.0)
                               ? 180.0 : r / std::abs(cosd);
    auto n = [](double x) { return std::to_string(x); };
    std::string bbox = "dec_deg BETWEEN " + n(dec_lo) + " AND " + n(dec_hi);
    if (ra_half < 180.0)
        bbox += " AND ra_deg BETWEEN " + n(ra - ra_half) + " AND " + n(ra + ra_half);

    // Haversine central angle <= r (all in degrees via RADIANS()).
    std::string hav =
        "DEGREES(2*ASIN(SQRT(POWER(SIN(RADIANS(dec_deg-" + n(dec) + ")/2),2) + "
        "COS(RADIANS(" + n(dec) + "))*COS(RADIANS(dec_deg))*"
        "POWER(SIN(RADIANS(ra_deg-" + n(ra) + ")/2),2)))) <= " + n(r);
    return "(ra_deg IS NOT NULL AND dec_deg IS NOT NULL AND (" + bbox + ") AND " + hav + ")";
}

// Membership in frame_tags by tag name (string) or id (number), as a correlated
// EXISTS on the outer v_frames row. `untagged` negates it.
std::string compile_tagged(const json& node, db::Database& db, bool negate) {
    if (!node.contains("value")) throw QueryError("tagged needs a tag name or id");
    const json& v = node["value"];
    std::string cond;
    if (v.is_number()) cond = "ft.tag_id = " + std::to_string(v.get<long long>());
    else if (v.is_string()) cond = "t.name = '" + db.escape(v.get<std::string>()) + "'";
    else throw QueryError("tag value must be a name or id");
    const std::string ex =
        "EXISTS (SELECT 1 FROM frame_tags ft JOIN tags t ON t.id = ft.tag_id "
        "WHERE ft.frame_id = v_frames.frame_id AND " + cond + ")";
    return negate ? "NOT " + ex : ex;
}

// Membership in a collection by name or id.
std::string compile_in_collection(const json& node, db::Database& db) {
    if (!node.contains("value")) throw QueryError("in_collection needs a name or id");
    const json& v = node["value"];
    std::string cond;
    if (v.is_number()) cond = "cf.collection_id = " + std::to_string(v.get<long long>());
    else if (v.is_string()) cond = "c.name = '" + db.escape(v.get<std::string>()) + "'";
    else throw QueryError("collection value must be a name or id");
    return "EXISTS (SELECT 1 FROM collection_frames cf JOIN collections c "
           "ON c.id = cf.collection_id WHERE cf.frame_id = v_frames.frame_id AND " + cond + ")";
}

std::string compile_comparison(const json& node, db::Database& db) {
    const std::string field = require_string(node.at("field"), "field");
    if (field.rfind("keyword:", 0) == 0) return compile_keyword(field, node, db);

    auto ft = fields().find(field);
    if (ft == fields().end()) throw QueryError("unknown field '" + field + "'");
    const Type type = ft->second;
    const std::string op = node.value("op", "eq");

    if (op == "isnull") return field + " IS NULL";
    if (op == "notnull") return field + " IS NOT NULL";

    if (op == "in") {
        const json& v = node.at("value");
        if (!v.is_array() || v.empty()) throw QueryError("'in' needs a non-empty array");
        std::string list;
        for (size_t i = 0; i < v.size(); ++i)
            list += (i ? ", " : "") + literal(db, type, v[i]);
        return field + " IN (" + list + ")";
    }
    if (op == "between") {
        const json& v = node.at("value");
        if (!v.is_array() || v.size() != 2) throw QueryError("'between' needs [lo, hi]");
        return field + " BETWEEN " + literal(db, type, v[0]) + " AND " + literal(db, type, v[1]);
    }

    auto it = binops().find(op);
    if (it == binops().end()) throw QueryError("unsupported operator '" + op + "'");
    if (!node.contains("value")) throw QueryError("operator '" + op + "' needs a value");
    return field + " " + it->second + " " + literal(db, type, node.at("value"));
}

std::string compile_node(const json& node, db::Database& db) {
    if (!node.is_object()) throw QueryError("each filter node must be an object");
    const std::string op = node.value("op", "");

    if (op == "and" || op == "or") {
        const json& clauses = node.at("clauses");
        if (!clauses.is_array() || clauses.empty())
            throw QueryError("'" + op + "' needs a non-empty clauses array");
        const std::string join = (op == "and") ? " AND " : " OR ";
        std::string out = "(";
        for (size_t i = 0; i < clauses.size(); ++i)
            out += (i ? join : "") + compile_node(clauses[i], db);
        return out + ")";
    }
    if (op == "not") {
        const json& clauses = node.at("clauses");
        if (!clauses.is_array() || clauses.size() != 1)
            throw QueryError("'not' needs exactly one clause");
        return "NOT (" + compile_node(clauses[0], db) + ")";
    }
    if (op == "cone") return compile_cone(node, db);
    if (op == "tagged" || op == "untagged") return compile_tagged(node, db, op == "untagged");
    if (op == "in_collection") return compile_in_collection(node, db);

    return compile_comparison(node, db);
}

}  // namespace

std::string compile_filter(const json& ast, db::Database& db) {
    if (ast.is_null() || (ast.is_object() && ast.empty())) return "1=1";
    return compile_node(ast, db);
}

std::string compile_sort(const json& sort) {
    if (sort.is_null() || !sort.is_array() || sort.empty()) return "date_obs_utc DESC";
    std::string out;
    for (const auto& s : sort) {
        const std::string field = s.value("field", "");
        if (!fields().count(field)) throw QueryError("cannot sort by '" + field + "'");
        std::string dir = s.value("dir", "asc");
        for (auto& c : dir) c = static_cast<char>(std::tolower(c));
        if (dir != "asc" && dir != "desc") throw QueryError("sort dir must be asc or desc");
        out += (out.empty() ? "" : ", ") + field + " " + (dir == "desc" ? "DESC" : "ASC");
    }
    return out;
}

}  // namespace starbase::query
