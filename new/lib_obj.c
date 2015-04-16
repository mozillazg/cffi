
/* A Lib object is what is in the "lib" attribute of a C extension
   module originally created by recompile().

   A Lib object is special in the sense that it has a custom
   __getattr__ which returns C globals, functions and constants.  It
   raises AttributeError for anything else, even attrs like '__class__'.

   A Lib object has got a reference to the _cffi_type_context_s
   structure, which is used to create lazily the objects returned by
   __getattr__.
*/

struct CPyExtFunc_s {
    PyMethodDef md;
    const struct _cffi_type_context_s *ctx;
    int type_index;
};

struct LibObject_s {
    PyObject_HEAD
    const struct _cffi_type_context_s *l_ctx;  /* ctx object */
    PyObject *l_dict;           /* content, built lazily */
    PyObject *l_libname;        /* some string that gives the name of the lib */
};

#define LibObject_Check(ob)  ((Py_TYPE(ob) == &Lib_Type))

static void lib_dealloc(LibObject *lib)
{
    Py_DECREF(lib->l_dict);
    Py_DECREF(lib->l_libname);
    PyObject_Del(lib);
}

static PyObject *lib_repr(LibObject *lib)
{
    return PyText_FromFormat("<cffi.Lib object for '%.200s'>",
                             PyText_AS_UTF8(lib->l_libname));
}

static PyObject *lib_build_cpython_func(LibObject *lib,
                                        const struct _cffi_global_s *g,
                                        const char *s, int flags)
{
    /* xxx the few bytes of memory we allocate here leak, but it's a
       minor concern because it should only occur for CPYTHON_BLTN.
       There is one per real C function in a CFFI C extension module.
       CPython never unloads its C extension modules anyway.
    */
    struct CPyExtFunc_s *xfunc = calloc(1, sizeof(struct CPyExtFunc_s));
    if (xfunc == NULL)
        goto no_memory;

    xfunc->md.ml_meth = (PyCFunction)g->address;
    xfunc->md.ml_flags = flags;
    xfunc->md.ml_name = g->name;
    /*xfunc->md.ml_doc = ... */
    if (xfunc->md.ml_name == NULL)
        goto no_memory;

    xfunc->ctx = lib->l_ctx;
    xfunc->type_index = _CFFI_GETARG(g->type_op);

    return PyCFunction_NewEx(&xfunc->md, NULL, lib->l_libname);

 no_memory:
    PyErr_NoMemory();
    return NULL;
}

static PyObject *lib_build_and_cache_attr(LibObject *lib, PyObject *name)
{
    /* does not return a new reference! */

    if (lib->l_ctx == NULL) {
        PyErr_Format(FFIError, "lib '%.200s' is already closed",
                     PyText_AS_UTF8(lib->l_libname));
        return NULL;
    }

    char *s = PyText_AsUTF8(name);
    if (s == NULL)
        return NULL;

    int index = search_in_globals(lib->l_ctx, s, strlen(s));
    if (index < 0) {
        PyErr_Format(PyExc_AttributeError,
                     "lib '%.200s' has no function,"
                     " global variable or constant named '%.200s'",
                     PyText_AS_UTF8(lib->l_libname),
                     PyText_Check(name) ? PyText_AS_UTF8(name) : "?");
        return NULL;
    }

    const struct _cffi_global_s *g = &lib->l_ctx->globals[index];
    PyObject *x;
    CTypeDescrObject *ct;

    switch (_CFFI_GETOP(g->type_op)) {

    case _CFFI_OP_CPYTHON_BLTN_V:
        x = lib_build_cpython_func(lib, g, s, METH_VARARGS);
        break;

    case _CFFI_OP_CPYTHON_BLTN_N:
        x = lib_build_cpython_func(lib, g, s, METH_NOARGS);
        break;

    case _CFFI_OP_CPYTHON_BLTN_O:
        x = lib_build_cpython_func(lib, g, s, METH_O);
        break;

    case _CFFI_OP_CONSTANT_INT:
    {
        /* a constant integer whose value, in an "unsigned long long",
           is obtained by calling the function at g->address */
        unsigned long long value;
        int neg = ((int(*)(unsigned long long*))g->address)(&value);
        if (!neg) {
            if (value <= (unsigned long long)LONG_MAX)
                x = PyInt_FromLong((long)value);
            else
                x = PyLong_FromUnsignedLongLong(value);
        }
        else {
            if ((long long)value >= (long long)LONG_MIN)
                x = PyInt_FromLong((long)value);
            else
                x = PyLong_FromLongLong((long long)value);
        }
        break;
    }

    case _CFFI_OP_CONSTANT:
    {
        /* a constant which is not of integer type */
        char *data;
        ct = realize_c_type(lib->l_ctx, lib->l_ctx->types,
                            _CFFI_GETARG(g->type_op));
        if (ct == NULL)
            return NULL;

        assert(ct->ct_size > 0);
        data = alloca(ct->ct_size);
        ((void(*)(char*))g->address)(data);
        x = convert_to_object(data, ct);
        Py_DECREF(ct);
        break;
    }

    case _CFFI_OP_GLOBAL_VAR:
        /* global variable of the exact type specified here */
        ct = realize_c_type(lib->l_ctx, lib->l_ctx->types,
                            _CFFI_GETARG(g->type_op));
        if (ct == NULL)
            return NULL;
        x = make_global_var(ct, g->address);
        Py_DECREF(ct);
        break;

    default:
        PyErr_SetString(PyExc_NotImplementedError, "in lib_build_attr");
        return NULL;
    }

    if (x != NULL) {
        int err = PyDict_SetItem(lib->l_dict, name, x);
        Py_DECREF(x);
        if (err < 0)     /* else there is still one ref left in the dict */
            return NULL;
    }
    return x;
}

