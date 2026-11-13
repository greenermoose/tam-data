#include "test.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "csv.h"
#include "metasheets.h"
#include "options.h"
#include "sheet.h"
#include "tam-data.h"

static td_sheet_t *make_sample(const char *name)
{
    static const char data[] = "city,pop\nnyc,8\nsf,1\n";
    char err[128] = "";
    td_csv_opts_t opts;

    td_csv_opts_default(&opts);
    return td_csv_load_mem(name, NULL, data, sizeof(data) - 1, &opts,
                           err, sizeof(err));
}

void test_metasheets(void)
{
    td_stack_t st = { 0 };
    td_sheet_t *s1 = make_sample("cities");
    td_sheet_t *s2 = make_sample("other");
    td_sheet_t *sh_meta;
    td_sheet_t *col_meta;
    td_sheet_t *opt_meta;
    td_sheet_t *help_meta;
    td_sheet_t *dir_meta;
    td_options_t opts;
    char tmp_cfg[128];
    char err[256] = "";
    int fd;

    TD_CHECK(s1 && s2);
    TD_CHECK(td_stack_push(&st, s1));
    TD_CHECK(td_stack_push(&st, s2));

    /* Sheets metasheet: switch and drop */
    sh_meta = td_meta_sheets_build(&st, false);
    TD_CHECK(sh_meta != NULL);
    TD_CHECK_INT(sh_meta->kind, TD_SHEET_SHEETS);
    TD_CHECK_INT(sh_meta->nrows, 2);
    TD_CHECK(td_stack_push(&st, sh_meta));
    sh_meta->cursor_row = 1; /* 'cities' */
    (void)td_meta_dive_row(sh_meta, &st, NULL, NULL, err, sizeof(err));
    TD_CHECK_INT(st.len, 2);
    TD_CHECK(td_stack_active(&st) == s1);

    sh_meta = td_meta_sheets_build(&st, false);
    TD_CHECK(td_stack_push(&st, sh_meta));
    sh_meta->cursor_row = 1; /* 'other' */
    TD_CHECK(td_meta_sheets_drop_row(sh_meta, &st, 1, err, sizeof(err)));
    TD_CHECK_INT(st.len, 2); /* sh_meta + s1 */
    td_sheet_free(td_stack_pop(&st));

    /* Columns metasheet: live rename & type change */
    col_meta = td_meta_cols_build(&st, false);
    TD_CHECK(col_meta != NULL);
    TD_CHECK_INT(col_meta->kind, TD_SHEET_COLS);
    TD_CHECK_INT(col_meta->nrows, 2);
    TD_CHECK(td_sheet_edit_cell(col_meta, 0, 1, "metro"));
    TD_CHECK(td_meta_cols_apply_edit(col_meta, &st, 0, 1, err, sizeof(err)));
    TD_CHECK_STR(s1->cols[0].name, "metro");
    TD_CHECK(td_sheet_edit_cell(col_meta, 1, 2, "int"));
    TD_CHECK(td_meta_cols_apply_edit(col_meta, &st, 1, 2, err, sizeof(err)));
    TD_CHECK_INT(s1->cols[1].type, TD_TYPE_INT);
    td_sheet_free(col_meta);

    /* Options metasheet: auto-save to temp config.ini and reload */
    snprintf(tmp_cfg, sizeof(tmp_cfg), "/tmp/td_opts_%d.ini", (int)getpid());
    td_options_init(&opts);
    opts.config_path = strdup(tmp_cfg);
    opt_meta = td_meta_options_build(&opts);
    TD_CHECK(opt_meta != NULL);
    TD_CHECK_INT(opt_meta->kind, TD_SHEET_OPTIONS);
    TD_CHECK(td_sheet_edit_cell(opt_meta, 2, 1, "60")); /* auto_width_cap */
    TD_CHECK(td_meta_options_apply_edit(opt_meta, &opts, 2, 1, err, sizeof(err)));
    TD_CHECK_INT(opts.auto_width_cap, 60);
    td_sheet_free(opt_meta);
    td_options_free(&opts);

    td_options_init(&opts);
    TD_CHECK(td_options_load(&opts, tmp_cfg, err, sizeof(err)));
    TD_CHECK_INT(opts.auto_width_cap, 60);
    td_options_free(&opts);
    unlink(tmp_cfg);

    /* Help metasheet */
    help_meta = td_meta_help_build();
    TD_CHECK(help_meta != NULL);
    TD_CHECK_INT(help_meta->kind, TD_SHEET_HELP);
    TD_CHECK(help_meta->nrows > 50);
    {
        int cmd_out = 0;

        help_meta->cursor_row = 0;
        (void)td_meta_dive_row(help_meta, &st, NULL, &cmd_out, err, sizeof(err));
        TD_CHECK(cmd_out != 0);
    }
    td_sheet_free(help_meta);

    /* Directory metasheet + dive into CSV file */
    {
        char tmp_csv[128];
        td_sheet_t *opened = NULL;

        snprintf(tmp_csv, sizeof(tmp_csv), "/tmp/td_dir_test_%d.csv", (int)getpid());
        fd = open(tmp_csv, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        TD_CHECK(fd >= 0);
        if (fd >= 0) {
            const char *csv = "a,b\n1,2\n";
            (void)write(fd, csv, strlen(csv));
            close(fd);
        }
        dir_meta = td_meta_dir_build("/tmp", err, sizeof(err));
        TD_CHECK(dir_meta != NULL);
        TD_CHECK_INT(dir_meta->kind, TD_SHEET_DIR);
        TD_CHECK(dir_meta->nrows > 0);
        for (size_t r = 0; r < dir_meta->nrows; r++) {
            td_val_t v = td_sheet_get_cell(dir_meta, r, 5);
            char pbuf[256];

            td_val_format(v, TD_TYPE_STR, pbuf, sizeof(pbuf));
            if (strcmp(pbuf, tmp_csv) == 0) {
                dir_meta->cursor_row = r;
                opened = td_meta_dive_row(dir_meta, &st, NULL, NULL,
                                          err, sizeof(err));
                break;
            }
        }
        TD_CHECK(opened != NULL);
        if (opened) {
            TD_CHECK_INT(opened->nrows, 1);
            TD_CHECK_INT(opened->ncols, 2);
            td_sheet_free(opened);
        }
        td_sheet_free(dir_meta);
        unlink(tmp_csv);
    }

    td_stack_free(&st);
}
