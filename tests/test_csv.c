#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csv.h"
#include "test.h"

static void check_cell_str(const td_sheet_t *s, size_t r, size_t c,
                           const char *expected)
{
    td_val_t v = td_sheet_get_cell(s, r, c);
    char buf[128];
    size_t n;

    TD_CHECK(v.type == TD_TYPE_ANY || v.type == TD_TYPE_STR);
    n = td_val_format(v, TD_TYPE_ANY, buf, sizeof(buf));
    TD_CHECK(n < sizeof(buf));
    TD_CHECK_STR(buf, expected);
}

static void test_csv_delimiter_detect(void)
{
    const char *csv_sample = "name,age,city\nAlice,30,NYC\n";
    const char *tsv_sample = "name\tage\tcity\nAlice\t30\tNYC\n";
    const char *quoted_comma_tsv = "\"Smith, Alice\"\t30\tNYC\n";

    TD_CHECK_INT(td_csv_detect_delimiter("sales.csv", tsv_sample,
                                         strlen(tsv_sample)), ',');
    TD_CHECK_INT(td_csv_detect_delimiter("SALES.TSV", csv_sample,
                                         strlen(csv_sample)), '\t');
    TD_CHECK_INT(td_csv_detect_delimiter("data.tab", csv_sample,
                                         strlen(csv_sample)), '\t');
    TD_CHECK_INT(td_csv_detect_delimiter("data.txt", csv_sample,
                                         strlen(csv_sample)), ',');
    TD_CHECK_INT(td_csv_detect_delimiter("data.txt", tsv_sample,
                                         strlen(tsv_sample)), '\t');
    TD_CHECK_INT(td_csv_detect_delimiter(NULL, quoted_comma_tsv,
                                         strlen(quoted_comma_tsv)), '\t');
    TD_CHECK_INT(td_csv_detect_delimiter(NULL, "", 0), ',');
}

static void test_csv_rfc4180_and_bom(void)
{
    /* Includes UTF-8 BOM, CRLF, quoted commas, doubled quotes, embedded
     * CRLF/LF, empty fields, ragged row, and no trailing newline. */
    const char *raw =
        "\xEF\xBB\xBF"
        "name,note,qty\r\n"
        "\"Smith, Alice\",\"he said \"\"hi\"\"\",42\r\n"
        "Bob,\"line1\r\nline2\",\n"
        "Cara,\"\",7,extra";
    char err[128];
    td_sheet_t *s = td_csv_load_mem("sample.csv", "/tmp/sample.csv",
                                    raw, strlen(raw), NULL, err, sizeof(err));

    TD_CHECK(s != NULL);
    if (!s)
        return;

    TD_CHECK_STR(err, "");
    TD_CHECK_INT(s->ncols, 4);
    TD_CHECK_INT(s->nrows, 3);
    TD_CHECK_STR(s->cols[0].name, "name");
    TD_CHECK_STR(s->cols[1].name, "note");
    TD_CHECK_STR(s->cols[2].name, "qty");
    TD_CHECK_STR(s->cols[3].name, "col4");

    /* Zero-copy slice check on unescaped quoted field. */
    td_val_t c00 = td_sheet_get_cell(s, 0, 0);
    TD_CHECK_INT(c00.type, TD_TYPE_ANY);
    TD_CHECK(c00.as.s.data >= raw && c00.as.s.data < raw + strlen(raw));
    check_cell_str(s, 0, 0, "Smith, Alice");

    check_cell_str(s, 0, 1, "he said \"hi\"");
    check_cell_str(s, 0, 2, "42");
    TD_CHECK_INT(td_sheet_get_cell(s, 0, 3).type, TD_TYPE_NULL);

    check_cell_str(s, 1, 0, "Bob");
    check_cell_str(s, 1, 1, "line1\nline2");
    TD_CHECK_INT(td_sheet_get_cell(s, 1, 2).type, TD_TYPE_NULL);

    check_cell_str(s, 2, 0, "Cara");
    TD_CHECK_INT(td_sheet_get_cell(s, 2, 1).type, TD_TYPE_NULL);
    check_cell_str(s, 2, 2, "7");
    check_cell_str(s, 2, 3, "extra");

    td_sheet_free(s);
}

