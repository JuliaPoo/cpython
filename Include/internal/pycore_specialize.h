#ifndef Py_INTERNAL_SPECIALIZE
#define Py_INTERNAL_SPECIALIZE
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

extern void _PyCCache_Init(PyInterpreterState *interp);

typedef struct {
    uint32_t version;
    uint16_t index;
    /* We don't need an actual counter because the C call sites are even more static
       than Python's. This just indicates if values are populated.
    */
    uint8_t counter;
} _PyCAttrCache;

#define CACHE_LOAD_MODULE_DICT_ATTR(var, interp, mod_dict, attr_name, cache_name) \
    do { \
    PyInterpreterState *_interp = interp; \
    PyDictObject *_mod_dict =  (PyDictObject *)mod_dict; \
    _PyCAttrCache *cache = &_interp->c_cache.##cache_name; \
    var = _mod_dict ? load_module_dict_attr(cache, _mod_dict, attr_name) : NULL; \
    } while (0)

extern PyObject *load_module_dict_attr(_PyCAttrCache *cache, PyDictObject *mod_dict, PyObject *name);

/* TODO: Autogenerate these similar to _Py_ID */
struct _Py_c_cache {
    _PyCAttrCache bltinmodule_sys_stdout;
};

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_SPECIALIZE */
