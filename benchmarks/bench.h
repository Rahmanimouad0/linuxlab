/*
 * Shared helpers for the benchmarks.
 *
 * Methodology used by every benchmark:
 *   - one untimed warm-up run
 *   - N timed repetitions, report the MEDIAN (robust against the occasional
 *     outlier caused by the scheduler, page cache, frequency scaling ...)
 *   - output either as an aligned table or as CSV (--csv), one row per result:
 *       benchmark,variant,parameter,value,unit
 */
#ifndef LINUXLAB_BENCH_H
#define LINUXLAB_BENCH_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int bench_csv;
static int bench_quick; /* --quick: tiny sizes, used by the test suite */

static void bench_parse_args(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--csv") == 0)
            bench_csv = 1;
        else if (strcmp(argv[i], "--quick") == 0)
            bench_quick = 1;
        else {
            fprintf(stderr, "usage: %s [--csv] [--quick]\n", argv[0]);
            exit(2);
        }
    }
    if (bench_csv)
        printf("benchmark,variant,parameter,value,unit\n");
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double median(double *v, int n)
{
    qsort(v, (size_t)n, sizeof *v, cmp_double);
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

static void bench_header(const char *title)
{
    if (!bench_csv)
        printf("\n%s\n%-28s %-16s %14s  %s\n", title, "variant", "parameter", "median", "unit");
}

static void report(const char *bench, const char *variant, const char *param, double value,
                   const char *unit)
{
    if (bench_csv)
        printf("%s,%s,%s,%.4f,%s\n", bench, variant, param, value, unit);
    else
        printf("%-28s %-16s %14.3f  %s\n", variant, param, value, unit);
    fflush(stdout);
}

#endif
