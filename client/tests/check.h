#ifndef SHYAKE_TEST_CHECK_H
#define SHYAKE_TEST_CHECK_H

#include <stdio.h>
#include <string.h>

/* Minimal test harness: each failed check prints its line and the
 * test binary exits non-zero. */
static int check_failures;
static int check_count;

#define CHECK(cond)                                                            \
	do {                                                                   \
		check_count++;                                                 \
		if (!(cond)) {                                                 \
			check_failures++;                                      \
			fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, \
				__LINE__, #cond);                              \
		}                                                              \
	} while (0)

#define CHECK_STR(got, want)                                                 \
	do {                                                                 \
		const char *g_ = (got), *w_ = (want);                        \
		check_count++;                                               \
		if (!g_ || strcmp(g_, w_) != 0) {                            \
			check_failures++;                                    \
			fprintf(stderr, "%s:%d: got \"%s\", want \"%s\"\n",  \
				__FILE__, __LINE__, g_ ? g_ : "(null)", w_); \
		}                                                            \
	} while (0)

static int check_report(const char *name)
{
	printf("%s: %d checks, %d failed\n", name, check_count, check_failures);
	return check_failures ? 1 : 0;
}

#endif /* SHYAKE_TEST_CHECK_H */
