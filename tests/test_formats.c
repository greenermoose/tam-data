#include "test.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "formats.h"
#include "json.h"
#include "sheet.h"
#include "tam.h"

void test_formats(void)
{
    char err[256] = "";

    /* JSON array of objects */
    {
        static const char j[] =
            "[{\"id\": 1, \"name\": \"alpha\", \"score\": 9.5},"
            " {\"id\": 2, \"name\": \"beta\", \"extra\": true}]";
        td_sheet_t *s = td_json_load_mem("j", NULL, j, sizeof(j) - 1, false,
                                         err, sizeof(err));
        tam_str_t out;
        td_sheet_t *reloaded;

        TD_CHECK(s != NULL);
        TD_CHECK_INT(s->nrows, 2);
        TD_CHECK_INT(s->ncols, 4);
        TD_CHECK_STR(s->cols[0].name, "id");
        TD_CHECK_STR(s->cols[3].name, "extra");

        out = td_json_format_sheet(s, false);
        TD_CHECK(out != NULL);
        reloaded = td_json_load_mem("j2", NULL, out, tam_str_len(out), false,
                                    err, sizeof(err));
        TD_CHECK(reloaded != NULL);
        TD_CHECK_INT(reloaded->nrows, 2);
        TD_CHECK_INT(reloaded->ncols, 4);
        td_sheet_free(reloaded);
        tam_str_free(out);
        td_sheet_free(s);
    }

    /* JSONL */
    {
        static const char jl[] =
            "{\"k\": \"a\", \"v\": 10}\n"
            "{\"k\": \"b\", \"v\": 20}\n";
        td_sheet_t *s = td_json_load_mem("jl", NULL, jl, sizeof(jl) - 1, true,
                                         err, sizeof(err));
        tam_str_t out;

        TD_CHECK(s != NULL);
        TD_CHECK_INT(s->nrows, 2);
        TD_CHECK_INT(s->ncols, 2);
        out = td_json_format_sheet(s, true);
        TD_CHECK(out != NULL);
        tam_str_free(out);
        td_sheet_free(s);
    }

    /* INI via libtam */
    {
        static const char ini[] =
            "[server]\nhost = localhost\nport = 8080\n";
        td_sheet_t *s = td_ini_load_mem("cfg", NULL, ini, sizeof(ini) - 1,
                                        err, sizeof(err));
        char buf[64];

        TD_CHECK(s != NULL);
        TD_CHECK_INT(s->nrows, 2);
        TD_CHECK_INT(s->ncols, 3);
        td_val_format(td_sheet_get_cell(s, 0, 0), TD_TYPE_STR, buf, sizeof(buf));
        TD_CHECK_STR(buf, "server");
        td_val_format(td_sheet_get_cell(s, 1, 2), TD_TYPE_STR, buf, sizeof(buf));
        TD_CHECK_STR(buf, "8080");
        td_sheet_free(s);
    }

    /* Fixed-width table */
    {
        static const char fwf[] =
            "ID   NAME     SCORE\n"
            "1    Alice    98.5\n"
            "2    Bob      87.0\n";
        td_sheet_t *s = td_fwf_load_mem("fwf", NULL, fwf, sizeof(fwf) - 1, true,
                                        err, sizeof(err));
        char buf[64];

        TD_CHECK(s != NULL);
        TD_CHECK_INT(s->nrows, 2);
        TD_CHECK_INT(s->ncols, 3);
        TD_CHECK_STR(s->cols[0].name, "ID");
        TD_CHECK_STR(s->cols[1].name, "NAME");
        TD_CHECK_STR(s->cols[2].name, "SCORE");
        td_val_format(td_sheet_get_cell(s, 1, 1), TD_TYPE_STR, buf, sizeof(buf));
        TD_CHECK_STR(buf, "Bob");
        td_sheet_free(s);
    }

    /* Format detection */
    TD_CHECK_INT(td_format_from_path("a.json"), TD_FMT_JSON);
    TD_CHECK_INT(td_format_from_path("a.jsonl"), TD_FMT_JSONL);
    TD_CHECK_INT(td_format_from_path("a.ini"), TD_FMT_INI);
    TD_CHECK_INT(td_format_from_path("a.fwf"), TD_FMT_FWF);
}
