#include "hclib.h"
#ifdef __cplusplus
#include "hclib_cpp.h"
#include "hclib_system.h"
#ifdef __CUDACC__
#include "hclib_cuda.h"
#endif
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>
#include <omp.h>
#include <assert.h>
#include <time.h>

#define MAX_ARGS 10
#define REC_LENGTH 49	// size of a record in db
#define REC_WINDOW 10	// number of records to read at a time
#define LATITUDE_POS 28	// location of latitude coordinates in input record
#define OPEN 10000	// initial value of nearest neighbors
struct neighbor {
	char entry[REC_LENGTH];
	double dist;
};

/**
* This program finds the k-nearest neighbors
* Usage:	./nn <filelist> <num> <target latitude> <target longitude>
*			filelist: File with the filenames to the records
*			num: Number of nearest neighbors to find
*			target lat: Latitude coordinate for distance calculations
*			target long: Longitude coordinate for distance calculations
* The filelist and data are generated by hurricane_gen.c
* REC_WINDOW has been arbitrarily assigned; A larger value would allow more work for the threads
*/
typedef struct _pragma127_omp_parallel {
    long long (*time0_ptr);
    FILE (*(*flist_ptr));
    FILE (*(*fp_ptr));
    int i;
    int (*j_ptr);
    int (*k_ptr);
    int (*rec_count_ptr);
    int (*done_ptr);
    char (*sandbox_ptr)[490];
    char (*rec_iter);
    char (*(*rec_iter2_ptr));
    char (*dbname_ptr)[64];
    struct neighbor (*(*neighbors_ptr));
    float (*target_lat_ptr);
    float (*target_long_ptr);
    float (*tmp_lat_ptr);
    float (*tmp_long_ptr);
    char (*filename_buf_ptr)[1024];
    char (*(*rodinia_data_dir_ptr));
    float (*(*z_ptr));
    int (*argc_ptr);
    char (*(*(*argv_ptr)));
 } pragma127_omp_parallel;


#ifdef OMP_TO_HCLIB_ENABLE_GPU

class pragma127_omp_parallel_hclib_async {
    private:

    public:
        __host__ __device__ void operator()(int idx) {
        }
};

#else
static void pragma127_omp_parallel_hclib_async(void *____arg, const int ___iter0);
#endif
typedef struct _main_entrypoint_ctx {
    long long time0;
    FILE (*flist);
    FILE (*fp);
    int i;
    int j;
    int k;
    int rec_count;
    int done;
    char sandbox[490];
    char (*rec_iter);
    char (*rec_iter2);
    char dbname[64];
    struct neighbor (*neighbors);
    float target_lat;
    float target_long;
    float tmp_lat;
    float tmp_long;
    char filename_buf[1024];
    char (*rodinia_data_dir);
    float (*z);
    int argc;
    char (*(*argv));
 } main_entrypoint_ctx;