static void test_csv_no_header_and_edge_cases(void)
{
    td_csv_opts_t opts;
    char err[128];
    td_sheet_t *s;

    td_csv_opts_default(&opts);
    opts.has_header = false;
    opts.delimiter = ';';

    const char *raw = "10;20\r30;40;";
    s = td_csv_load_mem("nohdr", "", raw, strlen(raw), &opts, err, sizeof(err));
    TD_CHECK(s != NULL);
    if (s) {
        TD_CHECK_INT(s->ncols, 3);
        TD_CHECK_INT(s->nrows, 2);
        TD_CHECK_STR(s->cols[0].name, "col1");
        TD_CHECK_STR(s->cols[1].name, "col2");
        TD_CHECK_STR(s->cols[2].name, "col3");
        check_cell_str(s, 0, 0, "10");
        check_cell_str(s, 0, 1, "20");
        TD_CHECK_INT(td_sheet_get_cell(s, 0, 2).type, TD_TYPE_NULL);
        check_cell_str(s, 1, 0, "30");
        check_cell_str(s, 1, 1, "40");
        TD_CHECK_INT(td_sheet_get_cell(s, 1, 2).type, TD_TYPE_NULL);
        td_sheet_free(s);
    }

    /* Strict TSV treats quotes literally. */
    td_csv_opts_default(&opts);
    opts.delimiter = '\t';
    opts.strict_tsv = true;
    const char *tsv = "a\tb\n\"x\ty\n";
    s = td_csv_load_mem("tsv", "", tsv, strlen(tsv), &opts, err, sizeof(err));
    TD_CHECK(s != NULL);
    if (s) {
        TD_CHECK_INT(s->ncols, 2);
        TD_CHECK_INT(s->nrows, 1);
        check_cell_str(s, 0, 0, "\"x");
        check_cell_str(s, 0, 1, "y");
        td_sheet_free(s);
    }

    /* Empty input produces valid empty sheet. */
    s = td_csv_load_mem("empty", "", "", 0, NULL, err, sizeof(err));
    TD_CHECK(s != NULL);
    if (s) {
        TD_CHECK_INT(s->ncols, 0);
        TD_CHECK_INT(s->nrows, 0);
        td_sheet_free(s);
    }

    /* Unterminated quote produces error cleanly without leaks. */
    const char *bad = "a,b\n\"unterminated,2\n";
    s = td_csv_load_mem("bad", "", bad, strlen(bad), NULL, err, sizeof(err));
    TD_CHECK(s == NULL);
    TD_CHECK(err[0] != '\0');
}

static void test_csv_load_file_and_fd(void)
{
    char dir[] = "/tmp/td_csv_test_XXXXXX";
    char path[128];
    char err[128];
    FILE *fp;
    td_sheet_t *s;
    int fds[2];

    TD_CHECK(mkdtemp(dir) != NULL);
    snprintf(path, sizeof(path), "%s/items.tsv", dir);

    fp = fopen(path, "wb");
    TD_CHECK(fp != NULL);
    if (fp) {
        fputs("item\tprice\nwidget\t12.50\ngadget\t99.00\n", fp);
        fclose(fp);
    }

    s = td_csv_load_file(path, NULL, err, sizeof(err));
    TD_CHECK(s != NULL);
    if (s) {
        TD_CHECK(s->mmap_base != NULL);
        TD_CHECK(s->mmap_len > 0);
        TD_CHECK_STR(s->name, "items.tsv");
        TD_CHECK_STR(s->source_path, path);
        TD_CHECK_INT(s->ncols, 2);
        TD_CHECK_INT(s->nrows, 2);
        check_cell_str(s, 0, 0, "widget");
        check_cell_str(s, 1, 1, "99.00");
        td_sheet_free(s);
    }

    /* Missing file error check. */
    s = td_csv_load_file("/tmp/definitely_missing_td_file_12345.csv",
                         NULL, err, sizeof(err));
    TD_CHECK(s == NULL);
    TD_CHECK(err[0] != '\0');

    /* Piped fd streaming check. */
    TD_CHECK_INT(pipe(fds), 0);
    const char *pipe_data = "k,v\nalpha,1\nbeta,2\n";
    ssize_t wr = write(fds[1], pipe_data, strlen(pipe_data));
    TD_CHECK_INT(wr, (ssize_t)strlen(pipe_data));
    close(fds[1]);

    s = td_csv_load_fd(fds[0], "stdin", NULL, err, sizeof(err));
    close(fds[0]);
    TD_CHECK(s != NULL);
    if (s) {
        TD_CHECK(s->buf_base != NULL);
        TD_CHECK_INT(s->buf_len, strlen(pipe_data));
        TD_CHECK(s->mmap_base == NULL);
        TD_CHECK_INT(s->ncols, 2);
        TD_CHECK_INT(s->nrows, 2);
        check_cell_str(s, 0, 0, "alpha");
        check_cell_str(s, 1, 1, "2");
        td_sheet_free(s);
    }

    remove(path);
    rmdir(dir);
}

