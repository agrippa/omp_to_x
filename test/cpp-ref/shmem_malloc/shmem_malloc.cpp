#include "hclib.h"
#ifdef __cplusplus
#include "hclib_cpp.h"
#include "hclib_system.h"
#endif
#include "shmem.h"
#include <assert.h>

int main(int argc, char **argv) {
    shmem_init();
    int MyRank = shmem_my_pe ();
    int Numprocs = shmem_n_pes ();
    printf("rank = %d size = %d\n", MyRank, Numprocs);

    int *shared_arr = (int *)shmem_malloc(10 * sizeof(int));
    assert(shared_arr);

    int i;
    for (i = 0; i < 10; i++) {
        shared_arr[i] = 3;
    }

    shmem_finalize();
    return 0;
}
