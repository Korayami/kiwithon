/* _kiwi_markdownmodule.c: Native C11 Markdown + GFM + KaTeX + HTML allowlist
 * sanitizer module for Kiwithon / KiwiSpaceONE.
 */

#ifndef Py_BUILD_CORE_BUILTIN
#  define Py_BUILD_CORE_MODULE 1
#endif

#include "Python.h"
#include "_kiwi_md4c/kiwi_md.h"

#include <stdlib.h>
#include <string.h>

static int
extract_extra_tags(PyObject *extra_tags_obj, uint64_t **out_tags, size_t *out_count)
{
    *out_tags = NULL;
    *out_count = 0;
    if (extra_tags_obj == NULL || extra_tags_obj == Py_None) {
        return 0;
    }

    PyObject *seq = PySequence_Fast(extra_tags_obj, "extra_tags must be an iterable of strings");
    if (seq == NULL) {
        return -1;
    }

    Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
    if (n == 0) {
        Py_DECREF(seq);
        return 0;
    }

    uint64_t *tags = (uint64_t *)malloc((size_t)n * sizeof(uint64_t));
    if (!tags) {
        Py_DECREF(seq);
        PyErr_NoMemory();
        return -1;
    }

    size_t count = 0;
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *item = PySequence_Fast_GET_ITEM(seq, i);
        if (!PyUnicode_Check(item)) {
            free(tags);
            Py_DECREF(seq);
            PyErr_SetString(PyExc_TypeError, "extra_tags elements must be strings");
            return -1;
        }
        Py_ssize_t slen = 0;
        const char *s = PyUnicode_AsUTF8AndSize(item, &slen);
        if (!s) {
            free(tags);
            Py_DECREF(seq);
            return -1;
        }
        uint64_t packed = kiwi_md_pack_tag(s, (size_t)slen);
        if (packed != UINT64_MAX) {
            tags[count++] = packed;
        }
    }

    Py_DECREF(seq);
    *out_tags = tags;
    *out_count = count;
    return 0;
}

