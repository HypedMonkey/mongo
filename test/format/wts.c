/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "format.h"

static int  bulk(WT_ITEM **, WT_ITEM **);
static int  wts_close(WT_CONNECTION *);
static int  wts_col_del(uint64_t, int *);
static int  wts_col_insert(uint64_t *);
static int  wts_col_put(uint64_t);
static int  wts_notfound_chk(const char *, int, int, uint64_t);
static int  wts_np(int, int, int *);
static int  wts_open(WT_CONNECTION **, WT_SESSION **session);
static int  wts_read(uint64_t);
static int  wts_row_del(uint64_t, int *);
static int  wts_row_put(uint64_t, int);
static void wts_stream_item(const char *, WT_ITEM *);
static int  wts_sync(void);

static int
handle_message(WT_EVENT_HANDLER *handler, const char *message)
{
	UNUSED(handler);

	if (g.logfp != NULL)
		fprintf(g.logfp, "%s\n", message);
	else
		printf("%s\n", message);
	return (0);
}

/*
 * __handle_progress_default --
 *	Default WT_EVENT_HANDLER->handle_progress implementation: ignore.
 */
static int
handle_progress(WT_EVENT_HANDLER *handler,
     const char *operation, uint64_t progress)
{
	UNUSED(handler);

	track(operation, progress);
	return (0);
}

static WT_EVENT_HANDLER event_handler = {
	NULL,
	handle_message,
	handle_progress
};