static void main_entrypoint(void *____arg) {
    main_entrypoint_ctx *ctx = (main_entrypoint_ctx *)____arg;
    long long time0; time0 = ctx->time0;
    FILE (*flist); flist = ctx->flist;
    FILE (*fp); fp = ctx->fp;
    int i; i = ctx->i;
    int j; j = ctx->j;
    int k; k = ctx->k;
    int rec_count; rec_count = ctx->rec_count;
    int done; done = ctx->done;
    char sandbox[490]; memcpy(sandbox, ctx->sandbox, 490 * (sizeof(char))); 
    char (*rec_iter); rec_iter = ctx->rec_iter;
    char (*rec_iter2); rec_iter2 = ctx->rec_iter2;
    char dbname[64]; memcpy(dbname, ctx->dbname, 64 * (sizeof(char))); 
    struct neighbor (*neighbors); neighbors = ctx->neighbors;
    float target_lat; target_lat = ctx->target_lat;
    float target_long; target_long = ctx->target_long;
    float tmp_lat; tmp_lat = ctx->tmp_lat;
    float tmp_long; tmp_long = ctx->tmp_long;
    char filename_buf[1024]; memcpy(filename_buf, ctx->filename_buf, 1024 * (sizeof(char))); 
    char (*rodinia_data_dir); rodinia_data_dir = ctx->rodinia_data_dir;
    float (*z); z = ctx->z;
    int argc; argc = ctx->argc;
    char (*(*argv)); argv = ctx->argv;
while(!done) {
		//Read in REC_WINDOW number of records
		rec_count = fread(sandbox, REC_LENGTH, REC_WINDOW, fp);
		if( rec_count != REC_WINDOW ) {
			if(!ferror(flist)) {// an eof occured
				fclose(fp);

				if(feof(flist))
		  			done = 1;
				else {
	 				if(fscanf(flist, "%s\n", dbname) != 1) {
	    					fprintf(stderr, "error reading filelist\n");
	    					exit(0);
					}


                    memcpy(filename_buf + strlen(rodinia_data_dir), dbname, strlen(dbname) + 1);
	  				fp = fopen(filename_buf, "r");

	  				if(!fp) {
					    printf("error opening a db\n");
					    exit(1);
	  				}
				}
			} else {
				perror("Error");
				exit(0);
			}
		}

 { 
pragma127_omp_parallel *new_ctx = (pragma127_omp_parallel *)malloc(sizeof(pragma127_omp_parallel));
new_ctx->time0_ptr = &(time0);
new_ctx->flist_ptr = &(flist);
new_ctx->fp_ptr = &(fp);
new_ctx->i = i;
new_ctx->j_ptr = &(j);
new_ctx->k_ptr = &(k);
new_ctx->rec_count_ptr = &(rec_count);
new_ctx->done_ptr = &(done);
new_ctx->sandbox_ptr = &(sandbox);
new_ctx->rec_iter = rec_iter;
new_ctx->rec_iter2_ptr = &(rec_iter2);
new_ctx->dbname_ptr = &(dbname);
new_ctx->neighbors_ptr = &(neighbors);
new_ctx->target_lat_ptr = &(target_lat);
new_ctx->target_long_ptr = &(target_long);
new_ctx->tmp_lat_ptr = &(tmp_lat);
new_ctx->tmp_long_ptr = &(tmp_long);
new_ctx->filename_buf_ptr = &(filename_buf);
new_ctx->rodinia_data_dir_ptr = &(rodinia_data_dir);
new_ctx->z_ptr = &(z);
new_ctx->argc_ptr = &(argc);
new_ctx->argv_ptr = &(argv);
hclib_loop_domain_t domain[1];
domain[0].low = 0;
domain[0].high = rec_count;
domain[0].stride = 1;
domain[0].tile = -1;
#ifdef OMP_TO_HCLIB_ENABLE_GPU
hclib::future_t *fut = hclib::forasync_cuda((rec_count) - (0), pragma127_omp_parallel_hclib_async(), hclib::get_closest_gpu_locale());
fut->wait();
#else
hclib_future_t *fut = hclib_forasync_future((void *)pragma127_omp_parallel_hclib_async, new_ctx, 1, domain, HCLIB_FORASYNC_MODE);
hclib_future_wait(fut);
#endif
free(new_ctx);
 } 

		
        for( i = 0 ; i < rec_count ; i++ ) {
			float max_dist = -1;
			int max_idx = 0;
			// find a neighbor with greatest dist and take his spot if allowed!
			for( j = 0 ; j < k ; j++ ) {
				if( neighbors[j].dist > max_dist ) {
					max_dist = neighbors[j].dist;
					max_idx = j;
				}
			}
			// compare each record with max value to find the nearest neighbor
			if( z[i] < neighbors[max_idx].dist ) {
				sandbox[(i+1)*REC_LENGTH-1] = '\0';
			  	strcpy(neighbors[max_idx].entry, sandbox +i*REC_LENGTH);
			  	neighbors[max_idx].dist = z[i];
			}
		}
	} ;     free(____arg);
}

