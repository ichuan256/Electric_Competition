#ifndef RED_ETS_SELFTEST_H
#define RED_ETS_SELFTEST_H

#ifdef __cplusplus
extern "C" {
#endif

/* Returns zero on success, otherwise the failing check number. */
int red_ets_selftest_run(void);

#ifdef __cplusplus
}
#endif
#endif
