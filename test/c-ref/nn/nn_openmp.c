#include "hclib.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>
#include <omp.h>
#include <assert.h>

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
typedef struct _pragma117 {
    int argc;
    char **argv;
    long long time0;
    FILE *flist;
    FILE *fp;
    int i;
    int j;
    int k;
    int rec_count;
    int done;
    char sandbox[490];
    char *rec_iter;
    char *rec_iter2;
    char dbname[64];
    struct neighbor *neighbors;
    float target_lat;
    float target_long;
    float tmp_lat;
    float tmp_long;
    char filename_buf[1024];
    char *rodinia_data_dir;
    float *z;
 } pragma117;

static void pragma117_hclib_async(void *____arg, const int ___iter);
typedef struct _main_entrypoint_ctx {
    int argc;
    char **argv;
    long long time0;
    FILE *flist;
    FILE *fp;
    int i;
    int j;
    int k;
    int rec_count;
    int done;
    char sandbox[490];
    char *rec_iter;
    char *rec_iter2;
    char dbname[64];
    struct neighbor *neighbors;
    float target_lat;
    float target_long;
    float tmp_lat;
    float tmp_long;
    char filename_buf[1024];
    char *rodinia_data_dir;
    float *z;
 } main_entrypoint_ctx;

static void main_entrypoint(void *____arg) {
    main_entrypoint_ctx *ctx = (main_entrypoint_ctx *)____arg;
    int argc; argc = ctx->argc;
    char **argv; argv = ctx->argv;
    long long time0; time0 = ctx->time0;
    FILE *flist; flist = ctx->flist;
    FILE *fp; fp = ctx->fp;
    int i; i = ctx->i;
    int j; j = ctx->j;
    int k; k = ctx->k;
    int rec_count; rec_count = ctx->rec_count;
    int done; done = ctx->done;
    char sandbox[490]; memcpy(sandbox, ctx->sandbox, 490 * (sizeof(char))); 
    char *rec_iter; rec_iter = ctx->rec_iter;
    char *rec_iter2; rec_iter2 = ctx->rec_iter2;
    char dbname[64]; memcpy(dbname, ctx->dbname, 64 * (sizeof(char))); 
    struct neighbor *neighbors; neighbors = ctx->neighbors;
    float target_lat; target_lat = ctx->target_lat;
    float target_long; target_long = ctx->target_long;
    float tmp_lat; tmp_lat = ctx->tmp_lat;
    float tmp_long; tmp_long = ctx->tmp_long;
    char filename_buf[1024]; memcpy(filename_buf, ctx->filename_buf, 1024 * (sizeof(char))); 
    char *rodinia_data_dir; rodinia_data_dir = ctx->rodinia_data_dir;
    float *z; z = ctx->z;
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
pragma117 *ctx = (pragma117 *)malloc(sizeof(pragma117));
ctx->argc = argc;
ctx->argv = argv;
ctx->time0 = time0;
ctx->flist = flist;
ctx->fp = fp;
ctx->i = i;
ctx->j = j;
ctx->k = k;
ctx->rec_count = rec_count;
ctx->done = done;
memcpy(ctx->sandbox, sandbox, 490 * (sizeof(char))); 
ctx->rec_iter = rec_iter;
ctx->rec_iter2 = rec_iter2;
memcpy(ctx->dbname, dbname, 64 * (sizeof(char))); 
ctx->neighbors = neighbors;
ctx->target_lat = target_lat;
ctx->target_long = target_long;
ctx->tmp_lat = tmp_lat;
ctx->tmp_long = tmp_long;
memcpy(ctx->filename_buf, filename_buf, 1024 * (sizeof(char))); 
ctx->rodinia_data_dir = rodinia_data_dir;
ctx->z = z;
hclib_loop_domain_t domain;
domain.low = 0;
domain.high = rec_count;
domain.stride = 1;
domain.tile = 1;
hclib_future_t *fut = hclib_forasync_future((void *)pragma117_hclib_async, ctx, NULL, 1, &domain, FORASYNC_MODE_RECURSIVE);
hclib_future_wait(fut);
free(ctx);
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
	} ; }

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

main_entrypoint_ctx *ctx = (main_entrypoint_ctx *)malloc(sizeof(main_entrypoint_ctx));
ctx->argc = argc;
ctx->argv = argv;
ctx->time0 = time0;
ctx->flist = flist;
ctx->fp = fp;
ctx->i = i;
ctx->j = j;
ctx->k = k;
ctx->rec_count = rec_count;
ctx->done = done;
memcpy(ctx->sandbox, sandbox, 490 * (sizeof(char))); 
ctx->rec_iter = rec_iter;
ctx->rec_iter2 = rec_iter2;
memcpy(ctx->dbname, dbname, 64 * (sizeof(char))); 
ctx->neighbors = neighbors;
ctx->target_lat = target_lat;
ctx->target_long = target_long;
ctx->tmp_lat = tmp_lat;
ctx->tmp_long = tmp_long;
memcpy(ctx->filename_buf, filename_buf, 1024 * (sizeof(char))); 
ctx->rodinia_data_dir = rodinia_data_dir;
ctx->z = z;
hclib_launch(main_entrypoint, ctx);
free(ctx);
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
}  static void pragma117_hclib_async(void *____arg, const int ___iter) {
    pragma117 *ctx = (pragma117 *)____arg;
    int argc; argc = ctx->argc;
    char **argv; argv = ctx->argv;
    long long time0; time0 = ctx->time0;
    FILE *flist; flist = ctx->flist;
    FILE *fp; fp = ctx->fp;
    int i; i = ctx->i;
    int j; j = ctx->j;
    int k; k = ctx->k;
    int rec_count; rec_count = ctx->rec_count;
    int done; done = ctx->done;
    char sandbox[490]; memcpy(sandbox, ctx->sandbox, 490 * (sizeof(char))); 
    char *rec_iter; rec_iter = ctx->rec_iter;
    char *rec_iter2; rec_iter2 = ctx->rec_iter2;
    char dbname[64]; memcpy(dbname, ctx->dbname, 64 * (sizeof(char))); 
    struct neighbor *neighbors; neighbors = ctx->neighbors;
    float target_lat; target_lat = ctx->target_lat;
    float target_long; target_long = ctx->target_long;
    float tmp_lat; tmp_lat = ctx->tmp_lat;
    float tmp_long; tmp_long = ctx->tmp_long;
    char filename_buf[1024]; memcpy(filename_buf, ctx->filename_buf, 1024 * (sizeof(char))); 
    char *rodinia_data_dir; rodinia_data_dir = ctx->rodinia_data_dir;
    float *z; z = ctx->z;
    hclib_start_finish();
    do {
    i = ___iter;
{
			rec_iter = sandbox+(i * REC_LENGTH + LATITUDE_POS - 1);
            float tmp_lat = atof(rec_iter);
            float tmp_long = atof(rec_iter+5);
			z[i] = sqrt(( (tmp_lat-target_lat) * (tmp_lat-target_lat) )+( (tmp_long-target_long) * (tmp_long-target_long) ));
        } ;     } while (0);
    ; hclib_end_finish();
}