static PyObject *lib_getattr(LibObject *lib, PyObject *name)
{
    PyObject *x = PyDict_GetItem(lib->l_dict, name);
    if (x == NULL) {
        x = lib_build_and_cache_attr(lib, name);
        if (x == NULL)
            return NULL;
    }

    if (GlobSupport_Check(x)) {
        return read_global_var((GlobSupportObject *)x);
    }
    Py_INCREF(x);
    return x;
}

static int lib_setattr(LibObject *lib, PyObject *name, PyObject *val)
{
    PyObject *x = PyDict_GetItem(lib->l_dict, name);
    if (x == NULL) {
        x = lib_build_and_cache_attr(lib, name);
        if (x == NULL)
            return -1;
    }

    if (val == NULL) {
        PyErr_SetString(PyExc_AttributeError, "C attribute cannot be deleted");
        return -1;
    }

    if (GlobSupport_Check(x)) {
        return write_global_var((GlobSupportObject *)x, val);
    }

    PyErr_Format(PyExc_AttributeError,
                 "cannot write to function or constant '%.200s'",
                 PyText_Check(name) ? PyText_AS_UTF8(name) : "?");
    return -1;
}

static PyObject *lib_dir(LibObject *lib, PyObject *noarg)
{
    const struct _cffi_global_s *g = lib->l_ctx->globals;
    int total = lib->l_ctx->num_globals;

    PyObject *lst = PyList_New(total);
    if (lst == NULL)
        return NULL;

    int i;
    for (i = 0; i < total; i++) {
        PyObject *s = PyString_FromString(g[i].name);
        if (s == NULL) {
            Py_DECREF(lst);
            return NULL;
        }
        PyList_SET_ITEM(lst, i, s);
    }
    return lst;
}

static PyMethodDef lib_methods[] = {
    {"__dir__",   (PyCFunction)lib_dir,  METH_NOARGS},
    {NULL,        NULL}           /* sentinel */
};

static PyTypeObject Lib_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "cffi.Lib",
    sizeof(LibObject),
    0,
    (destructor)lib_dealloc,                    /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_compare */
    (reprfunc)lib_repr,                         /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    (getattrofunc)lib_getattr,                  /* tp_getattro */
    (setattrofunc)lib_setattr,                  /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,                         /* tp_flags */
    0,                                          /* tp_doc */
    0,                                          /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    0,                                          /* tp_iter */
    0,                                          /* tp_iternext */
    lib_methods,                                /* tp_methods */
    0,                                          /* tp_members */
    0,                                          /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    offsetof(LibObject, l_dict),                /* tp_dictoffset */
};

static LibObject *lib_internal_new(const struct _cffi_type_context_s *ctx,
                                   char *module_name)
{
    LibObject *lib;
    PyObject *libname, *dict;

    libname = PyString_FromString(module_name);
    dict = PyDict_New();
    if (libname == NULL || dict == NULL) {
        Py_XDECREF(dict);
        Py_XDECREF(libname);
        return NULL;
    }

    lib = PyObject_New(LibObject, &Lib_Type);
    if (lib == NULL)
        return NULL;

    lib->l_ctx = ctx;
    lib->l_dict = dict;
    lib->l_libname = libname;
    return lib;
}