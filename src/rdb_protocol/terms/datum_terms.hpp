#include "rdb_protocol/op.hpp"
namespace ql {

class datum_term_t : public term_t {
public:
    datum_term_t(env_t *env, const Datum *datum)
        : term_t(env), raw_val(new val_t(new datum_t(datum), this)) {
        guarantee(raw_val.has());
    }
private:
    virtual val_t *eval_impl() { return raw_val.get(); }
    RDB_NAME("datum")
    scoped_ptr_t<val_t> raw_val;
};

class make_array_term_t : public op_term_t {
public:
    make_array_term_t(env_t *env, const Term2 *term)
        : op_term_t(env, term, argspec_t(0, -1)) { }
private:
    virtual val_t *eval_impl() {
        scoped_ptr_t<datum_t> acc(new datum_t(datum_t::R_ARRAY));
        for (size_t i = 0; i < num_args(); ++i) acc->add(arg(i)->as_datum());
        return new_val(acc.release());
    }
    RDB_NAME("make_array")
};

class make_obj_term_t : public op_term_t {
public:
    make_obj_term_t(env_t *env, const Term2 *term)
        : op_term_t(env, term, argspec_t(0), optargspec_t::make_object()) { }
private:
    virtual val_t *eval_impl() {
        scoped_ptr_t<datum_t> acc(new datum_t(datum_t::R_OBJECT));
        for (boost::ptr_map<const std::string, term_t>::iterator
                 it = optargs.begin(); it != optargs.end(); ++it) {
            bool dup = acc->add(it->first, it->second->eval(use_cached_val)->as_datum());
            rcheck(!dup, strprintf("Duplicate key in object: %s.", it->first.c_str()));
        }
        return new_val(acc.release());
    }
    RDB_NAME("make_obj")
};

}
