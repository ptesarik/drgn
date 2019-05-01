# Copyright 2018-2019 - Omar Sandoval
# SPDX-License-Identifier: GPL-3.0+

import os.path
import re
import sys


def gen_constant_class(drgn_h, output_file, class_name, enum_class, regex):
    matches = re.findall(r'^\s*(' + regex + r')\s*[=,]',
                         drgn_h, flags=re.MULTILINE)
    output_file.write(f"""
static int add_{class_name}(PyObject *m, PyObject *enum_module)
{{
	PyObject *tmp, *item;
	int ret = -1;

	tmp = PyList_New({len(matches)});
	if (!tmp)
		goto out;
""")
    for i, groups in enumerate(matches):
        output_file.write(f"""\
        item = Py_BuildValue("sk", "{'_'.join(groups[1:])}", {groups[0]});
	if (!item)
		goto out;
	PyList_SET_ITEM(tmp, {i}, item);
""")
    output_file.write(f"""\
	{class_name}_class = PyObject_CallMethod(enum_module, "{enum_class}", "sO", "{class_name}", tmp);
	if (!{class_name}_class)
		goto out;
	if (PyModule_AddObject(m, "{class_name}", {class_name}_class) == -1) {{
		Py_CLEAR({class_name}_class);
		goto out;
	}}
	Py_DECREF(tmp);
	tmp = PyUnicode_FromString(drgn_{class_name}_DOC);
	if (!tmp)
		goto out;
	if (PyObject_SetAttrString({class_name}_class, "__doc__", tmp) == -1)
		goto out;

	ret = 0;
out:
	Py_XDECREF(tmp);
	return ret;
}}
""")


def gen_constants(input_file, output_file, header_directory=None):
    drgn_h = input_file.read()
    output_file.write(f"""\
/* Generated by libdrgn/build-aux/gen_constants.py. */

#include "{os.path.join(header_directory or '', 'drgnpy.h')}"

PyObject *FindObjectFlags_class;
PyObject *PrimitiveType_class;
PyObject *ProgramFlags_class;
PyObject *Qualifiers_class;
PyObject *TypeKind_class;
""")
    gen_constant_class(drgn_h, output_file, 'FindObjectFlags', 'Flag',
                       r'DRGN_FIND_OBJECT_([a-zA-Z0-9_]+)')
    gen_constant_class(drgn_h, output_file, 'PrimitiveType', 'Enum',
                       r'DRGN_(C)_TYPE_([a-zA-Z0-9_]+)')
    gen_constant_class(drgn_h, output_file, 'ProgramFlags', 'Flag',
                       r'DRGN_PROGRAM_([a-zA-Z0-9_]+)(?<!DRGN_PROGRAM_ENDIAN)')
    gen_constant_class(drgn_h, output_file, 'Qualifiers', 'Flag',
                       r'DRGN_QUALIFIER_([a-zA-Z0-9_]+)')
    gen_constant_class(drgn_h, output_file, 'TypeKind', 'Enum',
                       r'DRGN_TYPE_([a-zA-Z0-9_]+)')
    output_file.write("""
int add_module_constants(PyObject *m)
{
	PyObject *enum_module;
	int ret;

	enum_module = PyImport_ImportModule("enum");
	if (!enum_module)
		return -1;

	if (add_FindObjectFlags(m, enum_module) == -1 ||
	    add_PrimitiveType(m, enum_module) == -1 ||
	    add_ProgramFlags(m, enum_module) == -1 ||
	    add_Qualifiers(m, enum_module) == -1 ||
	    add_TypeKind(m, enum_module) == -1)
		ret = -1;
	else
		ret = 0;
	Py_DECREF(enum_module);
	return ret;
}
""")


if __name__ == '__main__':
    gen_constants(sys.stdin, sys.stdout,
                  sys.argv[1] if len(sys.argv) >= 2 else None)