/* _kiwi_markdown.render(text, sanitize=True, math=True, extra_tags=None) -> str */
static PyObject *
kiwi_markdown_render(PyObject *Py_UNUSED(module),
                     PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    static const char * const kwlist[] = {"text", "sanitize", "math", "extra_tags", NULL};
    PyObject *text_obj = NULL;
    int sanitize = 1;
    int math = 1;
    PyObject *extra_tags_obj = NULL;

    if (nargs > 4) {
        PyErr_SetString(PyExc_TypeError,
                        "render() takes at most 4 positional arguments");
        return NULL;
    }
    if (nargs >= 1) {
        text_obj = args[0];
    }
    if (nargs >= 2) {
        sanitize = PyObject_IsTrue(args[1]);
        if (sanitize < 0) return NULL;
    }
    if (nargs >= 3) {
        math = PyObject_IsTrue(args[2]);
        if (math < 0) return NULL;
    }
    if (nargs >= 4) {
        extra_tags_obj = args[3];
    }

    if (kwnames != NULL) {
        Py_ssize_t nkw = PyTuple_GET_SIZE(kwnames);
        for (Py_ssize_t i = 0; i < nkw; i++) {
            PyObject *kw = PyTuple_GET_ITEM(kwnames, i);
            PyObject *val = args[nargs + i];
            if (PyUnicode_CompareWithASCIIString(kw, kwlist[0]) == 0) {
                if (nargs >= 1) {
                    PyErr_SetString(PyExc_TypeError,
                                    "render() got multiple values for argument 'text'");
                    return NULL;
                }
                text_obj = val;
            }
            else if (PyUnicode_CompareWithASCIIString(kw, kwlist[1]) == 0) {
                if (nargs >= 2) {
                    PyErr_SetString(PyExc_TypeError,
                                    "render() got multiple values for argument 'sanitize'");
                    return NULL;
                }
                sanitize = PyObject_IsTrue(val);
                if (sanitize < 0) return NULL;
            }
            else if (PyUnicode_CompareWithASCIIString(kw, kwlist[2]) == 0) {
                if (nargs >= 3) {
                    PyErr_SetString(PyExc_TypeError,
                                    "render() got multiple values for argument 'math'");
                    return NULL;
                }
                math = PyObject_IsTrue(val);
                if (math < 0) return NULL;
            }
            else if (PyUnicode_CompareWithASCIIString(kw, kwlist[3]) == 0) {
                if (nargs >= 4) {
                    PyErr_SetString(PyExc_TypeError,
                                    "render() got multiple values for argument 'extra_tags'");
                    return NULL;
                }
                extra_tags_obj = val;
            }
            else {
                PyErr_Format(PyExc_TypeError,
                             "render() got an unexpected keyword argument %R", kw);
                return NULL;
            }
        }
    }

    if (text_obj == NULL) {
        PyErr_SetString(PyExc_TypeError, "render() missing required argument 'text'");
        return NULL;
    }
    if (!PyUnicode_Check(text_obj)) {
        PyErr_SetString(PyExc_TypeError, "render() argument 'text' must be str");
        return NULL;
    }

    Py_ssize_t input_len = 0;
    const char *input = PyUnicode_AsUTF8AndSize(text_obj, &input_len);
    if (input == NULL) {
        return NULL;
    }
    if (input_len == 0) {
        return PyUnicode_FromStringAndSize("", 0);
    }

    uint64_t *extra_tags = NULL;
    size_t extra_tag_count = 0;
    if (extract_extra_tags(extra_tags_obj, &extra_tags, &extra_tag_count) < 0) {
        return NULL;
    }

    KiwiMdOptions opts;
    opts.sanitize = (sanitize != 0);
    opts.math = (math != 0);
    opts.extra_tags = extra_tags;
    opts.extra_tag_count = extra_tag_count;

    KiwiMdBuffer out;
    if (kiwi_md_buf_init(&out, (size_t)input_len + 128) < 0) {
        free(extra_tags);
        return PyErr_NoMemory();
    }

    int rc;
    Py_BEGIN_ALLOW_THREADS
    rc = kiwi_md_render(input, (size_t)input_len, &opts, &out);
    Py_END_ALLOW_THREADS

    free(extra_tags);
    if (rc < 0) {
        kiwi_md_buf_free(&out);
        return PyErr_NoMemory();
    }

    PyObject *result = PyUnicode_DecodeUTF8(out.data, (Py_ssize_t)out.len, "replace");
    kiwi_md_buf_free(&out);
    return result;
}