int main(int argc, char* argv[]) {
	long long time0 = clock();
    FILE   *flist,*fp;
	int    i=0,j=0, k=0, rec_count=0, done=0;
	char   sandbox[REC_LENGTH * REC_WINDOW], *rec_iter,*rec_iter2, dbname[64];
	struct neighbor *neighbors = NULL;
	float target_lat, target_long, tmp_lat=0, tmp_long=0;

    char filename_buf[1024];
    char *rodinia_data_dir = getenv("RODINIA_DATA_DIR");
    assert(rodinia_data_dir);
    memcpy(filename_buf, rodinia_data_dir, strlen(rodinia_data_dir));

    if(argc < 5) {
        fprintf(stderr, "Invalid set of arguments\n");
        exit(-1);
    }

	flist = fopen(argv[1], "r");
    if(!flist) {
        printf("error opening flist\n");
        exit(1);
    }

	k = atoi(argv[2]);
	target_lat = atof(argv[3]);
	target_long = atof(argv[4]);

	neighbors = (struct neighbor *)malloc(k*sizeof(struct neighbor));

    if(neighbors == NULL) {
        fprintf(stderr, "no room for neighbors\n");
        exit(0);
    }

	for( j = 0 ; j < k ; j++ ) { //Initialize list of nearest neighbors to very large dist
		neighbors[j].dist = OPEN;
	}

	/**** main processing ****/  
	if(fscanf(flist, "%s\n", dbname) != 1) {
		fprintf(stderr, "error reading filelist\n");
		exit(0);
	}

    memcpy(filename_buf + strlen(rodinia_data_dir), dbname, strlen(dbname) + 1);

	fp = fopen(filename_buf, "r");
	if(!fp) {
		printf("error opening file %s\n", filename_buf);
		exit(1);
	}

	float *z;
	z  = (float *) malloc(REC_WINDOW * sizeof(float));

main_entrypoint_ctx *new_ctx = (main_entrypoint_ctx *)malloc(sizeof(main_entrypoint_ctx));
new_ctx->time0 = time0;
new_ctx->flist = flist;
new_ctx->fp = fp;
new_ctx->i = i;
new_ctx->j = j;
new_ctx->k = k;
new_ctx->rec_count = rec_count;
new_ctx->done = done;
memcpy(new_ctx->sandbox, sandbox, 490 * (sizeof(char))); 
new_ctx->rec_iter = rec_iter;
new_ctx->rec_iter2 = rec_iter2;
memcpy(new_ctx->dbname, dbname, 64 * (sizeof(char))); 
new_ctx->neighbors = neighbors;
new_ctx->target_lat = target_lat;
new_ctx->target_long = target_long;
new_ctx->tmp_lat = tmp_lat;
new_ctx->tmp_long = tmp_long;
memcpy(new_ctx->filename_buf, filename_buf, 1024 * (sizeof(char))); 
new_ctx->rodinia_data_dir = rodinia_data_dir;
new_ctx->z = z;
new_ctx->argc = argc;
new_ctx->argv = argv;
const char *deps[] = { "system" };
hclib_launch(main_entrypoint, new_ctx, deps, 1);
//End while loop

	fprintf(stderr, "The %d nearest neighbors are:\n", k);
	for( j = 0 ; j < k ; j++ ) {
		if( !(neighbors[j].dist == OPEN) )
			fprintf(stderr, "%s --> %f\n", neighbors[j].entry, neighbors[j].dist);
	}

	fclose(flist);
	

    long long time1 = clock();
    printf("total time : %15.12f s\n", (float) (time1 - time0) / 1000000);
    return 0;
}  
static void pragma127_omp_parallel_hclib_async(void *____arg, const int ___iter0) {
    pragma127_omp_parallel *ctx = (pragma127_omp_parallel *)____arg;
    int i; i = ctx->i;
    char (*rec_iter); rec_iter = ctx->rec_iter;
    hclib_start_finish();
    do {
    i = ___iter0;
{
			rec_iter = (*(ctx->sandbox_ptr))+(i * REC_LENGTH + LATITUDE_POS - 1);
            float tmp_lat = atof(rec_iter);
            float tmp_long = atof(rec_iter+5);
			(*(ctx->z_ptr))[i] = sqrt(( (tmp_lat-(*(ctx->target_lat_ptr))) * (tmp_lat-(*(ctx->target_lat_ptr))) )+( (tmp_long-(*(ctx->target_long_ptr))) * (tmp_long-(*(ctx->target_long_ptr))) ));
        } ;     } while (0);
    ; hclib_end_finish_nonblocking();

}



