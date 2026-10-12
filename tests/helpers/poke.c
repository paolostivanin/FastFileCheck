// test helper: poke hash <dbdir> <path> | meta_back <dbdir> <seconds> | cursor <dbdir> | lastrun <dbdir> | seed <dbdir>
#include <lmdb.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/time.h>
typedef struct { uint32_t magic, ver; uint64_t cursor; int64_t last_run_us; } Meta;
int main (int argc, char **argv) {
    if (argc < 3) return 2;
    MDB_env *env; MDB_txn *txn; MDB_dbi dbi;
    mdb_env_create (&env); mdb_env_set_mapsize (env, 50UL << 20);
    if (mdb_env_open (env, argv[2], 0, 0644)) { perror ("open"); return 1; }
    mdb_txn_begin (env, NULL, 0, &txn); mdb_dbi_open (txn, NULL, 0, &dbi);
    int rc = 1;
    if (!strcmp (argv[1], "hash") && argc == 4) {
        MDB_val k = { strlen (argv[3]), argv[3] }, v;
        if (mdb_get (txn, dbi, &k, &v) == 0) {
            unsigned char buf[256]; memcpy (buf, v.mv_data, v.mv_size); buf[0] ^= 0xFF;
            MDB_val nv = { v.mv_size, buf }; rc = mdb_put (txn, dbi, &k, &nv, 0);
        }
    } else if (!strcmp (argv[1], "seed")) {
        MDB_val k = { 12, "/legacy/file" }, v = { 40, (char[40]){ 0 } };
        rc = mdb_put (txn, dbi, &k, &v, 0);
    } else if (!strcmp (argv[1], "meta_back") && argc == 4) {
        MDB_val k = { 9, "\0ffc:meta" }, v;
        if (mdb_get (txn, dbi, &k, &v) == 0) {
            Meta m; memset (&m, 0, sizeof m); memcpy (&m, v.mv_data, v.mv_size < sizeof m ? v.mv_size : sizeof m);
            struct timeval tv; gettimeofday (&tv, NULL);
            m.last_run_us = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec - (int64_t)(atof (argv[3]) * 1e6);
            MDB_val nv = { sizeof m, &m }; rc = mdb_put (txn, dbi, &k, &nv, 0);
        }
    } else if (!strcmp (argv[1], "lastrun")) {
        MDB_val k = { 9, "\0ffc:meta" }, v;
        if (mdb_get (txn, dbi, &k, &v) == 0) { Meta m; memcpy (&m, v.mv_data, sizeof m); printf ("%.3f\n", m.last_run_us / 1e6); mdb_txn_abort (txn); mdb_env_close (env); return 0; }
    } else if (!strcmp (argv[1], "cursor")) {
        MDB_val k = { 9, "\0ffc:meta" }, v;
        if (mdb_get (txn, dbi, &k, &v) == 0) { Meta m; memcpy (&m, v.mv_data, sizeof m); printf ("%.4f\n", (double)m.cursor / 18446744073709551616.0); rc = 0; mdb_txn_abort (txn); mdb_env_close (env); return 0; }
    }
    if (rc == 0) mdb_txn_commit (txn); else mdb_txn_abort (txn);
    mdb_env_close (env);
    return rc ? 1 : 0;
}
