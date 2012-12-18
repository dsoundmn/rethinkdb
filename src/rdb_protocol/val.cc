// Copyright 2010-2012 RethinkDB, all rights reserved.
#include "clustering/administration/metadata.hpp"
#include "rdb_protocol/val.hpp"
#include "rdb_protocol/env.hpp"
#include "rdb_protocol/term.hpp"

namespace ql {
static const char *valid_char_msg = name_string_t::valid_char_msg;

datum_stream_t::datum_stream_t(env_t *_env, bool use_outdated,
                               namespace_repo_t<rdb_protocol_t>::access_t *ns_access)
    : env(_env),
      json_stream(new query_language::batched_rget_stream_t(
                      *ns_access, _env->interruptor, key_range_t::universe(),
                      100, use_outdated))
{ }

datum_stream_t::datum_stream_t(datum_stream_t *src, func_t *f)
    : env(src->env), json_stream(src->json_stream) {
    query_language::scopes_t _s;
    query_language::backtrace_t _b;
    rdb_protocol_details::transform_variant_t trans(wire_func_t(env, f));
    json_stream = json_stream->add_transformation(trans, 0, env, _s, _b);
}

const datum_t *datum_stream_t::next() {
    if (last.has()) register_data(last.release());
    boost::shared_ptr<scoped_cJSON_t> json = json_stream->next();
    if (!json.get()) return 0;
    last.init(new datum_t(json));
    return last.get();
}

void datum_stream_t::free_last_datum() {
    r_sanity_check(last.has());
    delete last.release();
}


static void meta_check(metadata_search_status_t status, metadata_search_status_t want,
                       const std::string &operation) {
    if (status != want) {
        const char *msg;
        switch (status) {
        case METADATA_SUCCESS: msg = "Entry already exists."; break;
        case METADATA_ERR_NONE: msg = "No entry with that name."; break;
        case METADATA_ERR_MULTIPLE: msg = "Multiple entries with that name."; break;
        case METADATA_CONFLICT: msg = "Entry with that name is in conflict."; break;
        default:
            unreachable();
        }
        throw ql::exc_t(strprintf("Error during operation `%s`: %s",
                                  operation.c_str(), msg));
    }
}

template<class T, class U>
static uuid_t meta_get_uuid(T searcher, const U &predicate, std::string operation) {
    metadata_search_status_t status;
    typename T::iterator entry = searcher.find_uniq(predicate, &status);
    meta_check(status, METADATA_SUCCESS, operation);
    return entry->first;
}

table_t::table_t(env_t *_env, uuid_t db_id, const std::string &name, bool _use_outdated)
    : env(_env), use_outdated(_use_outdated) {
    name_string_t table_name;
    bool b = table_name.assign_value(name);
    rcheck(b, strprintf("table name %s invalid (%s)", name.c_str(), valid_char_msg));
    cow_ptr_t<namespaces_semilattice_metadata_t<rdb_protocol_t> >
        namespaces_metadata = env->namespaces_semilattice_metadata->get();
    cow_ptr_t<namespaces_semilattice_metadata_t<rdb_protocol_t> >::change_t
        namespaces_metadata_change(&namespaces_metadata);
    metadata_searcher_t<namespace_semilattice_metadata_t<rdb_protocol_t> >
        ns_searcher(&namespaces_metadata_change.get()->namespaces);
    namespace_predicate_t pred(&table_name, &db_id);
    uuid_t id = meta_get_uuid(ns_searcher, pred, "EVAL_TABLE " + table_name.str());

    access.init(new namespace_repo_t<rdb_protocol_t>::access_t(
                    env->ns_repo, id, env->interruptor));
}

datum_stream_t *table_t::as_datum_stream() {
    return new datum_stream_t(env, use_outdated, access.get());
}

val_t::type_t::type_t(val_t::type_t::raw_type_t _raw_type) : raw_type(_raw_type) { }

bool raw_type_is_convertible(val_t::type_t::raw_type_t _t1,
                             val_t::type_t::raw_type_t _t2) {
    const int t1 = _t1, t2 = _t2,
        DB = val_t::type_t::DB,
        TABLE = val_t::type_t::TABLE,
        SELECTION = val_t::type_t::SELECTION,
        SEQUENCE = val_t::type_t::SEQUENCE,
        SINGLE_SELECTION = val_t::type_t::SINGLE_SELECTION,
        DATUM = val_t::type_t::DATUM,
        FUNC = val_t::type_t::FUNC;
    switch(t1) {
    case DB: return t2 == DB;
    case TABLE: return t2 == TABLE || t2 == SELECTION || t2 == SEQUENCE;
    case SELECTION: return t2 == SELECTION || t2 == SEQUENCE;
    case SEQUENCE: return t2 == SEQUENCE;
    case SINGLE_SELECTION: return t2 == SINGLE_SELECTION || t2 == DATUM;
    case DATUM: return t2 == DATUM || t2 == SEQUENCE;
    case FUNC: return t2 == FUNC;
    default: unreachable();
    }
    unreachable();
}
bool val_t::type_t::is_convertible(type_t rhs) const {
    return raw_type_is_convertible(raw_type, rhs.raw_type);
}

const char *val_t::type_t::name() const {
    switch(raw_type) {
    case DB: return "DATABASE";
    case TABLE: return "TABLE";
    case SELECTION: return "SELECTION";
    case SEQUENCE: return "SEQUENCE";
    case SINGLE_SELECTION: return "SINGLE_SELECTION";
    case DATUM: return "DATUM";
    case FUNC: return "FUNCTION";
    default: unreachable();
    }
    unreachable();
}

val_t::val_t(const datum_t *_datum, const term_t *_parent)
    : type(type_t::DATUM), datum(_datum), parent(_parent) {
    guarantee(_datum);
}
val_t::val_t(table_t *_table, const term_t *_parent)
    : type(type_t::TABLE), table(_table), parent(_parent) {
    guarantee(_table);
}
val_t::val_t(uuid_t _db, const term_t *_parent)
    : type(type_t::DB), db(_db), parent(_parent) {
}
val_t::val_t(func_t *_func, const term_t *_parent)
    : type(type_t::FUNC), func(_func), parent(_parent) {
}

uuid_t get_db_uuid(env_t *env, const std::string &dbs) {
    name_string_t db_name;
    bool b = db_name.assign_value(dbs);
    rcheck(b, strprintf("db name %s invalid (%s)", dbs.c_str(), valid_char_msg));
    databases_semilattice_metadata_t
        databases_metadata = env->databases_semilattice_metadata->get();
    metadata_searcher_t<database_semilattice_metadata_t>
        db_searcher(&databases_metadata.databases);
    return meta_get_uuid(db_searcher, db_name, "EVAL_DB " + db_name.str());
}

val_t::type_t val_t::get_type() const { return type; }

const datum_t *val_t::as_datum() {
    rcheck(type.raw_type == type_t::DATUM,
           strprintf("Type error: cannot convert %s to DATUM.", type.name()));
    return datum.get();
}

table_t *val_t::as_table() {
    rcheck(type.raw_type == type_t::TABLE,
           strprintf("Type error: cannot convert %s to TABLE.", type.name()));
    return table.get();
}

datum_stream_t *val_t::as_seq() {
    if (type.raw_type == type_t::SEQUENCE || type.raw_type == type_t::SELECTION) {
        // passthru
    } else if (type.raw_type == type_t::TABLE) {
        if (!sequence.has()) sequence.init(table.get()->as_datum_stream());
    } else {
        rfail("Type error: cannot convert %s to SEQUENCE.", type.name());
    }
    return sequence.get();
}

func_t *val_t::as_func() {
    rcheck(type.raw_type == type_t::FUNC,
           strprintf("Type error: cannot convert %s to FUNC.", type.name()));
    return func.get();
}

uuid_t val_t::as_db() {
    rcheck(type.raw_type == type_t::DB,
           strprintf("Type error: cannot convert %s to DB.", type.name()));
    return db;
}

} //namespace ql
