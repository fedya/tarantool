#include "memory.h"
#include "fiber.h"
#include "vy_write_iterator.h"
#include "vy_iterators_helper.h"

enum { MAX_DEFERRED_COUNT = 32 };

/** Test deferred delete handler. */
struct test_handler {
	struct vy_deferred_delete_handler base;
	/** Format to use for making DELETEs. */
	struct tuple_format *format;
	/** Deferred DELETEs generated by the write iterator. */
	struct tuple *stmt[MAX_DEFERRED_COUNT];
	/** Number of elements in @stmt array. */
	int count;
};

/**
 * Callback passed to the write iterator for generating deferred
 * DELETE statements.
 */
static int
test_handler_process(struct vy_deferred_delete_handler *base,
		     struct tuple *old_stmt, struct tuple *new_stmt)
{
	struct test_handler *handler = (struct test_handler *)base;

	fail_if(vy_stmt_type(old_stmt) == IPROTO_DELETE);
	fail_if(vy_stmt_type(new_stmt) != IPROTO_DELETE &&
		vy_stmt_type(new_stmt) != IPROTO_REPLACE);

	struct tuple *delete = vy_stmt_new_surrogate_delete(handler->format,
							    old_stmt);
	fail_if(delete == NULL);
	vy_stmt_set_lsn(delete, vy_stmt_lsn(new_stmt));

	fail_if(handler->count >= MAX_DEFERRED_COUNT);
	handler->stmt[handler->count++] = delete;
	return 0;
}

static void
test_handler_destroy(struct vy_deferred_delete_handler *base)
{
	struct test_handler *handler = (struct test_handler *)base;
	for (int i = 0; i < handler->count; i++)
		tuple_unref(handler->stmt[i]);
}

static const struct vy_deferred_delete_handler_iface test_handler_iface = {
	.process = test_handler_process,
	.destroy = test_handler_destroy,
};

static void
test_handler_create(struct test_handler *handler, struct tuple_format *format)
{
	memset(handler, 0, sizeof(*handler));
	handler->base.iface = &test_handler_iface;
	handler->format = format;
	tuple_format_ref(format);
}

/**
 * Create a mem with the specified content, iterate over it with
 * write_iterator and compare actual result statements with the
 * expected ones.
 *
 * @param content Mem content statements.
 * @param content_count Size of the @content.
 * @param expected Expected results of the iteration.
 * @param expected_count Size of the @expected.
 * @param deferred Expected deferred DELETEs returned by the iteration.
 * @param deferred_count Size of @deferred.
 * @param vlsns Read view lsns for the write iterator.
 * @param vlsns_count Size of the @vlsns.
 * @param is_primary True, if the new mem belongs to the primary
 *        index.
 * @param is_last_level True, if the new mem is the last level.
 */
void
compare_write_iterator_results(const struct vy_stmt_template *content,
			       int content_count,
			       const struct vy_stmt_template *expected,
			       int expected_count,
			       const struct vy_stmt_template *deferred,
			       int deferred_count,
			       const int *vlsns, int vlsns_count,
			       bool is_primary, bool is_last_level)
{
	uint32_t fields[] = { 0 };
	uint32_t types[] = { FIELD_TYPE_UNSIGNED };
	struct key_def *key_def = box_key_def_new(fields, types, 1);
	fail_if(key_def == NULL);
	struct vy_mem *mem = create_test_mem(key_def);
	for (int i = 0; i < content_count; ++i)
		vy_mem_insert_template(mem, &content[i]);
	struct rlist rv_list;
	struct vy_read_view *rv_array = malloc(sizeof(*rv_array) * vlsns_count);
	fail_if(rv_array == NULL);
	init_read_views_list(&rv_list, rv_array, vlsns, vlsns_count);

	struct test_handler handler;
	test_handler_create(&handler, mem->format);

	struct vy_stmt_stream *wi;
	wi = vy_write_iterator_new(key_def, is_primary, is_last_level, &rv_list,
				   is_primary ? &handler.base : NULL);
	fail_if(wi == NULL);
	fail_if(vy_write_iterator_new_mem(wi, mem) != 0);

	struct vy_entry ret;
	fail_if(wi->iface->start(wi) != 0);
	int i = 0;
	do {
		fail_if(wi->iface->next(wi, &ret) != 0);
		if (ret.stmt == NULL)
			break;
		fail_if(i >= expected_count);
		ok(vy_stmt_are_same(ret, &expected[i], mem->format, key_def),
		   "stmt %d is correct", i);
		++i;
	} while (ret.stmt != NULL);
	ok(i == expected_count, "correct results count");

	for (i = 0; i < handler.count; i++) {
		fail_if(i >= deferred_count);
		struct vy_entry entry;
		entry.stmt = handler.stmt[i];
		entry.hint = vy_stmt_hint(entry.stmt, key_def);
		ok(vy_stmt_are_same(entry, &deferred[i],
				    handler.format, key_def),
		   "deferred stmt %d is correct", i);
	}
	if (deferred != NULL) {
		ok(handler.count == deferred_count,
		   "correct deferred stmt count");
	}

	/* Clean up */
	wi->iface->stop(wi);
	wi->iface->close(wi);
	vy_mem_delete(mem);
	box_key_def_delete(key_def);
	free(rv_array);
}