/* _kiwi_markdown.sanitize_html(html_str, extra_tags=None) -> str */
static PyObject *
kiwi_markdown_sanitize_html(PyObject *Py_UNUSED(module),
                            PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    static const char * const kwlist[] = {"html_str", "extra_tags", NULL};
    PyObject *html_obj = NULL;
    PyObject *extra_tags_obj = NULL;

    if (nargs > 2) {
        PyErr_SetString(PyExc_TypeError,
                        "sanitize_html() takes at most 2 positional arguments");
        return NULL;
    }
    if (nargs >= 1) {
        html_obj = args[0];
    }
    if (nargs >= 2) {
        extra_tags_obj = args[1];
    }

    if (kwnames != NULL) {
        Py_ssize_t nkw = PyTuple_GET_SIZE(kwnames);
        for (Py_ssize_t i = 0; i < nkw; i++) {
            PyObject *kw = PyTuple_GET_ITEM(kwnames, i);
            PyObject *val = args[nargs + i];
            if (PyUnicode_CompareWithASCIIString(kw, kwlist[0]) == 0) {
                if (nargs >= 1) {
                    PyErr_SetString(PyExc_TypeError,
                                    "sanitize_html() got multiple values for argument 'html_str'");
                    return NULL;
                }
                html_obj = val;
            }
            else if (PyUnicode_CompareWithASCIIString(kw, kwlist[1]) == 0) {
                if (nargs >= 2) {
                    PyErr_SetString(PyExc_TypeError,
                                    "sanitize_html() got multiple values for argument 'extra_tags'");
                    return NULL;
                }
                extra_tags_obj = val;
            }
            else {
                PyErr_Format(PyExc_TypeError,
                             "sanitize_html() got an unexpected keyword argument %R", kw);
                return NULL;
            }
        }
    }

    if (html_obj == NULL) {
        PyErr_SetString(PyExc_TypeError,
                        "sanitize_html() missing required argument 'html_str'");
        return NULL;
    }
    if (!PyUnicode_Check(html_obj)) {
        PyErr_SetString(PyExc_TypeError, "sanitize_html() argument 'html_str' must be str");
        return NULL;
    }

    Py_ssize_t input_len = 0;
    const char *input = PyUnicode_AsUTF8AndSize(html_obj, &input_len);
    if (input == NULL) {
        return NULL;
    }
    if (input_len == 0) {
        return PyUnicode_FromStringAndSize("", 0);
    }

    uint64_t *extra_tags = NULL;
    size_t extra_tag_count = 0;
    if (extract_extra_tags(extra_tags_obj, &extra_tags, &extra_tag_count) < 0) {
        return NULL;
    }

    KiwiMdBuffer out;
    if (kiwi_md_buf_init(&out, (size_t)input_len + 64) < 0) {
        free(extra_tags);
        return PyErr_NoMemory();
    }

    int rc;
    Py_BEGIN_ALLOW_THREADS
    rc = kiwi_md_sanitize_html(input, (size_t)input_len,
                               extra_tags, extra_tag_count, &out);
    Py_END_ALLOW_THREADS

    free(extra_tags);
    if (rc < 0) {
        kiwi_md_buf_free(&out);
        return PyErr_NoMemory();
    }

    PyObject *result = PyUnicode_DecodeUTF8(out.data, (Py_ssize_t)out.len, "replace");
    kiwi_md_buf_free(&out);
    return result;
}

PyDoc_STRVAR(_kiwi_markdown_doc,
"Kiwithon native C11 CommonMark + GFM + KaTeX + HTML sanitizer engine.");

static PyMethodDef _kiwi_markdown_methods[] = {
    {"render", _PyCFunction_CAST(kiwi_markdown_render),
     METH_FASTCALL | METH_KEYWORDS,
     PyDoc_STR("render(text, *, sanitize=True, math=True, extra_tags=None) -> str\n--\n\n"
               "Render Markdown (CommonMark + GFM + KaTeX math protection) into safe HTML.")},
    {"render_markdown", _PyCFunction_CAST(kiwi_markdown_render),
     METH_FASTCALL | METH_KEYWORDS,
     PyDoc_STR("render_markdown(text, *, sanitize=True, math=True, extra_tags=None) -> str\n--\n\n"
               "Alias for render().")},
    {"sanitize_html", _PyCFunction_CAST(kiwi_markdown_sanitize_html),
     METH_FASTCALL | METH_KEYWORDS,
     PyDoc_STR("sanitize_html(html_str, *, extra_tags=None) -> str\n--\n\n"
               "Sanitize HTML string using KiwiSpaceONE's O(1) 64-bit allowlist classifier.")},
    {NULL, NULL, 0, NULL}
};

static PyModuleDef_Slot _kiwi_markdown_slots[] = {
    _Py_ABI_SLOT,
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
    {0, NULL}
};

static struct PyModuleDef _kiwi_markdownmodule = {
    .m_base = PyModuleDef_HEAD_INIT,
    .m_name = "_kiwi_markdown",
    .m_doc = _kiwi_markdown_doc,
    .m_size = 0,
    .m_methods = _kiwi_markdown_methods,
    .m_slots = _kiwi_markdown_slots,
};

PyMODINIT_FUNC
PyInit__kiwi_markdown(void)
{
    return PyModuleDef_Init(&_kiwi_markdownmodule);
}
