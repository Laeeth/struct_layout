/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Yonatan Goldschmidt
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/* skeleton taken from structsizes.cc plugin by Richard W.M. Jones
   https://rwmj.wordpress.com/2016/02/24/playing-with-gcc-plugins/ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gcc-plugin.h>
#include <tree.h>
#include <print-tree.h>


int plugin_is_GPL_compatible; // must be defined for the plugin to run

static FILE *output_file;
static const char *target_struct = NULL;

// taken from linux/scripts/gcc-plugins/randomize_layout_plugin.c
#define ORIG_TYPE_NAME(node) \
    (TYPE_NAME(TYPE_MAIN_VARIANT(node)) != NULL_TREE ? ((const char *)IDENTIFIER_POINTER(TYPE_NAME(TYPE_MAIN_VARIANT(node)))) : NULL)

static void debug_tree_helper(tree t, const char *msg)
{
#ifndef NDEBUG
    printf("!!!!!!!! %s\n", msg);
    debug_tree(t);
    printf("\n\n");
    fflush(stdout);
#endif
}

struct name_list {
    struct name_list *next;
    char name[0];
};

static struct name_list dumped_structs;

static bool was_dumped(const char *name)
{
    const struct name_list *iter = dumped_structs.next;

    while (NULL != iter) {
        if (0 == strcmp(name, iter->name)) {
            return true;
        }
        iter = iter->next;
    }

    return false;
}

static void add_dumped(const char *name)
{
    const size_t len = strlen(name) + 1;
    struct name_list *n = (struct name_list*)xmalloc(sizeof(*n) + len);
    memcpy(n->name, name, len);

    struct name_list *iter = &dumped_structs;

    while (NULL != iter->next) {
        iter = iter->next;
    }

    iter->next = n;
}

// allowed inner types for array elemented and pointees
static bool is_allowed_inner_type(tree type)
{
    switch (TREE_CODE(type)) {
    case INTEGER_TYPE:
        return true;

    default:
        return false;
    }
}

static void plugin_finish_type(void *event_data, void *user_data)
{
    tree type = (tree)event_data;

    if (TREE_CODE(type) != RECORD_TYPE // it is a struct tree
        || TYPE_FIELDS(type) == NULL_TREE) // not empty, i.e not a forward declaration.
    {
        return;
    }

    const char *name = ORIG_TYPE_NAME(type);
    if (NULL == name) {
        // anonymous, ignore.
        return;
    }

    // TODO if any of the fields in target_struct references other structs, print them as well.
    if (strcmp(name, target_struct)) {
        // bye
        return;
    }

    if (was_dumped(name)) {
        return;
    }
    // add it immediately, so if we find any back references into current struct, we don't
    // dump it again.
    add_dumped(name);

    fprintf(output_file, "%s = [\n", name);

    for (tree field = TYPE_FIELDS(type); field; field = TREE_CHAIN(field)) {
        gcc_assert(TREE_CODE(field) == FIELD_DECL);

        debug_tree_helper(field, "field");

        // field name
        const char *field_name = IDENTIFIER_POINTER(DECL_NAME(field));
        gcc_assert(NULL != field_name); // shouldn't be NULL, no annonymous decls in a struct.

        // field type size
        tree field_type = TREE_TYPE(field);
        size_t field_size;
        size_t elem_size;
        size_t num_elem;

        const char *field_class = "Scalar";

        // TODO handle arrays recursively?
        if (TREE_CODE(field_type) == ARRAY_TYPE) {
            field_class = "Array";

            // that's the total size
            field_size = TREE_INT_CST_LOW(TYPE_SIZE(field_type));
            num_elem = TREE_INT_CST_LOW(TYPE_SIZE_UNIT(field_type));

            field_type = TREE_TYPE(field_type);
            gcc_assert(is_allowed_inner_type(field_type));

            elem_size = TREE_INT_CST_LOW(TYPE_SIZE(field_type));
        } else {
            field_size = TREE_INT_CST_LOW(TYPE_SIZE(field_type));
        }

        // field type name
        // TODO handle pointers recursively?
        if (POINTER_TYPE_P(field_type)) {
            field_class = "Pointer";

            field_type = TREE_TYPE(field_type);
            gcc_assert(is_allowed_inner_type(field_type));
        }

        tree type_name = TYPE_IDENTIFIER(field_type);
        const char *field_type_name = IDENTIFIER_POINTER(type_name);

        // field offset
        tree t_offset = DECL_FIELD_OFFSET(field);
        gcc_assert(TREE_CODE(t_offset) == INTEGER_CST && TREE_CONSTANT(t_offset));
        int offset = TREE_INT_CST_LOW(t_offset);
        // add bit offset. there's an explanation about why it's required, see macro declaration in tree.h
        tree t_bit_offset = DECL_FIELD_BIT_OFFSET(field);
        gcc_assert(TREE_CODE(t_bit_offset) == INTEGER_CST && TREE_CONSTANT(t_bit_offset));
        offset += TREE_INT_CST_LOW(t_bit_offset);

        fprintf(output_file, "\t%s('%s', %d, %d", field_class, field_name, offset, field_size);
        if (0 == strcmp(field_class, "Array")) {
            fprintf(output_file, ", %d, '%s'", num_elem, field_type_name);
        } else if (0 == strcmp(field_class, "Pointer") || 0 == strcmp(field_class, "Scalar")) {
            fprintf(output_file, ", '%s'", field_type_name);
        } else {
            gcc_unreachable();
        }
        fprintf(output_file, "),\n");
    }

    fprintf(output_file, "]\n");

    fflush(output_file);
}

int plugin_init(struct plugin_name_args *plugin_info, struct plugin_gcc_version *version)
{
    const char *output = NULL;

    for (size_t i = 0; i < plugin_info->argc; ++i) {
        if (0 == strcmp(plugin_info->argv[i].key, "output")) {
            output = plugin_info->argv[i].value;
        }
        if (0 == strcmp(plugin_info->argv[i].key, "struct")) {
            target_struct = xstrdup(plugin_info->argv[i].value);
        }
    }

    if (NULL == output) {
        fprintf(stderr, "structlayout plugin: missing parameter: -fplugin-arg-struct_layout-output=<output>\n");
        exit(EXIT_FAILURE);
    }

    if (NULL == target_struct) {
        fprintf(stderr, "structlayout plugin: missing parameter: -fplugin-arg-struct_layout-struct=<struct>\n");
        exit(EXIT_FAILURE);
    }

    output_file = fopen(output, "w");
    if (NULL == output_file) {
        perror(output);
        exit(EXIT_FAILURE);
    }

    register_callback(plugin_info->base_name, PLUGIN_FINISH_TYPE, plugin_finish_type, NULL);

    return 0;
}