void
test_basic(void)
{
	header();
	plan(58);
{
/*
 * STATEMENT: REPL REPL REPL  DEL  REPL  REPL  REPL  REPL  REPL  REPL
 * LSN:        5     6   7     8    9     10    11    12    13    14
 * READ VIEW:            *          *                 *
 *            \____________/\________/\_________________/\___________/
 *                 merge       merge          merge           merge
 */
	const struct vy_stmt_template content[] = {
		STMT_TEMPLATE(5, REPLACE, 1, 1),
		STMT_TEMPLATE(6, REPLACE, 1, 2),
		STMT_TEMPLATE(7, REPLACE, 1, 3),
		STMT_TEMPLATE(8, REPLACE, 1, 4),
		STMT_TEMPLATE(9, REPLACE, 1, 5),
		STMT_TEMPLATE(10, REPLACE, 1, 6),
		STMT_TEMPLATE(11, REPLACE, 1, 7),
		STMT_TEMPLATE(12, REPLACE, 1, 8),
		STMT_TEMPLATE(13, REPLACE, 1, 9),
		STMT_TEMPLATE(14, REPLACE, 1, 10),
	};
	const struct vy_stmt_template expected[] = {
		content[9], content[7], content[4], content[2]
	};
	const int vlsns[] = {7, 9, 12};
	int content_count = sizeof(content) / sizeof(content[0]);
	int expected_count = sizeof(expected) / sizeof(expected[0]);
	int vlsns_count = sizeof(vlsns) / sizeof(vlsns[0]);
	compare_write_iterator_results(content, content_count,
				       expected, expected_count, NULL, 0,
				       vlsns, vlsns_count, true, true);
}
{
/*
 * STATEMENT: UPS  UPS  UPS  UPS  UPS  UPS  UPS  UPS  UPS  UPS
 * LSN:        5    6    7    8    9   10   11   12   13   14
 * READ VIEW:       *                  *              *
 *           \________/\_________________/\_____________/\_____/
 *             squash         squash           squash     squash
 */
	const struct vy_stmt_template content[] = {
		STMT_TEMPLATE(5, UPSERT, 1, 1),
		STMT_TEMPLATE(6, UPSERT, 1, 2),
		STMT_TEMPLATE(7, UPSERT, 1, 3),
		STMT_TEMPLATE(8, UPSERT, 1, 4),
		STMT_TEMPLATE(9, UPSERT, 1, 5),
		STMT_TEMPLATE(10, UPSERT, 1, 6),
		STMT_TEMPLATE(11, UPSERT, 1, 7),
		STMT_TEMPLATE(12, UPSERT, 1, 8),
		STMT_TEMPLATE(13, UPSERT, 1, 9),
		STMT_TEMPLATE(14, UPSERT, 1, 10),
	};
	const struct vy_stmt_template expected[] = {
		content[9],
		STMT_TEMPLATE(13, UPSERT, 1, 7),
		STMT_TEMPLATE(10, UPSERT, 1, 3),
		STMT_TEMPLATE(6, UPSERT, 1, 1),
	};
	const int vlsns[] = {6, 10, 13};
	int content_count = sizeof(content) / sizeof(content[0]);
	int expected_count = sizeof(expected) / sizeof(expected[0]);
	int vlsns_count = sizeof(vlsns) / sizeof(vlsns[0]);
	compare_write_iterator_results(content, content_count,
				       expected, expected_count, NULL, 0,
				       vlsns, vlsns_count, true, false);
}
{
/*
 * STATEMENT: REPL     DEL UPS     REPL
 * LSN:        5       6    7        8
 * READ VIEW:               *
 *            \_______________/\_______/
 *             \_____\_/_____/   merge
 *    skip last level  merge
 *       delete
 */
	const struct vy_stmt_template content[] = {
		STMT_TEMPLATE(5, REPLACE, 1, 1),
		STMT_TEMPLATE(6, DELETE, 1),
		STMT_TEMPLATE(7, UPSERT, 1, 2),
		STMT_TEMPLATE(8, REPLACE, 1, 3),
	};
	const struct vy_stmt_template expected[] = {
		content[3],
		STMT_TEMPLATE(7, REPLACE, 1, 2)
	};
	const int vlsns[] = {7};
	int content_count = sizeof(content) / sizeof(content[0]);
	int expected_count = sizeof(expected) / sizeof(expected[0]);
	int vlsns_count = sizeof(vlsns) / sizeof(vlsns[0]);
	compare_write_iterator_results(content, content_count,
				       expected, expected_count, NULL, 0,
				       vlsns, vlsns_count, true, true);
}
{
/*
 * STATEMENT: REPL     REPL
 * LSN:        7        8
 * READ VIEW:  *        *
 *              No merge.
 */
	const struct vy_stmt_template content[] = {
		STMT_TEMPLATE(7, REPLACE, 1, 1),
		STMT_TEMPLATE(8, REPLACE, 1, 2),
	};
	const struct vy_stmt_template expected[] = { content[1], content[0] };
	const int vlsns[] = {7, 8};
	int content_count = sizeof(content) / sizeof(content[0]);
	int expected_count = sizeof(expected) / sizeof(expected[0]);
	int vlsns_count = sizeof(vlsns) / sizeof(vlsns[0]);
	compare_write_iterator_results(content, content_count,
				       expected, expected_count, NULL, 0,
				       vlsns, vlsns_count, true, true);
}
{
/*
 * LINKED WITH: gh-1824, about pruning last DELETE.
 * STATEMENT: DEL      REPL
 * LSN:        7        8
 * READ VIEW:  *        *
 *
 * is_last_level = true.
 * No merge, skip DELETE from last level, although there the read
 * view on the DELETE exists.
 */
	const struct vy_stmt_template content[] = {
		STMT_TEMPLATE(7, DELETE, 1),
		STMT_TEMPLATE(8, REPLACE, 1, 1),
	};
	const struct vy_stmt_template expected[] = { content[1] };
	const int vlsns[] = {7, 8};
	int content_count = sizeof(content) / sizeof(content[0]);
	int expected_count = sizeof(expected) / sizeof(expected[0]);
	int vlsns_count = sizeof(vlsns) / sizeof(vlsns[0]);
	compare_write_iterator_results(content, content_count,
				       expected, expected_count, NULL, 0,
				       vlsns, vlsns_count, true, true);
}
{
/*
 * LINKED WITH: gh-1824, about pruning last DELETE.
 * STATEMENT: DEL      REPL
 * LSN:        7        8
 * READ VIEW:  *        *
 *
 * is_last_level = false;
 * No merge, don't skip DELETE from last level.
 */
	const struct vy_stmt_template content[] = {
		STMT_TEMPLATE(7, DELETE, 1),
		STMT_TEMPLATE(8, REPLACE, 1, 1),
	};
	const struct vy_stmt_template expected[] = { content[1], content[0] };
	const int vlsns[] = {7, 8};
	int content_count = sizeof(content) / sizeof(content[0]);
	int expected_count = sizeof(expected) / sizeof(expected[0]);
	int vlsns_count = sizeof(vlsns) / sizeof(vlsns[0]);
	compare_write_iterator_results(content, content_count,
				       expected, expected_count, NULL, 0,
				       vlsns, vlsns_count, true, false);
}
{
/*
 * STATEMENT: UPS  UPS  UPS  REPL
 * LSN:        6    7    8    9
 * READ VIEW:       *
 *            \______/\________/
 *             merge    merge
 * UPSERT before REPLACE must be squashed with only older
 * statements.
 */
	const struct vy_stmt_template content[] = {
		STMT_TEMPLATE(6, UPSERT, 1, 1),
		STMT_TEMPLATE(7, UPSERT, 1, 2),
		STMT_TEMPLATE(8, UPSERT, 1, 3),
		STMT_TEMPLATE(9, REPLACE, 1, 4)
	};
	const struct vy_stmt_template expected[] = {
		content[3], STMT_TEMPLATE(7, UPSERT, 1, 1)
	};
	const int vlsns[] = {7};
	int content_count = sizeof(content) / sizeof(content[0]);
	int expected_count = sizeof(expected) / sizeof(expected[0]);
	int vlsns_count = sizeof(vlsns) / sizeof(vlsns[0]);
	compare_write_iterator_results(content, content_count,
				       expected, expected_count, NULL, 0,
				       vlsns, vlsns_count, true, false);
}
{
/*
 * STATEMENT: REPL  REPL           REPL  REPL
 * LSN:        6     7             20     21
 * READ VIEW:        *    *(10)    *      *      *(22)  *(23)
 *            \________/\______/\_____/\______/\____________/
 *              merge   nullify   merge  merge     nullify
 *
 * Do not remember the read views with the same versions of the
 * key.
 */
	const struct vy_stmt_template content[] = {
		STMT_TEMPLATE(6, REPLACE, 1, 1),
		STMT_TEMPLATE(7, REPLACE, 1, 2),
		STMT_TEMPLATE(20, REPLACE, 1, 3),
		STMT_TEMPLATE(21, REPLACE, 1, 4)
	};
	const struct vy_stmt_template expected[] = {
		content[3], content[2], content[1]
	};
	const int vlsns[] = {7, 10, 20, 21, 22, 23};
	int content_count = sizeof(content) / sizeof(content[0]);
	int expected_count = sizeof(expected) / sizeof(expected[0]);
	int vlsns_count = sizeof(vlsns) / sizeof(vlsns[0]);
	compare_write_iterator_results(content, content_count,
				       expected, expected_count, NULL, 0,
				       vlsns, vlsns_count, true, true);
}
{
/*
 * STATEMENT: REPL DEL REPL DEL REPL DEL
 * LSN:        4    5   6    7    8    9
 * READ VIEW:       *        *         *
 *            \_______/\_______________/
 *              merge         skip
 *
 * is_last_level = false
 *
 * Check that tautological DELETEs referenced by newer
 * read views are skipped.
 */
	const struct vy_stmt_template content[] = {
		STMT_TEMPLATE(4, REPLACE, 1, 1),
		STMT_TEMPLATE(5, DELETE, 1),
		STMT_TEMPLATE(6, REPLACE, 1, 2),
		STMT_TEMPLATE(7, DELETE, 1),
		STMT_TEMPLATE(8, REPLACE, 1, 3),
		STMT_TEMPLATE(9, DELETE, 1),
	};
	const struct vy_stmt_template expected[] = { content[1] };
	const int vlsns[] = {5, 7, 9};
	int content_count = sizeof(content) / sizeof(content[0]);
	int expected_count = sizeof(expected) / sizeof(expected[0]);
	int vlsns_count = sizeof(vlsns) / sizeof(vlsns[0]);
	compare_write_iterator_results(content, content_count,
				       expected, expected_count, NULL, 0,
				       vlsns, vlsns_count, true, false);
}
{
/*
 * STATEMENT: INS DEL REPL DEL REPL REPL INS REPL
 * LSN:        2   3   4    5   6    7    8   9
 * READ VIEW:      *        *        *    *   *
 *            \______/\_______/\_______/
 *              merge   merge    merge
 *
 *                DEL      DEL      REPL INS REPL
 *                \__________/      \__/
 *                  discard     convert to INS
 *
 * is_last_level = false
 *
 * If the oldest statement for a given key is an INSERT, all
 * leading DELETE statements should be discarded and the first
 * non-DELETE statement should be turned into an INSERT.
 */
	const struct vy_stmt_template content[] = {
		STMT_TEMPLATE(2, INSERT, 1, 1),
		STMT_TEMPLATE(3, DELETE, 1),
		STMT_TEMPLATE(4, REPLACE, 1, 2),
		STMT_TEMPLATE(5, DELETE, 1),
		STMT_TEMPLATE(6, REPLACE, 1, 3),
		STMT_TEMPLATE(7, REPLACE, 1, 4),
		STMT_TEMPLATE(8, INSERT, 1, 5),
		STMT_TEMPLATE(9, REPLACE, 1, 6),
	};
	const struct vy_stmt_template expected[] = {
		content[7],
		content[6],
		STMT_TEMPLATE(7, INSERT, 1, 4),
	};
	const int vlsns[] = {3, 5, 7, 8, 9};
	int content_count = sizeof(content) / sizeof(content[0]);
	int expected_count = sizeof(expected) / sizeof(expected[0]);
	int vlsns_count = sizeof(vlsns) / sizeof(vlsns[0]);
	compare_write_iterator_results(content, content_count,
				       expected, expected_count, NULL, 0,
				       vlsns, vlsns_count, true, false);
}
{
/*
 * STATEMENT: DEL INS DEL INS REPL DEL INS
 * LSN:        3   4   5   6   7    8   9
 * READ VIEW:              *   *
 *            \______________/     \_____/
 *                  merge           merge
 *
 *                        INS REPL     INS
 *                        \__/
 *                   convert to REPL
 *
 * is_last_level = false
 *
 * If the oldest statement for a given key is NOT an INSERT
 * and the first key in the resulting history turns out to be
 * an INSERT, it should be converted to a REPLACE.
 */
	const struct vy_stmt_template content[] = {
		STMT_TEMPLATE(3, DELETE, 1),
		STMT_TEMPLATE(4, INSERT, 1, 1),
		STMT_TEMPLATE(5, DELETE, 1),
		STMT_TEMPLATE(6, INSERT, 1, 2),
		STMT_TEMPLATE(7, REPLACE, 1, 3),
		STMT_TEMPLATE(8, DELETE, 1),
		STMT_TEMPLATE(9, INSERT, 1, 4),
	};
	const struct vy_stmt_template expected[] = {
		content[6],
		content[4],
		STMT_TEMPLATE(6, REPLACE, 1, 2),
	};
	const int vlsns[] = {6, 7};
	int content_count = sizeof(content) / sizeof(content[0]);
	int expected_count = sizeof(expected) / sizeof(expected[0]);
	int vlsns_count = sizeof(vlsns) / sizeof(vlsns[0]);
	compare_write_iterator_results(content, content_count,
				       expected, expected_count, NULL, 0,
				       vlsns, vlsns_count, true, false);
}
{
/*
 * STATEMENT:    REPL DEL REPL REPL DEL DEL DEL REPL DEL INS DEL INS REPL
 * LSN:            4   5    6    7   8   9  10   11  12  13  14  15   16
 * DEFERRED DEL:   +   +    +        +   +        +           +        +
 * READ VIEW:          *         *                *
 *
 * is_last_level = true
 *
 * Test generation of deferred DELETEs for various combinations
 * of input statements.
 */
	const struct vy_stmt_template content[] = {
		STMT_TEMPLATE_DEFERRED_DELETE(4, REPLACE, 1, 2),
		STMT_TEMPLATE_DEFERRED_DELETE(5, DELETE, 1),
		STMT_TEMPLATE_DEFERRED_DELETE(6, REPLACE, 1, 3),
		STMT_TEMPLATE(7, REPLACE, 1, 4),
		STMT_TEMPLATE_DEFERRED_DELETE(8, DELETE, 1),
		STMT_TEMPLATE_DEFERRED_DELETE(9, DELETE, 1),
		STMT_TEMPLATE(10, DELETE, 1),
		STMT_TEMPLATE_DEFERRED_DELETE(11, REPLACE, 1, 5),
		STMT_TEMPLATE(12, DELETE, 1),
		STMT_TEMPLATE(13, INSERT, 1, 6),
		STMT_TEMPLATE_DEFERRED_DELETE(14, DELETE, 1),
		STMT_TEMPLATE(15, INSERT, 1, 7),
		STMT_TEMPLATE_DEFERRED_DELETE(16, REPLACE, 1, 8),
	};
	const struct vy_stmt_template expected[] = {
		STMT_TEMPLATE(16, REPLACE, 1, 8),
		STMT_TEMPLATE(11, REPLACE, 1, 5),
		STMT_TEMPLATE(7, REPLACE, 1, 4),
	};
	const struct vy_stmt_template deferred[] = {
		STMT_TEMPLATE(16, DELETE, 1, 7),
		STMT_TEMPLATE(14, DELETE, 1, 6),
		STMT_TEMPLATE(8, DELETE, 1, 4),
		STMT_TEMPLATE(5, DELETE, 1, 2),
	};
	const int vlsns[] = {5, 7, 11};
	int content_count = sizeof(content) / sizeof(content[0]);
	int expected_count = sizeof(expected) / sizeof(expected[0]);
	int deferred_count = sizeof(deferred) / sizeof(deferred[0]);
	int vlsns_count = sizeof(vlsns) / sizeof(vlsns[0]);
	compare_write_iterator_results(content, content_count,
				       expected, expected_count,
				       deferred, deferred_count,
				       vlsns, vlsns_count, true, true);
}
{
/*
 * STATEMENT:    REPL REPL DEL
 * LSN:            7    8   9
 * DEFERRED DEL:   +
 *
 * is_last_level = false
 *
 * Check that the oldest VY_STMT_DEFERRED_DELETE statement is
 * preserved in case it doesn't overwrite a terminal statement
 * and this is not a major compaction.
 */
	const struct vy_stmt_template content[] = {
		STMT_TEMPLATE_DEFERRED_DELETE(7, REPLACE, 1, 1),
		STMT_TEMPLATE(8, REPLACE, 1, 2),
		STMT_TEMPLATE(9, DELETE, 1, 3),
	};
	const struct vy_stmt_template expected[] = {
		STMT_TEMPLATE(9, DELETE, 1, 1),
		STMT_TEMPLATE_DEFERRED_DELETE(7, REPLACE, 1, 1),
	};
	const struct vy_stmt_template deferred[] = {};
	const int vlsns[] = {};
	int content_count = sizeof(content) / sizeof(content[0]);
	int expected_count = sizeof(expected) / sizeof(expected[0]);
	int deferred_count = sizeof(deferred) / sizeof(deferred[0]);
	int vlsns_count = sizeof(vlsns) / sizeof(vlsns[0]);
	compare_write_iterator_results(content, content_count,
				       expected, expected_count,
				       deferred, deferred_count,
				       vlsns, vlsns_count, true, false);
}
{
/*
 * STATEMENT:    REPL REPL DEL
 * LSN:            7    8   9
 * DEFERRED DEL:   +
 * READ VIEW:      *
 *
 * is_last_level = false
 *
 * Check that the oldest VY_STMT_DEFERRED_DELETE statement is
 * not returned twice if it is referenced by a read view.
 */
	const struct vy_stmt_template content[] = {
		STMT_TEMPLATE_DEFERRED_DELETE(7, REPLACE, 1, 1),
		STMT_TEMPLATE(8, REPLACE, 1, 2),
		STMT_TEMPLATE(9, DELETE, 1, 3),
	};
	const struct vy_stmt_template expected[] = {
		STMT_TEMPLATE(9, DELETE, 1, 1),
		STMT_TEMPLATE_DEFERRED_DELETE(7, REPLACE, 1, 1),
	};
	const struct vy_stmt_template deferred[] = {};
	const int vlsns[] = {7};
	int content_count = sizeof(content) / sizeof(content[0]);
	int expected_count = sizeof(expected) / sizeof(expected[0]);
	int deferred_count = sizeof(deferred) / sizeof(deferred[0]);
	int vlsns_count = sizeof(vlsns) / sizeof(vlsns[0]);
	compare_write_iterator_results(content, content_count,
				       expected, expected_count,
				       deferred, deferred_count,
				       vlsns, vlsns_count, true, false);
}
{
/*
 * STATEMENT:    REPL
 * LSN:            7
 * DEFERRED DEL:   +
 *
 * is_last_level = false
 *
 * Check that the oldest VY_STMT_DEFERRED_DELETE statement is
 * not returned twice if it is the only statement in the output.
 */
	const struct vy_stmt_template content[] = {
		STMT_TEMPLATE_DEFERRED_DELETE(7, REPLACE, 1, 1),
	};
	const struct vy_stmt_template expected[] = {
		STMT_TEMPLATE_DEFERRED_DELETE(7, REPLACE, 1, 1),
	};
	const struct vy_stmt_template deferred[] = {};
	const int vlsns[] = {};
	int content_count = sizeof(content) / sizeof(content[0]);
	int expected_count = sizeof(expected) / sizeof(expected[0]);
	int deferred_count = sizeof(deferred) / sizeof(deferred[0]);
	int vlsns_count = sizeof(vlsns) / sizeof(vlsns[0]);
	compare_write_iterator_results(content, content_count,
				       expected, expected_count,
				       deferred, deferred_count,
				       vlsns, vlsns_count, true, false);
}
	fiber_gc();
	footer();
	check_plan();
}

int
main(int argc, char *argv[])
{
	vy_iterator_C_test_init(0);

	test_basic();

	vy_iterator_C_test_finish();
	return 0;
}