static void test_csv_format_and_save(void)
{
    char dir[] = "/tmp/td_csv_save_XXXXXX";
    char csv_path[128];
    char tsv_path[128];
    char bad_path[160];
    char err[128];
    td_sheet_t *s;
    td_sheet_t *reloaded;
    tam_str_t out;

    TD_CHECK(mkdtemp(dir) != NULL);
    snprintf(csv_path, sizeof(csv_path), "%s/out.csv", dir);
    snprintf(tsv_path, sizeof(tsv_path), "%s/out.tsv", dir);
    snprintf(bad_path, sizeof(bad_path), "%s/no_such_dir/out.csv", dir);

    s = td_sheet_new("export", "");
    TD_CHECK(s != NULL);
    if (!s)
        return;

    td_sheet_add_col(s, "id", TD_TYPE_INT, 5);
    td_sheet_add_col(s, "secret", TD_TYPE_STR, 0); /* hidden */
    td_sheet_add_col(s, "note", TD_TYPE_STR, 20);
    td_sheet_add_col(s, "keycol", TD_TYPE_STR, 10);
    s->cols[3].is_key = true; /* pinned: should export first */

    TD_CHECK_INT(td_sheet_add_row(s), 0);
    TD_CHECK_INT(td_sheet_add_row(s), 1);

    td_sheet_set_cell(s, 0, 0, td_val_parse(td_slice_from_cstr("10"), TD_TYPE_INT));
    td_sheet_set_cell(s, 0, 1, td_val_parse(td_slice_from_cstr("skip"), TD_TYPE_STR));
    td_sheet_set_cell(s, 0, 2,
                      td_val_parse(td_slice_from_cstr("a, \"b\"\nc"), TD_TYPE_STR));
    td_sheet_set_cell(s, 0, 3, td_val_parse(td_slice_from_cstr("k0"), TD_TYPE_STR));

    td_sheet_set_cell(s, 1, 0, td_val_parse(td_slice_from_cstr("20"), TD_TYPE_INT));
    /* col 2 left NULL on row 1 */
    td_sheet_set_cell(s, 1, 3, td_val_parse(td_slice_from_cstr("k\t1"), TD_TYPE_STR));

    /* Permute row_order so row 1 exports before row 0. */
    s->row_order[0] = 1;
    s->row_order[1] = 0;

    out = td_csv_format_sheet(s, ',');
    TD_CHECK(out != NULL);
    if (out) {
        TD_CHECK_STR(out, "keycol,id,note\nk\t1,20,\nk0,10,\"a, \"\"b\"\"\nc\"\n");
        tam_str_free(out);
    }

    out = td_csv_format_sheet(s, '\t');
    TD_CHECK(out != NULL);
    if (out) {
        TD_CHECK_STR(out, "keycol\tid\tnote\nk 1\t20\t\nk0\t10\ta, \"b\" c\n");
        tam_str_free(out);
    }

    /* Atomic save to CSV (delimiter inferred as ','), then reload and verify. */
    TD_CHECK(td_csv_save_sheet(s, csv_path, '\0', err, sizeof(err)));
    TD_CHECK_STR(err, "");
    TD_CHECK_STR(s->source_path, csv_path);

    reloaded = td_csv_load_file(csv_path, NULL, err, sizeof(err));
    TD_CHECK(reloaded != NULL);
    if (reloaded) {
        TD_CHECK_INT(reloaded->ncols, 3);
        TD_CHECK_INT(reloaded->nrows, 2);
        TD_CHECK_STR(reloaded->cols[0].name, "keycol");
        TD_CHECK_STR(reloaded->cols[1].name, "id");
        TD_CHECK_STR(reloaded->cols[2].name, "note");
        check_cell_str(reloaded, 0, 0, "k\t1");
        check_cell_str(reloaded, 0, 1, "20");
        TD_CHECK_INT(td_sheet_get_cell(reloaded, 0, 2).type, TD_TYPE_NULL);
        check_cell_str(reloaded, 1, 0, "k0");
        check_cell_str(reloaded, 1, 1, "10");
        check_cell_str(reloaded, 1, 2, "a, \"b\"\nc");
        td_sheet_free(reloaded);
    }

    /* Atomic save to TSV (delimiter inferred as '\t'). */
    TD_CHECK(td_csv_save_sheet(s, tsv_path, '\0', err, sizeof(err)));
    reloaded = td_csv_load_file(tsv_path, NULL, err, sizeof(err));
    TD_CHECK(reloaded != NULL);
    if (reloaded) {
        TD_CHECK_INT(reloaded->ncols, 3);
        TD_CHECK_INT(reloaded->nrows, 2);
        check_cell_str(reloaded, 0, 0, "k 1");
        check_cell_str(reloaded, 1, 2, "a, \"b\" c");
        td_sheet_free(reloaded);
    }

    /* Error path: unwritable destination directory. */
    TD_CHECK(!td_csv_save_sheet(s, bad_path, '\0', err, sizeof(err)));
    TD_CHECK(err[0] != '\0');

    td_sheet_free(s);
    remove(csv_path);
    remove(tsv_path);
    rmdir(dir);
}

void test_csv(void)
{
    test_csv_delimiter_detect();
    test_csv_rfc4180_and_bom();
    test_csv_no_header_and_edge_cases();
    test_csv_load_file_and_fd();
    test_csv_format_and_save();
}