static int
wts_open(WT_CONNECTION **connp, WT_SESSION **sessionp)
{
	WT_CONNECTION *conn;
	WT_SESSION *session;
	int ret;
	const char *ext1, *ext2;
	char config[256];

	/* If the bzip2 compression module has been built, use it. */
	ext1 = "../../ext/compressors/bzip2_compress/.libs/bzip2_compress.so";
	if (access(ext1, R_OK) != 0) {
		ext1 = "";
		g.c_bzip = 0;
	}
	ext2 = "../../ext/collators/reverse/.libs/reverse_collator.so";

	/*
	 * Open configuration -- put command line configuration options at the
	 * end so they can override "standard" configuration.
	 */
	snprintf(config, sizeof(config),
	    "create,error_prefix=\"%s\",cache_size=%" PRIu32 "MB,"
	    "extensions=[\"%s\",\"%s\"],%s,%s",
	    g.progname, g.c_cache, ext1, ext2,
	    g.c_multithread ? "multithread" : "",
	    g.config_open == NULL ? "" : g.config_open);

	if ((ret = wiredtiger_open(NULL, &event_handler, config, &conn)) != 0) {
		fprintf(stderr, "%s: wiredtiger_open: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0) {
		fprintf(stderr, "%s: conn.session: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		(void)conn->close(conn, NULL);
		return (1);
	}

	*sessionp = session;
	*connp = conn;
	return (0);
}

static int
wts_close(WT_CONNECTION *conn)
{
	int ret;
	if ((ret = conn->close(conn, NULL)) != 0) {
		fprintf(stderr, "%s: conn.close: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}

	return (0);
}

int
wts_startup(int open_cursors)
{
	time_t now;
	WT_CONNECTION *conn;
	WT_CURSOR *cursor, *cursor_insert;
	WT_SESSION *session;
	uint32_t intlmax, intlovfl, leafmax, leafovfl;
	int ret;
	char config[512], *end, *p;

	if (wts_open(&conn, &session))
		return (1);

	intlmax = 1U << g.c_intl_node_max;
	intlovfl = MMRAND(intlmax / 50, intlmax / 40);
	if (intlovfl < 40)
		intlovfl = 40;
	leafmax = 1U << g.c_leaf_node_max;
	leafovfl = MMRAND(leafmax / 50, leafmax / 40);
	if (leafovfl < 40)
		leafovfl = 40;

	p = config;
	end = config + sizeof(config);
	p += snprintf(p, (size_t)(end - p),
	    "key_format=%s,"
	    "internal_node_max=%d,internal_overflow_size=%d,"
	    "leaf_node_max=%d,leaf_overflow_size=%d",
	    (g.c_file_type == ROW) ? "u" : "r",
	    intlmax, intlovfl, leafmax, leafovfl);

	if (g.c_bzip)
		p += snprintf(p, (size_t)(end - p),
		    ",block_compressor=\"bzip2_compress\"");

	switch (g.c_file_type) {
	case FIX:
		p += snprintf(p, (size_t)(end - p),
		    ",value_format=%dt", g.c_bitcnt);
		break;
	case ROW:
		if (g.c_huffman_key)
			p += snprintf(p, (size_t)(end - p),
			    ",huffman_key=english");
                if (g.c_reverse)
			p += snprintf(p, (size_t)(end - p),
			    ",collator=reverse");
		/* FALLTHROUGH */
	case VAR:
		if (g.c_huffman_value)
			p += snprintf(p, (size_t)(end - p),
			    ",huffman_value=english");
		break;
	}

	if ((ret = session->create(session, WT_TABLENAME, config)) != 0) {
		fprintf(stderr, "%s: create table: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}

	if (open_cursors) {
		/*
		 * We open 2 cursors, one configured for overwriting, one not
		 * configured for overwriting.  The reason is that for
		 * row-store and column-store files where we're testing with
		 * existing records, we don't track if a record was deleted or
		 * not, which means we need to use cursor->insert with
		 * overwriting configured.  But, in column-store files where
		 * we're testing with new, appended records, we don't want to
		 * have to specify the record number, which means we can't
		 * configure with overwriting.
		 */
		if ((ret = session->open_cursor(
		    session, WT_TABLENAME, NULL, NULL, &cursor_insert)) != 0) {
			fprintf(stderr, "%s: open_cursor: %s\n",
			    g.progname, wiredtiger_strerror(ret));
			return (1);
		}
		if ((ret = session->open_cursor(
		    session, WT_TABLENAME, NULL, "overwrite", &cursor)) != 0) {
			fprintf(stderr, "%s: open_cursor: %s\n",
			    g.progname, wiredtiger_strerror(ret));
			return (1);
		}
	} else
		cursor = cursor_insert = NULL;

	if (g.logging) {
		(void)time(&now);
		(void)session->msg_printf(session,
		    "===============\nWT start: %s===============",
		    ctime(&now));
	}

	g.wts_conn = conn;
	g.wts_cursor = cursor;
	g.wts_cursor_insert = cursor_insert;
	g.wts_session = session;

	return (0);
}

int
wts_teardown(void)
{
	WT_CONNECTION *conn;
	WT_CURSOR *cursor, *cursor_insert;
	WT_SESSION *session;
	time_t now;
	int ret;

	conn = g.wts_conn;
	cursor = g.wts_cursor;
	cursor_insert = g.wts_cursor_insert;
	session = g.wts_session;

	if (g.logging) {
		(void)time(&now);
		(void)session->msg_printf(session,
		    "===============\nWT stop: %s===============",
		    ctime(&now));
	}

	/*
	 * Close the open cursors -- they will block sync.
	 */
	if ((cursor_insert != NULL &&
	    (ret = cursor_insert->close(cursor_insert, NULL)) != 0) ||
	    (cursor != NULL && (ret = cursor->close(cursor, NULL)) != 0))
		die("cursor.close", ret);

	ret = wts_sync();
	return (wts_close(conn) ? 1 : ret);
}

int
wts_bulk_load(void)
{
	WT_CURSOR *cursor;
	WT_SESSION *session;
	WT_ITEM *key, *value;
	uint64_t insert_count;
	int ret;

	session = g.wts_session;

        /*
         * Avoid bulk load with a custom collator, because the order of
         * insertion will not match the collation order.
         */
	if ((ret = session->open_cursor(session, WT_TABLENAME, NULL,
            (g.c_file_type == ROW && g.c_reverse) ? NULL : "bulk",
            &cursor)) != 0) {
		fprintf(stderr, "%s: cursor open failed: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}

	insert_count = 0;
	while (bulk(&key, &value) == 0) {
                /* Report on progress every 100 inserts. */
                if (++insert_count % 100 == 0)
                        track("bulk load", insert_count);
	
		if (key != NULL)
			cursor->set_key(cursor, key);
		if (g.c_file_type == FIX)
			cursor->set_value(cursor, *(uint8_t *)value->data);
		else
			cursor->set_value(cursor, value);
		if ((ret = cursor->insert(cursor)) != 0) {
			fprintf(stderr, "%s: cursor insert failed: %s\n",
			    g.progname, wiredtiger_strerror(ret));
			ret = 1;
			goto err;
		}
	}

err:	(void)cursor->close(cursor, NULL);
	return (ret);
}

int
wts_dump(const char *tag, int dump_bdb)
{
	char cmd[128];

	track("dump files and compare", 0ULL);
	switch (g.c_file_type) {
	case FIX:
	case VAR:
		snprintf(cmd, sizeof(cmd),
		    "sh ./s_dumpcmp%s -c", dump_bdb ? " -b" : "");
		break;
	case ROW:
		snprintf(cmd, sizeof(cmd),
		    "sh ./s_dumpcmp%s", dump_bdb ? " -b" : "");
		break;
	default:
		return (1);
	}
	if (system(cmd) != 0) {
		fprintf(stderr,
		    "%s: %s dump comparison failed\n", g.progname, tag);
		return (1);
	}

	return (0);
}

int
wts_salvage(void)
{
	WT_CONNECTION *conn;
	WT_SESSION *session;
	int ret;

	track("salvage", 0ULL);

	/* Save a copy of the file before we salvage it. */
	(void)system("cp __wt __salvage_copy");

	if (wts_open(&conn, &session))
		return (1);

	if ((ret = session->salvage(session, WT_TABLENAME, NULL)) != 0) {
		fprintf(stderr, "%s: salvage: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}

	return (wts_close(conn));
}

static int
wts_sync(void)
{
	WT_SESSION *session;
	int ret;

	session = g.wts_session;

	track("sync", 0ULL);

	if ((ret = session->sync(
	    session, WT_TABLENAME, NULL)) != 0 && ret != EBUSY) {
		fprintf(stderr, "%s: sync: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}
	return (0);
}

int
wts_verify(const char *tag)
{
	WT_CONNECTION *conn;
	WT_SESSION *session;
	int ret;

	track("verify", 0ULL);

	if (wts_open(&conn, &session))
		return (1);

	if ((ret = session->verify(session, WT_TABLENAME, NULL)) != 0)
		fprintf(stderr, "%s: %s verify: %s\n",
		    g.progname, tag, wiredtiger_strerror(ret));

	return (wts_close(conn) ? 1 : ret);
}

/*
 * wts_stats --
 *	Dump the run's statistics.
 */
int
wts_stats(void)
{
	WT_CURSOR *cursor;
	WT_SESSION *session;
	FILE *fp;
	const char *pval, *desc;
	uint64_t v;
	int ret;

	session = g.wts_session;

	track("stat", 0ULL);

	if ((fp = fopen("__stats", "w")) == NULL)
		die("__stats", errno);

	/* Connection statistics. */
	if ((ret = session->open_cursor(session,
	    "statistics:", NULL, NULL, &cursor)) != 0) {
		fprintf(stderr, "%s: stat cursor open failed: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}
	while (
	    (ret = cursor->next(cursor)) == 0 &&
	    (ret = cursor->get_key(cursor, &desc)) == 0 &&
	    (ret = cursor->get_value(cursor, &pval, &v)) == 0)
		if (fprintf(fp, "%s=%s\n", desc, pval) < 0) {
			ret = errno;
			break;
		}

	if (ret != WT_NOTFOUND)
		die("cursor.next", ret);
	if ((ret = cursor->close(cursor, NULL)) != 0)
		die("cursor.close", ret);
	
	/* File statistics. */
	if ((ret = session->open_cursor(session,
	    "statistics:" WT_TABLENAME, NULL, NULL, &cursor)) != 0) {
		fprintf(stderr, "%s: stat cursor open failed: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}
	while (
	    (ret = cursor->next(cursor)) == 0 &&
	    (ret = cursor->get_key(cursor, &desc)) == 0 &&
	    (ret = cursor->get_value(cursor, &pval, &v)) == 0)
		if (fprintf(fp, "%s=%s\n", desc, pval) < 0) {
			ret = errno;
			break;
		}

	if (ret != WT_NOTFOUND)
		die("cursor.next", ret);
	if ((ret = cursor->close(cursor, NULL)) != 0)
		die("cursor.close", ret);

	(void)fclose(fp);

	return (0);
}

/*
 * bulk --
 *	WiredTiger bulk load routine. 
 */
static int
bulk(WT_ITEM **keyp, WT_ITEM **valuep)
{
	static WT_ITEM key, value;
	WT_SESSION *session;

	session = g.wts_session;
	++g.key_cnt;

	if (g.key_cnt > g.c_rows) {
		g.key_cnt = g.c_rows;
		return (1);
	}

	key_gen(&key.data, &key.size, (uint64_t)g.key_cnt, 0);
	value_gen(&value.data, &value.size, (uint64_t)g.key_cnt);

	switch (g.c_file_type) {
	case FIX:
		*keyp = NULL;
		*valuep = &value;
		if (g.logging)
			(void)session->msg_printf(session,
			    "%-10s %" PRIu32 " {0x%02" PRIx8 "}",
			    "bulk V",
			    g.key_cnt, ((uint8_t *)value.data)[0]);
		break;
	case VAR:
		*keyp = NULL;
		*valuep = &value;
		if (g.logging)
			(void)session->msg_printf(session,
			    "%-10s %" PRIu32 " {%.*s}", "bulk V",
			    g.key_cnt, (int)value.size, (char *)value.data);
		break;
	case ROW:
		*keyp = &key;
		if (g.logging)
			(void)session->msg_printf(session,
			    "%-10s %" PRIu32 " {%.*s}", "bulk K",
		    g.key_cnt, (int)key.size, (char *)key.data);
		*valuep = &value;
		if (g.logging)
			(void)session->msg_printf(session,
			    "%-10s %" PRIu32 " {%.*s}", "bulk V",
			    g.key_cnt, (int)value.size, (char *)value.data);
		break;
	}

	/* Insert the item into BDB. */
	bdb_insert(key.data, key.size, value.data, value.size);

	return (0);
}

/*
 * wts_ops --
 *	Perform a number of operations.
 */
int
wts_ops(void)
{
	uint64_t cnt, keyno;
	uint32_t op;
	u_int np;
	int insert, notfound;

	for (cnt = 0; cnt < g.c_ops; ++cnt) {
		if (cnt % 10 == 0)
			track("read/write ops", cnt);

		insert = notfound = 0;
		keyno = MMRAND(1, g.c_rows);

		/*
		 * Perform some number of operations: the percentage of deletes,
		 * inserts and writes are specified, reads are the rest.  The
		 * percentages don't have to add up to 100, a high percentage
		 * of deletes will mean fewer inserts and writes.  A read
		 * operation always follows a modification to confirm it worked.
		 */
		op = (uint32_t)(wts_rand() % 100);
		if (op < g.c_delete_pct) {
			switch (g.c_file_type) {
			case ROW:
				/*
				 * If deleting a non-existent record, the cursor
				 * won't be positioned, and so can't do a next.
				 */
				if (wts_row_del(keyno, &notfound))
					return (1);
				break;
			case FIX:
			case VAR:
				if (wts_col_del(keyno, &notfound))
					return (1);
				break;
			}
		} else if (op < g.c_delete_pct + g.c_insert_pct) {
			switch (g.c_file_type) {
			case ROW:
				if (wts_row_put(keyno, 1))
					return (1);
				break;
			case FIX:
			case VAR:
				if (wts_col_insert(&keyno))
					return (1);
				insert = 1;
				break;
			}
		} else if (
		    op < g.c_delete_pct + g.c_insert_pct + g.c_write_pct) {
			switch (g.c_file_type) {
			case ROW:
				if (wts_row_put(keyno, 0))
					return (1);
				break;
			case FIX:
			case VAR:
				if (wts_col_put(keyno))
					return (1);
				break;
			}
		} else {
			if (wts_read(keyno))
				return (1);
			continue;
		}

		/*
		 * If we did any operation, we've set the cursor, do a small
		 * number of next/prev cursor operations.
		 */
		for (np = 0; np < MMRAND(1, 4); ++np) {
			if (notfound)
				break;
			if (wts_np(MMRAND(0, 1), insert, &notfound))
				return (1);
		}

		/* Then read the value we modified to confirm it worked. */
		if (wts_read(keyno))
			return (1);
	}
	return (0);
}

/*
 * wts_read_scan --
 *	Read and verify all elements in a file.
 */
int
wts_read_scan(void)
{
	uint64_t cnt, last_cnt;

	/* Check a random subset of the records using the key. */
	for (last_cnt = cnt = 0; cnt < g.key_cnt;) {
		cnt += wts_rand() % 17 + 1;
		if (cnt > g.c_rows)
			cnt = g.c_rows;
		if (cnt - last_cnt > 1000) {
			track("read row scan", cnt);
			last_cnt = cnt;
		}

		if (wts_read(cnt))
			return (1);
	}
	return (0);
}

#define	NTF_CHK(a) do {							\
	switch (a) {							\
	case 0:								\
		break;							\
	case 1:								\
		return (1);						\
	case 2:								\
		return (0);						\
	}								\
} while (0)

/*
 * wts_read --
 *	Read and verify a single element in a row- or column-store file.
 */
static int
wts_read(uint64_t keyno)
{
	static WT_ITEM key, value, bdb_value;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	int notfound, ret;
	uint8_t bitfield;

	cursor = g.wts_cursor;
	session = g.wts_session;

	/* Log the operation */
	if (g.logging)
		(void)session->msg_printf(
		    session, "%-10s%" PRIu64, "read", keyno);

	/* Retrieve the BDB value. */
	if (bdb_read(keyno, &bdb_value.data, &bdb_value.size, &notfound))
		return (1);

	/* Retrieve the key/value pair by key. */
	switch (g.c_file_type) {
	case FIX:
	case VAR:
		cursor->set_key(cursor, keyno);
		break;
	case ROW:
		key_gen(&key.data, &key.size, keyno, 0);
		cursor->set_key(cursor, &key);
		break;
	}

	if ((ret = cursor->search(cursor)) == 0) {
		if (g.c_file_type == FIX) {
			ret = cursor->get_value(cursor, &bitfield);
			value.data = &bitfield;
			value.size = 1;
		} else
			ret = cursor->get_value(cursor, &value);
	}
	if (ret != 0 && ret != WT_NOTFOUND) {
		fprintf(stderr, "%s: wts_read: read row %" PRIu64 ": %s\n",
		    g.progname, keyno, wiredtiger_strerror(ret));
		return (1);
	}

	/*
	 * Check for not-found status.
	 *
	 * In fixed length stores, zero values at the end of the key space
	 * are treated as not found.  Treat this the same as a zero value
	 * in the key space, to match BDB's behavior.
	 */
	if (g.c_file_type == FIX && ret == WT_NOTFOUND) {
		bitfield = 0;
		ret = 0;
	}

	NTF_CHK(wts_notfound_chk("wts_read", ret, notfound, keyno));

	/* Compare the two. */
	if (value.size != bdb_value.size ||
	    memcmp(value.data, bdb_value.data, value.size) != 0) {
		fprintf(stderr,
		    "wts_read: read row value mismatch %" PRIu64 ":\n", keyno);
		wts_stream_item("bdb", &bdb_value);
		wts_stream_item(" wt", &value);
		return (1);
	}
	return (0);
}

/*
 * wts_np --
 *	Read and verify the next/prev element in a row- or column-store file.
 */
static int
wts_np(int next, int insert, int *notfoundp)
{
	static WT_ITEM key, value, bdb_key, bdb_value;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	uint64_t keyno;
	int notfound, ret;
	uint8_t bitfield;
	const char *which;
	char *p;

	cursor = insert ? g.wts_cursor_insert : g.wts_cursor;
	session = g.wts_session;
	which = next ? "next" : "prev";

	/* Retrieve the BDB value. */
	if (bdb_np(next, &bdb_key.data, &bdb_key.size,
	    &bdb_value.data, &bdb_value.size, &notfound))
		return (1);
	*notfoundp = notfound;

	keyno = 0;
	ret = next ? cursor->next(cursor) : cursor->prev(cursor);
	if (ret == 0)
		switch (g.c_file_type) {
		case FIX:
			if ((ret = cursor->get_key(cursor, &keyno)) == 0 &&
			    (ret = cursor->get_value(cursor, &bitfield)) == 0) {
				value.data = &bitfield;
				value.size = 1;
			}
			break;
		case ROW:
			if ((ret = cursor->get_key(cursor, &key)) == 0)
				ret = cursor->get_value(cursor, &value);
			break;
		case VAR:
			if ((ret = cursor->get_key(cursor, &keyno)) == 0)
				ret = cursor->get_value(cursor, &value);
			break;
		}
	if (ret != 0 && ret != WT_NOTFOUND) {
		fprintf(stderr,
		    "%s: wts_%s: %s\n",
		    g.progname, which, wiredtiger_strerror(ret));
		return (1);
	}

	NTF_CHK(wts_notfound_chk(
	    next ? "wts_np(next)" : "wts_np(prev)", ret, notfound, keyno));

	/* Compare the two. */
	if (g.c_file_type == ROW) {
		if (key.size != bdb_key.size ||
		    memcmp(key.data, bdb_key.data, key.size) != 0) {
			fprintf(stderr, "wts_np: %s key mismatch:\n", which);
			wts_stream_item("bdb-key", &bdb_key);
			wts_stream_item(" wt-key", &key);
			return (1);
		}
	} else {
		if (keyno != (uint64_t)atoll(bdb_key.data)) {
			if ((p = strchr((char *)bdb_key.data, '.')) != NULL)
				*p = '\0';
			fprintf(stderr,
			    "wts_np: %s key mismatch: %.*s != %" PRIu64 "\n",
			    which,
			    (int)bdb_key.size, (char *)bdb_key.data, keyno);
			return (1);
		}
	}
	if (value.size != bdb_value.size ||
	    memcmp(value.data, bdb_value.data, value.size) != 0) {
		fprintf(stderr, "wts_np: %s value mismatch:\n", which);
		wts_stream_item("bdb-value", &bdb_value);
		wts_stream_item(" wt-value", &value);
		return (1);
	}

	if (g.logging) {
		if (g.c_file_type == ROW)
			(void)session->msg_printf(session, "%-10s{%.*s/%.*s}",
			    which,
			    (int)key.size, (char *)key.data,
			    (int)value.size, (char *)value.data);
		else
			(void)session->msg_printf(
			    session, "%-10s{%" PRIu64 "/%.*s}",
			    which, keyno, (int)value.size, (char *)value.data);
	}

	return (0);
}

/*
 * wts_row_put --
 *	Update an element in a row-store file.
 */
static int
wts_row_put(uint64_t keyno, int insert)
{
	static WT_ITEM key, value;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	int notfound, ret;

	cursor = g.wts_cursor;
	session = g.wts_session;

	key_gen(&key.data, &key.size, keyno, insert);
	value_gen(&value.data, &value.size, keyno);

	/* Log the operation */
	if (g.logging)
		(void)session->msg_printf(session, "%-10s{%.*s}\n%-10s{%.*s}",
		    insert ? "insertK" : "putK",
		    (int)key.size, (char *)key.data,
		    insert ? "insertV" : "putV",
		    (int)value.size, (char *)value.data);

	if (bdb_put(key.data, key.size, value.data, value.size, &notfound))
		return (1);

	cursor->set_key(cursor, &key);
	cursor->set_value(cursor, &value);
	ret = cursor->insert(cursor);
	if (ret != 0 && ret != WT_NOTFOUND) {
		fprintf(stderr,
                    "%s: wts_row_put: %s row %" PRIu64 " by key: %s\n",
		    g.progname,
		    insert ? "insert" : "put", keyno, wiredtiger_strerror(ret));
		return (1);
	}

	NTF_CHK(wts_notfound_chk("wts_row_put", ret, notfound, keyno));
	return (0);
}

/*
 * wts_col_put --
 *	Update an element in a column-store file.
 */
static int
wts_col_put(uint64_t keyno)
{
	static WT_ITEM key, value;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	int notfound, ret;

	cursor = g.wts_cursor;
	session = g.wts_session;

	key_gen(&key.data, &key.size, keyno, 0);
	value_gen(&value.data, &value.size, keyno);

	/* Log the operation */
	if (g.logging) {
		if (g.c_file_type == FIX)
			(void)session->msg_printf(session,
			    "%-10s%" PRIu64 " {0x%02" PRIx8 "}",
			    "put", keyno,
			    ((uint8_t *)value.data)[0]);
		else
			(void)session->msg_printf(session,
			    "%-10s%" PRIu64 " {%.*s}",
			    "put", keyno,
			    (int)value.size, (char *)value.data);
	}

	cursor->set_key(cursor, keyno);
	if (g.c_file_type == FIX)
		cursor->set_value(cursor, *(uint8_t *)value.data);
	else
		cursor->set_value(cursor, &value);
	ret = cursor->update(cursor);
	if (ret != 0 && ret != WT_NOTFOUND) {
		fprintf(stderr,
                    "%s: wts_col_put: %" PRIu64 " : %s\n",
		    g.progname, keyno, wiredtiger_strerror(ret));
		return (1);
	}

	if (bdb_put(key.data, key.size, value.data, value.size, &notfound))
		return (1);

	NTF_CHK(wts_notfound_chk("wts_col_put", ret, notfound, keyno));
	return (0);
}

/*
 * wts_col_insert --
 *	Insert an element in a column-store file.
 */
static int
wts_col_insert(uint64_t *keynop)
{
	static WT_ITEM key, value;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	uint64_t keyno;
	int notfound, ret;

	cursor = g.wts_cursor_insert;
	session = g.wts_session;

	value_gen(&value.data, &value.size, g.c_rows + 1);

	if (g.c_file_type == FIX)
		cursor->set_value(cursor, *(uint8_t *)value.data);
	else
		cursor->set_value(cursor, &value);
	ret = cursor->insert(cursor);
	if (ret != 0) {
		fprintf(stderr, "%s: wts_col_insert: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}
	if ((ret = cursor->get_key(cursor, &keyno)) != 0) {
		fprintf(stderr, "%s: cursor->get_key: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}
	if (keyno <= g.c_rows) {
		fprintf(stderr,
		    "%s: inserted key did not create new row\n", g.progname);
		return (1);
	}
	*keynop = g.c_rows = (uint32_t)keyno;

	if (g.logging) {
		if (g.c_file_type == FIX)
			(void)session->msg_printf(session,
			    "%-10s%" PRIu64 " {0x%02" PRIx8 "}",
			    "insert", keyno,
			    ((uint8_t *)value.data)[0]);
		else
			(void)session->msg_printf(session,
			    "%-10s%" PRIu64 " {%.*s}",
			    "insert", keyno,
			    (int)value.size, (char *)value.data);
	}

	key_gen(&key.data, &key.size, keyno, 0);
	return (bdb_put(
	    key.data, key.size, value.data, value.size, &notfound) ? 1 : 0);
}

/*
 * wts_row_del --
 *	Delete an element from a row-store file.
 */
static int
wts_row_del(uint64_t keyno, int *notfoundp)
{
	static WT_ITEM key;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	int notfound, ret;

	*notfoundp = 0;
	cursor = g.wts_cursor;
	session = g.wts_session;

	key_gen(&key.data, &key.size, keyno, 0);

	/* Log the operation */
	if (g.logging)
		(void)session->msg_printf(
		    session, "%-10s%" PRIu64, "delete", keyno);

	if (bdb_del(keyno, &notfound))
		return (1);
	*notfoundp = notfound;

	cursor->set_key(cursor, &key);
	ret = cursor->remove(cursor);
	if (ret != 0 && ret != WT_NOTFOUND) {
		fprintf(stderr,
                    "%s: wts_row_del: remove %" PRIu64 " by key: %s\n",
		    g.progname, keyno, wiredtiger_strerror(ret));
		return (1);
	}

	NTF_CHK(wts_notfound_chk("wts_row_del", ret, notfound, keyno));
	return (0);
}

/*
 * wts_col_del --
 *	Delete an element from a column-store file.
 */
static int
wts_col_del(uint64_t keyno, int *notfoundp)
{
	static WT_ITEM key;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	int notfound, ret;

	cursor = g.wts_cursor;
	session = g.wts_session;

	/* Log the operation */
	if (g.logging)
		(void)session->msg_printf(
		    session, "%-10s%" PRIu64, "delete", keyno);

	/*
	 * Deleting a fixed-length item is the same as setting the bits to 0;
	 * do the same thing for the BDB store.
	 */
	if (g.c_file_type == FIX) {
		key_gen(&key.data, &key.size, keyno, 0);
		if (bdb_put(key.data, key.size, "\0", 1, &notfound))
			return (1);
	} else {
		if (bdb_del(keyno, &notfound))
			return (1);
		*notfoundp = notfound;
	}

	cursor->set_key(cursor, keyno);
	ret = cursor->remove(cursor);
	if (ret != 0 && ret != WT_NOTFOUND) {
		fprintf(stderr,
                    "%s: wts_col_del: remove %" PRIu64 " by key: %s\n",
		    g.progname, keyno, wiredtiger_strerror(ret));
		return (1);
	}

	NTF_CHK(wts_notfound_chk("wts_col_del", ret, notfound, keyno));
	return (0);
}

/*
 * wts_notfound_chk --
 *	Compare notfound returns for consistency.
 */
static int
wts_notfound_chk(const char *f, int wt_ret, int bdb_notfound, uint64_t keyno)
{
	/* Check for not found status. */
	if (bdb_notfound) {
		if (wt_ret == WT_NOTFOUND)
			return (2);

		fprintf(stderr, "%s: %s:", g.progname, f);
		if (keyno != 0)
			fprintf(stderr, " row %" PRIu64 ":", keyno);
		fprintf(stderr,
		    " not found in Berkeley DB, found in WiredTiger\n");
		return (1);
	}
	if (wt_ret == WT_NOTFOUND) {
		fprintf(stderr, "%s: %s:", g.progname, f);
		if (keyno != 0)
			fprintf(stderr, " row %" PRIu64 ":", keyno);
		fprintf(stderr,
		    " found in Berkeley DB, not found in WiredTiger\n");
		return (1);
	}
	return (0);
}

/*
 * wts_stream_item --
 *	Dump a single data/size pair, with a tag.
 */
static void
wts_stream_item(const char *tag, WT_ITEM *item)
{
	static const char hex[] = "0123456789abcdef";
	const uint8_t *data;
	uint32_t size;
	int ch;

	data = item->data;
	size = item->size;

	fprintf(stderr, "\t%s {", tag);
	if (g.c_file_type == FIX)
		fprintf(stderr, "0x%02x", data[0]);
	else
		for (; size > 0; --size, ++data) {
			ch = data[0];
			if (isprint(ch))
				fprintf(stderr, "%c", ch);
			else
				fprintf(stderr, "%x%x",
				    hex[(data[0] & 0xf0) >> 4],
				    hex[data[0] & 0x0f]);
		}
	fprintf(stderr, "}\n");
}
