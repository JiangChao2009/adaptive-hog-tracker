/**************************************************************************
 * Desc: Simple particle filter for localization.
 * Author: Andrew Howard
 * Date: 10 Dec 2002
 * CVS: $Id: pf.c,v 1.14 2006/01/20 02:44:25 gerkey Exp $
 *************************************************************************/

#include <assert.h>
#include <math.h>
#include <stdlib.h>

#include "pf.h"
#include "pf_pdf.h"
#include "pf_kdtree.h"

// Compute the required number of samples, given that there are k bins
// with samples in them.
static int pf_resample_limit(pf_t *pf, int k);
static int
		pf_resample_limit_hyps(pf_t *pf, int k, double pop_z, double pop_err);

// Re-compute the cluster statistics for a sample set
static void pf_cluster_stats(pf_t *pf, pf_sample_set_t *set);

// Create a new filter
pf_t *pf_alloc(int min_samples, int max_samples, int overHead_samples) {
	int i, j;
	pf_t *pf;
	pf_sample_set_t *set;
	pf_sample_t *sample;

	pf = (pf_t*) calloc(1, sizeof(pf_t));

	pf->min_samples = min_samples;
	pf->max_samples = max_samples;
	pf->overHead_samples = overHead_samples;

	pf->sumSquareWeights = 0.0;

	// Control parameters for the population size calculation.  [err] is
	// the max error between the true distribution and the estimated
	// distribution.  [z] is the upper standard normal quantile for (1 -
	// p), where p is the probability that the error on the estimated
	// distrubition will be less than [err].
	pf->pop_err = 0.01;
	pf->pop_z = 3;

	pf->current_set = 0;
	for (j = 0; j < 2; j++) {
		set = pf->sets + j;

		set->sample_count = max_samples;
		set->samples = (pf_sample_t*) calloc(max_samples, sizeof(pf_sample_t));

		for (i = 0; i < set->sample_count; i++) {
			sample = set->samples + i;
			sample->pose.v[0] = 0.0;
			sample->pose.v[1] = 0.0;
			sample->pose.v[2] = 0.0;
			sample->weight = 1.0 / max_samples;
		}

		// HACK: is 3 times max_samples enough?
		set->kdtree = pf_kdtree_alloc(3 * max_samples);

		set->cluster_count = 0;
		set->cluster_max_count = 100;
		set->clusters = (pf_cluster_t*) calloc(set->cluster_max_count, sizeof(pf_cluster_t));
	}

	gsl_rng_env_setup();

	return pf;
}

// Free an existing filter
void pf_free(pf_t *pf) {
	int i;

	for (i = 0; i < 2; i++) {
		free(pf->sets[i].clusters);
		pf_kdtree_free(pf->sets[i].kdtree);
		free(pf->sets[i].samples);
	}
	free(pf);

	return;
}

// Initialize the filter using a guassian
void pf_init(pf_t *pf, pf_vector_t mean, pf_matrix_t cov) {
	int i;
	pf_sample_set_t *set;
	pf_sample_t *sample;
	pf_pdf_gaussian_t *pdf;

	set = pf->sets + pf->current_set;

	// Create the kd tree for adaptive sampling
	pf_kdtree_clear(set->kdtree);

	set->sample_count = pf->max_samples;

	pdf = pf_pdf_gaussian_alloc(mean, cov);

	// Compute the new sample poses
	for (i = 0; i < set->sample_count; i++) {
		sample = set->samples + i;
		sample->weight = 1.0 / pf->max_samples;
		sample->pose = pf_pdf_gaussian_sample(pdf);

		// Add sample to histogram
		pf_kdtree_insert(set->kdtree, sample->pose, sample->weight);
	}

	pf_pdf_gaussian_free(pdf);

	// Re-compute cluster statistics
	pf_cluster_stats(pf, set);

	return;
}

// Initialize the filter using a UNIFORM distribution and
// discart pose in the storange area
void pf_init_map(pf_t *pf, map_t *map) {
	int i;
	pf_sample_set_t *set;
	pf_sample_t *sample;

	set = pf->sets + pf->current_set;

	// Create the kd tree for adaptive sampling
	pf_kdtree_clear(set->kdtree);

	set->sample_count = pf->max_samples;

	// Setup random environment for UNIFORM distribution
	const gsl_rng_type * T;
	gsl_rng * r;
	T = gsl_rng_default;
	r = gsl_rng_alloc(T);
	gsl_rng_set(r, (unsigned long int) time(NULL));

	//pdf = pf_pdf_gaussian_alloc(mean, cov);

	// Compute the new sample poses
	for (i = 0; i < set->sample_count; i++) {
		sample = set->samples + i;
		sample->weight = 1.0 / pf->max_samples;

		// tmp pose, the sembple cannot be positioned in a storange area
		pf_vector_t tmp;
		int correct = 0; // true if the pose is correct
		do {
			tmp.v[0] = (gsl_rng_uniform_pos(r) - 0.5) * map->size_x
					* map->scale;
			tmp.v[1] = (gsl_rng_uniform_pos(r) - 0.5) * map->size_y
					* map->scale;
			tmp.v[2] = 0.0; //(gsl_rng_uniform(r) - 0.5) * 2 * M_PI; ///////occhio MODIFICAAAAAAAAAAAA

			int ix = MAP_GXWX(map, tmp.v[0]);
			int iy = MAP_GYWY(map, tmp.v[1]);
			if (MAP_VALID(map, ix, iy)
					&& map->cells[MAP_INDEX(map, ix, iy)].occ_state == -1) {
				correct = 1;
			}
		} while (!correct);

		sample->pose = tmp;

		//    	sample->pose = pf_pdf_gaussian_sample(pdf);

		// Add sample to histogram
		pf_kdtree_insert(set->kdtree, sample->pose, sample->weight);
	}

	gsl_rng_free(r);

	// Re-compute cluster statistics
	pf_cluster_stats(pf, set);

	return;
}

// Initialize the filter using some model
void pf_init_model(pf_t *pf, pf_init_model_fn_t init_fn, void *init_data) {
	int i;
	pf_sample_set_t *set;
	pf_sample_t *sample;

	set = pf->sets + pf->current_set;

	// Create the kd tree for adaptive sampling
	pf_kdtree_clear(set->kdtree);

	set->sample_count = pf->max_samples;

	// Compute the new sample poses
	for (i = 0; i < set->sample_count; i++) {
		sample = set->samples + i;
		sample->weight = 1.0 / pf->max_samples;
		sample->pose = (*init_fn)(init_data);

		// Add sample to histogram
		pf_kdtree_insert(set->kdtree, sample->pose, sample->weight);
	}

	// Re-compute cluster statistics
	pf_cluster_stats(pf, set);

	return;
}

// Update the filter with some new action
void pf_update_action(pf_t *pf, pf_action_model_fn_t action_fn,
		void *action_data) {
	pf_sample_set_t *set;

	set = pf->sets + pf->current_set;

	(*action_fn)(action_data, set);

	return;
}

// Update the filter with some new action
void pf_update_action_update_cluster(pf_t *pf, pf_action_model_fn_t action_fn,
		void *action_data) {
	pf_sample_set_t *set;

	set = pf->sets + pf->current_set;

	// Create the kd tree for adaptive sampling
	pf_kdtree_clear(set->kdtree);

	(*action_fn)(action_data, set);

	int i;
	pf_sample_t *sample;
	for (i = 0; i < set->sample_count; i++) {
		sample = set->samples + i;
		//  	printf("%d %f\n", i, sample->weight);
		// Add sample to histogram
		pf_kdtree_insert(set->kdtree, sample->pose, sample->weight);
	}

	// Re-compute cluster statistics
	pf_cluster_stats(pf, set);

	return;
}

void pf_cluster_set(pf_sample_set_t *set) {
	int i;
	pf_sample_t *sample;

	set->kdtree = pf_kdtree_alloc(3 * set->sample_count);
	set->cluster_count = 0;
	set->cluster_max_count = 100;
	set->clusters = (pf_cluster_t*) calloc(set->cluster_max_count,
			sizeof(pf_cluster_t));

	// Create the kd tree for adaptive sampling
	pf_kdtree_clear(set->kdtree);

	for (i = 0; i < set->sample_count; i++) {
		sample = set->samples + i;

		//  	if( i < 10 )
		//  	{
		//		printf("%d %f\n", i, sample->weight);
		//  	}
		//
		//  	printf("%d %f %f %f %f\n", i, sample->pose.v[0],
		//  									sample->pose.v[1],
		//  									sample->pose.v[2],
		//  									sample->weight);
		// Add sample to histogram
		pf_kdtree_insert(set->kdtree, sample->pose, sample->weight);
	}

	// Re-compute cluster statistics
	pf_cluster_stats(NULL, set);
}

// Update the filter with some new sensor observation
double pf_update_sensor(pf_t *pf, pf_sensor_model_fn_t sensor_fn,
		void *sensor_data) {

	// short and long average
//	static double w_fast = 0.0;
//	static double w_slow = 0.0;
//	// short and long average parameter
//	static double a_fast = 0.5;
//	static double a_slow = 0.1;
//	FILE *fileAvg = fopen("weightAvg.csv", "a");
//	double w_avg;

	int i;
	pf_sample_set_t *set;
	pf_sample_t *sample;
	double total;
	double squareWeightSum;

	squareWeightSum = 0.0;

	set = pf->sets + pf->current_set;

	// Compute the sample weights
	total = (*sensor_fn)(sensor_data, set);

//	w_avg = total / set->sample_count;
//	w_fast = w_fast + a_fast * (w_avg - w_fast);
//	w_slow = w_slow + a_slow * (w_avg - w_slow);
//
//	double r = 1 - w_fast/w_slow;

//	printf("%f,%f,%f,%f\n", w_avg, w_fast, w_slow, r);
//	fprintf(fileAvg, "%ld,%f,%d\n", time(NULL), total, set->sample_count);

	if (total > 0.0) {

		// Normalize weights
		for (i = 0; i < set->sample_count; i++) {
			sample = set->samples + i;
			sample->weight /= total;

			squareWeightSum += pow(sample->weight,2);

		}
	} else {
		printf("pdf has zero probability\n");

		// Handle zero total
		for (i = 0; i < set->sample_count; i++) {
			sample = set->samples + i;
			sample->weight = 1.0 / set->sample_count;

			squareWeightSum += pow(sample->weight,2);
		}
	}

//	fclose(fileAvg);

	return squareWeightSum;
}

// Resample the distribution
void pf_update_resample(pf_t *pf, const int nMaxParticles) {
	int i;
	double total;
	double *randlist;
	pf_sample_set_t *set_a, *set_b;
	pf_sample_t *sample_a, *sample_b;
	pf_pdf_discrete_t *pdf;

	set_a = pf->sets + pf->current_set;
	set_b = pf->sets + (pf->current_set + 1) % 2;

	//  printf("NORMAL BEGIN set_a->sample_count = %d \n", set_a->sample_count);

	// Create the discrete distribution to sample from
	total = 0;
	randlist = (double*) calloc(set_a->sample_count, sizeof(double));
	for (i = 0; i < set_a->sample_count; i++) {
		total += set_a->samples[i].weight;
		randlist[i] = set_a->samples[i].weight;
		//totalsq += pow(set_a->samples[i].weight,2);
	}

	// Initialize the random number generator
	pdf = pf_pdf_discrete_alloc(set_a->sample_count, randlist);

	// Create the kd tree for adaptive sampling
	pf_kdtree_clear(set_b->kdtree);

	// Draw samples from set a to create set b.
	total = 0;
	set_b->sample_count = 0;
	while (set_b->sample_count < nMaxParticles )//pf->max_samples)
	//  while (set_b->sample_count < (pf->max_samples - pf->overHead_samples))
	{
		i = pf_pdf_discrete_sample(pdf);
		sample_a = set_a->samples + i;

		//printf("%d %f\n", i, sample_a->weight);
		assert(sample_a->weight > 0);

		// Add sample to list
		sample_b = set_b->samples + set_b->sample_count++;
		sample_b->pose = sample_a->pose;
		sample_b->weight = 1.0;
		total += sample_b->weight;

		// Add sample to histogram
		pf_kdtree_insert(set_b->kdtree, sample_b->pose, sample_b->weight);

		//fprintf(stderr, "resample %d %d %d\n", set_b->sample_count, set_b->kdtree->leaf_count,
		//        pf_resample_limit(pf, set_b->kdtree->leaf_count));

		// See if we have enough samples yet
		if (set_b->sample_count > pf_resample_limit(pf,
				set_b->kdtree->leaf_count))
			break;
	}

	//fprintf(stderr, "\n\n");

	pf_pdf_discrete_free(pdf);
	free(randlist);

	// Normalize weights
	for (i = 0; i < set_b->sample_count; i++) {
		sample_b = set_b->samples + i;
		sample_b->weight /= total;
	}

	// Re-compute cluster statistics
	pf_cluster_stats(pf, set_b);

	// Use the newly created sample set
	pf->current_set = (pf->current_set + 1) % 2;

	//  printf("NORMAL END set_b->sample_count = %d \n", set_b->sample_count);

	return;
}

void pf_update_resample_addParticle(pf_t *pf, int nPartToAdd, map_t *map) {
	int i;
	double total;
	double *randlist;
	pf_sample_set_t *set_a, *set_b;
	pf_sample_t *sample_a, *sample_b;
	pf_pdf_discrete_t *pdf;

	set_a = pf->sets + pf->current_set;
	set_b = pf->sets + (pf->current_set + 1) % 2;

	//  printf("NORMAL BEGIN set_a->sample_count = %d \n", set_a->sample_count);

	// Create the discrete distribution to sample from
	total = 0;
	randlist = (double*) calloc(set_a->sample_count, sizeof(double));
	for (i = 0; i < set_a->sample_count; i++) {
		total += set_a->samples[i].weight;
		randlist[i] = set_a->samples[i].weight;
	}

	//printf("resample total %f\n", total);

	// Initialize the random number generator
	pdf = pf_pdf_discrete_alloc(set_a->sample_count, randlist);

	// Create the kd tree for adaptive sampling
	pf_kdtree_clear(set_b->kdtree);

	// Draw samples from set a to create set b.
	total = 0;
	set_b->sample_count = 0;
	//while (set_b->sample_count < pf->max_samples)
	while (set_b->sample_count < (pf->max_samples - nPartToAdd)) {
		i = pf_pdf_discrete_sample(pdf);
		sample_a = set_a->samples + i;

		//printf("%d %f\n", i, sample_a->weight);
		assert(sample_a->weight > 0);

		// Add sample to list
		sample_b = set_b->samples + set_b->sample_count++;
		sample_b->pose = sample_a->pose;
		sample_b->weight = 1.0;
		total += sample_b->weight;

		// Add sample to histogram
		pf_kdtree_insert(set_b->kdtree, sample_b->pose, sample_b->weight);

		//fprintf(stderr, "resample %d %d %d\n", set_b->sample_count, set_b->kdtree->leaf_count,
		//        pf_resample_limit(pf, set_b->kdtree->leaf_count));

		// See if we have enough samples yet
		if (set_b->sample_count > pf_resample_limit(pf,
				set_b->kdtree->leaf_count))
			break;
	}

	//fprintf(stderr, "\n\n");

	pf_pdf_discrete_free(pdf);
	free(randlist);

	printf("set_b->sample_count: %d\n", set_b->sample_count);

	// Setup random environment for UNIFORM distribution
	const gsl_rng_type * T;
	gsl_rng * r;
	T = gsl_rng_default;
	r = gsl_rng_alloc(T);
	gsl_rng_set(r, (unsigned long int) time(NULL));

	for (i = 0; i < nPartToAdd; i++) {
		sample_b = set_b->samples + set_b->sample_count++;
		sample_b->weight = 1.0;
		total += sample_b->weight;

		// tmp pose, the sembple cannot be positioned in a storange area
		pf_vector_t tmp;
		int correct = 0; // true if the pose is correct
		do {
			tmp.v[0] = (gsl_rng_uniform_pos(r) - 0.5) * map->size_x
					* map->scale;
			tmp.v[1] = (gsl_rng_uniform_pos(r) - 0.5) * map->size_y
					* map->scale;
			tmp.v[2] = (gsl_rng_uniform(r) - 0.5) * 2 * M_PI;

			int ix = MAP_GXWX(map, tmp.v[0]);
			int iy = MAP_GYWY(map, tmp.v[1]);
			if (MAP_VALID(map, ix, iy)
					&& map->cells[MAP_INDEX(map, ix, iy)].occ_state == -1) {
				correct = 1;
			}
		} while (!correct);

		sample_b->pose = tmp;

		//    	sample->pose = pf_pdf_gaussian_sample(pdf);

		// Add sample to histogram
		pf_kdtree_insert(set_b->kdtree, sample_b->pose, sample_b->weight);
	}

	gsl_rng_free(r);

	printf("set_b->sample_count: %d\n", set_b->sample_count);

	// Normalize weights
	for (i = 0; i < set_b->sample_count; i++) {
		sample_b = set_b->samples + i;
		sample_b->weight /= total;
	}

	// Re-compute cluster statistics
	pf_cluster_stats(pf, set_b);

	// Use the newly created sample set
	pf->current_set = (pf->current_set + 1) % 2;

	//  printf("NORMAL END set_b->sample_count = %d \n", set_b->sample_count);

	return;
}

// Resample the distribution
void pf_update_resample_map(pf_t *pf, map_t *map) {
	int i;
	double total;
	double *randlist;
	pf_sample_set_t *set_a, *set_b;
	pf_sample_t *sample_a, *sample_b;
	pf_pdf_discrete_t *pdf;

	set_a = pf->sets + pf->current_set;
	set_b = pf->sets + (pf->current_set + 1) % 2;

	// Create the discrete distribution to sample from
	total = 0;
	randlist = (double *) calloc(set_a->sample_count, sizeof(double));
	for (i = 0; i < set_a->sample_count; i++) {
		total += set_a->samples[i].weight;
		randlist[i] = set_a->samples[i].weight;
	}

	//printf("resample total %f\n", total);

	// Initialize the random number generator
	pdf = pf_pdf_discrete_alloc(set_a->sample_count, randlist);

	// Create the kd tree for adaptive sampling
	pf_kdtree_clear(set_b->kdtree);

	// Draw samples from set a to create set b.
	total = 0;
	set_b->sample_count = 0;
	//   while (set_b->sample_count < pf->max_samples)
	while (set_b->sample_count < (pf->max_samples - pf->overHead_samples)) {
		i = pf_pdf_discrete_sample(pdf);
		sample_a = set_a->samples + i;

		//printf("%d %f\n", i, sample_a->weight);
		assert(sample_a->weight > 0);

		// Add sample to list
		sample_b = set_b->samples + set_b->sample_count++;
		sample_b->pose = sample_a->pose;
		sample_b->weight = 1.0;
		total += sample_b->weight;

		// Add sample to histogram
		pf_kdtree_insert(set_b->kdtree, sample_b->pose, sample_b->weight);

		//fprintf(stderr, "resample %d %d %d\n", set_b->sample_count, set_b->kdtree->leaf_count,
		//        pf_resample_limit(pf, set_b->kdtree->leaf_count));

		// See if we have enough samples yet
		if (set_b->sample_count > pf_resample_limit(pf,
				set_b->kdtree->leaf_count))
			break;
	}

	// Setup random environment for UNIFORM distribution
	const gsl_rng_type * T;
	gsl_rng * r;
	T = gsl_rng_default;
	r = gsl_rng_alloc(T);
	gsl_rng_set(r, (unsigned long int) time(NULL));

	// add random samples
	if (set_b->sample_count < pf->min_samples + 10) {
		printf("set_b->sample_count: %d, pf->min_samples: %d\n",
				set_b->sample_count, pf->min_samples);
		for (i = 0; i < 100 && set_b->sample_count < pf->max_samples; i++) {
			sample_b = set_b->samples + set_b->sample_count++;

			pf_vector_t tmp;
			int correct = 0; // true if the pose is correct
			do {
				tmp.v[0] = (gsl_rng_uniform_pos(r) - 0.5) * map->size_x
						* map->scale;
				tmp.v[1] = (gsl_rng_uniform_pos(r) - 0.5) * map->size_y
						* map->scale;
				tmp.v[2] = (gsl_rng_uniform(r) - 0.5) * 2 * M_PI;

				int ix = MAP_GXWX(map, tmp.v[0]);
				int iy = MAP_GYWY(map, tmp.v[1]);
				if (MAP_VALID(map, ix, iy)
						&& map->cells[MAP_INDEX(map, ix, iy)].occ_state == -1) {
					correct = 1;
				}
			} while (!correct);

			sample_b->pose = tmp;
			sample_b->weight = 1.0;
			total += sample_b->weight;

			// Add sample to histogram
			pf_kdtree_insert(set_b->kdtree, sample_b->pose, sample_b->weight);
		}
	}

	gsl_rng_free(r);

	//fprintf(stderr, "\n\n");

	pf_pdf_discrete_free(pdf);
	free(randlist);

	// Normalize weights
	for (i = 0; i < set_b->sample_count; i++) {
		sample_b = set_b->samples + i;
		sample_b->weight /= total;
	}

	// Re-compute cluster statistics
	pf_cluster_stats(pf, set_b);

	// Use the newly created sample set
	pf->current_set = (pf->current_set + 1) % 2;

	return;
}

void pf_update_resample_hyps(pf_t *pf, map_t *map, int nHyp, hyp_t *hyps,
		int nParticle) {
	int i, j;
	double total;
	double *randlist;
	pf_sample_set_t *set_a, *set_b;
	pf_sample_t *sample_a, *sample_b;
	pf_pdf_discrete_t *pdf;

	set_a = pf->sets + pf->current_set;
	set_b = pf->sets + (pf->current_set + 1) % 2;

	// Create the discrete distribution to sample from
	// N.B.: all the samples have the same weight
	total = 0;
	randlist = (double*) calloc(set_a->sample_count, sizeof(double));
	for (i = 0; i < set_a->sample_count; i++) {
		total += set_a->samples[i].weight;
		randlist[i] = set_a->samples[i].weight;
	}

	// Initialize the random number generator
	pdf = pf_pdf_discrete_alloc(set_a->sample_count, randlist);

	// Create the kd tree for adaptive sampling
	pf_kdtree_clear(set_b->kdtree);

	printf("BEGIN set_b->sample_count = %d \n", set_b->sample_count);

	// Draw samples from set a to create set b.
	total = 0;
	set_b->sample_count = 0;
	j = 0;

	while (set_b->sample_count < (pf->max_samples - pf->overHead_samples)) {
		i = pf_pdf_discrete_sample(pdf);
		sample_a = set_a->samples + i;
		//    printf("%d %f\n", i, sample_a->weight);
		assert(sample_a->weight > 0);

		// Add sample to list
		sample_b = set_b->samples + set_b->sample_count++;
		sample_b->pose = sample_a->pose;
		sample_b->weight = 1.0;
		total += sample_b->weight;

		// Add sample to histogram
		pf_kdtree_insert(set_b->kdtree, sample_b->pose, sample_b->weight);

		//fprintf(stderr, "resample %d %d %d\n", set_b->sample_count, set_b->kdtree->leaf_count,
		//        pf_resample_limit(pf, set_b->kdtree->leaf_count));

		// See if we have enough samples yet
		if (set_b->sample_count > pf_resample_limit(pf,
				set_b->kdtree->leaf_count))
			break;
	}

	printf("BEGIN2 set_b->sample_count = %d \n", set_b->sample_count);

	// Setup random environment for UNIFORM distribution
	const gsl_rng_type * T;
	gsl_rng * r;
	T = gsl_rng_default;
	r = gsl_rng_alloc(T);
	gsl_rng_set(r, (unsigned long int) time(NULL));

	double rho;

	//  int nNewSample = (pf->max_samples - set_b->sample_count) / nHyp;
	int nNewSample = (pf->max_samples - set_b->sample_count);

	fprintf(stderr, "resample hyps: nNewSample %d, nParticle: %d\n",
			nNewSample, nParticle);

	if (nParticle < nNewSample)
		nNewSample = nParticle;

	nNewSample /= nHyp;

	//  if( nNewSample > pf->min_samples )
	//  	nNewSample = pf->min_samples;

	// add random samples in accord with the hypotesis
	for (j = 0; j < nHyp; j++) {
		for (i = 0; i < nNewSample; i++) {
			pf_vector_t tmp;
			int correct = 0; // true if the pose is correct
			rho = hyps[j].pf_pose_cov.m[0][1] / (hyps[j].pf_pose_cov.m[0][0]
					* hyps[j].pf_pose_cov.m[1][1]);
			//N.B. pf_pose_cov used like standard deviation, NOT a variance
			gsl_ran_bivariate_gaussian(r, hyps[j].pf_pose_cov.m[0][0],
					hyps[j].pf_pose_cov.m[1][1], rho, &tmp.v[0], &tmp.v[1]);
			tmp.v[0] += hyps[j].pf_pose_mean.v[0];
			tmp.v[1] += hyps[j].pf_pose_mean.v[1];

			// not know the heading, then uniform
			tmp.v[2] = (gsl_rng_uniform(r) - 0.5) * 2 * M_PI; //tetha[i]; //

			int ix = MAP_GXWX(map, tmp.v[0]);
			int iy = MAP_GYWY(map, tmp.v[1]);
			if (MAP_VALID(map, ix, iy)
					&& map->cells[MAP_INDEX(map, ix, iy)].occ_state == -1) {
				correct = 1;
			}

			if (correct) {
				sample_b = set_b->samples + set_b->sample_count++;
				sample_b->pose = tmp;
				sample_b->weight = 1.0;
				total += sample_b->weight;
				// Add sample to histogram
				pf_kdtree_insert(set_b->kdtree, sample_b->pose,
						sample_b->weight);
			}
		}
	}

	gsl_rng_free(r);

	printf("END set_b->sample_count = %d \n", set_b->sample_count);

	//fprintf(stderr, "\n\n");

	pf_pdf_discrete_free(pdf);
	free(randlist);

	// Normalize weights
	for (i = 0; i < set_b->sample_count; i++) {
		sample_b = set_b->samples + i;
		sample_b->weight /= total;
	}

	// Re-compute cluster statistics
	pf_cluster_stats(pf, set_b);

	// Use the newly created sample set
	pf->current_set = (pf->current_set + 1) % 2;

	return;
}

void pf_update_resample_hyps_2(pf_t *pf, map_t *map, int nHyp, hyp_t *hyps,
		int over_head) {
	int i, j;
	double total;
	double *randlist;
	pf_sample_set_t *set_a, *set_b;
	pf_sample_t *sample_a, *sample_b;
	pf_pdf_discrete_t *pdf;

	set_a = pf->sets + pf->current_set;
	set_b = pf->sets + (pf->current_set + 1) % 2;

	// Draw samples from set a to create set b.
	//total = 0;
	//set_b->sample_count = 0;
	j = 0;

	// butto particles sulle ipotesi------------------------

	// Setup random environment for UNIFORM distribution
	const gsl_rng_type * T;
	gsl_rng * r;
	T = gsl_rng_default;
	r = gsl_rng_alloc(T);
	gsl_rng_set(r, (unsigned long int) time(NULL));

	double rho;

	int nNewSample = (pf->max_samples - set_a->sample_count) / nHyp;

	//	  printf("\nParticles per me %d \n",set_a->sample_count);

	// add random samples in accord with the hypothesespf_resample_limit(pf, set_b->kdtree->leaf_count)
	for (j = 0; j < nHyp; j++) {
		for (i = 0; i < nNewSample; i++) {

			pf_vector_t tmp;
			int correct = 0; // true if the pose is correct
			rho = hyps[j].pf_pose_cov.m[0][1] / (hyps[j].pf_pose_cov.m[0][0]
					* hyps[j].pf_pose_cov.m[1][1]);
			gsl_ran_bivariate_gaussian(r, hyps[j].pf_pose_cov.m[0][0],
					hyps[j].pf_pose_cov.m[1][1], rho, &tmp.v[0], &tmp.v[1]);
			tmp.v[0] += hyps[j].pf_pose_mean.v[0];
			tmp.v[1] += hyps[j].pf_pose_mean.v[1];

			// not know the heading, then uniform
			tmp.v[2] = (gsl_rng_uniform(r) - 0.5) * 2 * M_PI; //tetha[i]; //

			int ix = MAP_GXWX(map, tmp.v[0]);
			int iy = MAP_GYWY(map, tmp.v[1]);
			if (MAP_VALID(map, ix, iy)
					&& map->cells[MAP_INDEX(map, ix, iy)].occ_state == -1) {
				correct = 1;
			}

			if (correct) {
				sample_a = set_a->samples + set_a->sample_count++;
				sample_a->pose = tmp;
				sample_a->weight = 1.0;
				//total += sample_a->weight;
				// Add sample to histogram
				//pf_kdtree_insert(set_b->kdtree, sample_b->pose, sample_b->weight);
			}
		}
	}

	//	  printf("\nParticles totali %d particles\n",set_a->sample_count);

	// Normalizzo
	for (i = 0; i < set_a->sample_count; i++) {
		set_a->samples[i].weight = 1.0 / (set_a->sample_count);
		//printf("peso: %f\n",set_a->samples[i].weight);
	}

	// Create the discrete distribution to sample from
	total = 0;
	randlist = (double*) calloc(set_a->sample_count, sizeof(double));
	for (i = 0; i < set_a->sample_count; i++) {
		total += set_a->samples[i].weight;
		randlist[i] = set_a->samples[i].weight;
	}

	// Initialize the random number generator
	pdf = pf_pdf_discrete_alloc(set_a->sample_count, randlist);

	// Create the kd tree for adaptive sampling
	pf_kdtree_clear(set_b->kdtree);

	set_b->sample_count = 0;
	// faccio resampling------------------
	while (set_b->sample_count < pf->max_samples - 1000) {
		i = pf_pdf_discrete_sample(pdf);
		sample_a = set_a->samples + i;
		//    printf("%d %f\n", i, sample_a->weight);
		assert(sample_a->weight > 0);

		// Add sample to list
		sample_b = set_b->samples + set_b->sample_count++;
		sample_b->pose = sample_a->pose;
		sample_b->weight = 1.0;
		//total += sample_b->weight;

		// Add sample to histogram
		pf_kdtree_insert(set_b->kdtree, sample_b->pose, sample_b->weight);

		//fprintf(stderr, "resample %d %d %d\n", set_b->sample_count, set_b->kdtree->leaf_count,
		//        pf_resample_limit(pf, set_b->kdtree->leaf_count));

		// See if we have enough samples yet
		if (set_b->sample_count > pf_resample_limit(pf,
				set_b->kdtree->leaf_count))
			break;
	}
	//	  printf("Particles ricampionati %d particles\n\n",set_b->sample_count);
	//-------------------------------------------

	gsl_rng_free(r);

	//fprintf(stderr, "\n\n");

	pf_pdf_discrete_free(pdf);

	free(randlist);

	// Normalize weights
	for (i = 0; i < set_b->sample_count; i++) {
		sample_b = set_b->samples + i;
		sample_b->weight /= set_b->sample_count;
	}

	// Re-compute cluster statistics
	pf_cluster_stats(pf, set_b);

	// Use the newly created sample set
	pf->current_set = (pf->current_set + 1) % 2;

	return;
}

// Compute the required number of samples, given that there are k bins
// with samples in them.  This is taken directly from Fox et al.
int pf_resample_limit(pf_t *pf, int k) {
	double a, b, c, x;
	int n;

	if (k <= 1)
		return pf->min_samples;

	a = 1;
	b = 2 / (9 * ((double) k - 1));
	c = sqrt(2 / (9 * ((double) k - 1))) * pf->pop_z;
	x = a - b + c;

	n = (int) ceil((k - 1) / (2 * pf->pop_err) * x * x * x);

	if (n < pf->min_samples)
		return pf->min_samples;
	if (n >= pf->max_samples)
		return pf->max_samples;

	return n;
}

// Compute the required number of samples, given that there are k bins
// with samples in them.  This is taken directly from Fox et al.
int pf_resample_limit_2(pf_t *pf, int k) {
	double a, b, c, x;
	int n;

	//if (k <= 1)
	//return pf->min_samples;

	a = 1;
	b = 2 / (9 * ((double) k - 1));
	c = sqrt(2 / (9 * ((double) k - 1))) * pf->pop_z;
	x = a - b + c;

	n = (int) ceil((k - 1) / (5* 2 * pf ->pop_err) * x * x * x);

	//if (n < pf->min_samples)
	//return pf->min_samples;
	//if (n >= pf->max_samples)
	//return pf->max_samples;

	return n;
}

// Compute the required number of samples, given that there are k bins
// with samples in them.  This is taken directly from Fox et al.
int pf_resample_limit_hyps(pf_t *pf, int k, double pop_z, double pop_err) {
	double a, b, c, x;
	int n;

	if (k <= 1)
		return pf->min_samples;

	a = 1;
	b = 2 / (9 * ((double) k - 1));
	c = sqrt(2 / (9 * ((double) k - 1))) * pop_z;
	x = a - b + c;

	n = (int) ceil((k - 1) / (2 * pop_err) * x * x * x);

	if (n < pf->min_samples)
		return pf->min_samples;
	if (n >= pf->max_samples)
		return pf->max_samples;

	return n;
}

// Re-compute the cluster statistics for a sample set
void pf_cluster_stats(pf_t *pf, pf_sample_set_t *set) {
	int i, j, k, c;
	pf_sample_t *sample;
	pf_cluster_t *cluster;

	// Cluster the samples
	pf_kdtree_cluster(set->kdtree);

	// Initialize cluster stats
	set->cluster_count = 0;

	for (i = 0; i < set->cluster_max_count; i++) {
		cluster = set->clusters + i;
		cluster->count = 0;
		cluster->weight = 0;
		cluster->mean = pf_vector_zero();
		cluster->cov = pf_matrix_zero();

		for (j = 0; j < 4; j++)
			cluster->m[j] = 0.0;
		for (j = 0; j < 2; j++)
			for (k = 0; k < 2; k++)
				cluster->c[j][k] = 0.0;
	}

	// Compute cluster stats
	for (i = 0; i < set->sample_count; i++) {
		sample = set->samples + i;

		//printf("%d %f %f %f\n", i, sample->pose.v[0], sample->pose.v[1], sample->pose.v[2]);

		// Get the cluster label for this sample
		c = pf_kdtree_get_cluster(set->kdtree, sample->pose);
		assert(c >= 0);
		if (c >= set->cluster_max_count)
			continue;
		if (c + 1 > set->cluster_count)
			set->cluster_count = c + 1;

		cluster = set->clusters + c;

		cluster->count += 1;
		cluster->weight += sample->weight;

		// Compute mean
		cluster->m[0] += sample->weight * sample->pose.v[0];
		cluster->m[1] += sample->weight * sample->pose.v[1];
		cluster->m[2] += sample->weight * cos(sample->pose.v[2]);
		cluster->m[3] += sample->weight * sin(sample->pose.v[2]);

		// Compute covariance in linear components
		for (j = 0; j < 2; j++)
			for (k = 0; k < 2; k++)
				cluster->c[j][k] += sample->weight * sample->pose.v[j]
						* sample->pose.v[k];
	}

	// Normalize
	for (i = 0; i < set->cluster_count; i++) {
		cluster = set->clusters + i;

		cluster->mean.v[0] = cluster->m[0] / cluster->weight;
		cluster->mean.v[1] = cluster->m[1] / cluster->weight;
		cluster->mean.v[2] = atan2(cluster->m[3], cluster->m[2]);

		cluster->cov = pf_matrix_zero();

		// Covariance in linear compontents
		for (j = 0; j < 2; j++)
			for (k = 0; k < 2; k++)
				cluster->cov.m[j][k] = cluster->c[j][k] / cluster->weight
						- cluster->mean.v[j] * cluster->mean.v[k];

		// Covariance in angular components; I think this is the correct
		// formula for circular statistics.
		cluster->cov.m[2][2] = -2 * log(sqrt(cluster->m[2] * cluster->m[2]
				+ cluster->m[3] * cluster->m[3]));

		//printf("cluster %d %d %f (%f %f %f)\n", i, cluster->count, cluster->weight,
		//       cluster->mean.v[0], cluster->mean.v[1], cluster->mean.v[2]);
		//pf_matrix_fprintf(cluster->cov, stdout, "%e");
	}

	return;
}

// Compute the CEP statistics (mean and variance).
void pf_get_cep_stats(pf_t *pf, pf_vector_t *mean, double *var) {
	int i;
	double mn, mx, my, mrr;
	pf_sample_set_t *set;
	pf_sample_t *sample;

	set = pf->sets + pf->current_set;

	mn = 0.0;
	mx = 0.0;
	my = 0.0;
	mrr = 0.0;

	for (i = 0; i < set->sample_count; i++) {
		sample = set->samples + i;

		mn += sample->weight;
		mx += sample->weight * sample->pose.v[0];
		my += sample->weight * sample->pose.v[1];
		mrr += sample->weight * sample->pose.v[0] * sample->pose.v[0];
		mrr += sample->weight * sample->pose.v[1] * sample->pose.v[1];
	}

	mean->v[0] = mx / mn;
	mean->v[1] = my / mn;
	mean->v[2] = 0.0;

	*var = mrr / mn - (mx * mx / (mn * mn) + my * my / (mn * mn));

	return;
}

// Get the statistics for a particular cluster.
int pf_get_cluster_stats(pf_t *pf, int clabel, double *weight,
		pf_vector_t *mean, pf_matrix_t *cov) {
	pf_sample_set_t *set;
	pf_cluster_t *cluster;

	set = pf->sets + pf->current_set;

	if (clabel >= set->cluster_count)
		return 0;
	cluster = set->clusters + clabel;

	*weight = cluster->weight;
	*mean = cluster->mean;
	*cov = cluster->cov;

	return 1;
}

// Get the statistics for a particular cluster.
int pf_get_cluster_stats_set(pf_sample_set_t *set, int clabel, double *weight,
		pf_vector_t *mean, pf_matrix_t *cov) {
	pf_cluster_t *cluster;

	if (clabel >= set->cluster_count)
		return 0;
	cluster = set->clusters + clabel;

	*weight = cluster->weight;
	*mean = cluster->mean;
	*cov = cluster->cov;

	return 1;
}

void pf_init_to_point(pf_t *pf, map_t *map, double x, double y, double var) {
	int i;
	pf_sample_set_t *set;
	pf_sample_t *sample;

	set = pf->sets + pf->current_set;

	// Create the kd tree for adaptive sampling
	pf_kdtree_clear(set->kdtree);

	set->sample_count = pf->max_samples;

	// Setup random environment for UNIFORM distribution
	const gsl_rng_type * T;
	gsl_rng * r;
	T = gsl_rng_default;
	r = gsl_rng_alloc(T);
	gsl_rng_set(r, (unsigned long int) time(NULL));

	//pdf = pf_pdf_gaussian_alloc(mean, cov);

	// Compute the new sample poses
	for (i = 0; i < set->sample_count; i++) {
		sample = set->samples + i;
		sample->weight = 1.0 / pf->max_samples;

		// tmp pose, the sembple cannot be positioned in a storange area
		pf_vector_t tmp;
		int correct = 0; // true if the pose is correct
		do {
			tmp.v[0] = (gsl_rng_uniform_pos(r) - 0.5) * var + x;
			tmp.v[1] = (gsl_rng_uniform_pos(r) - 0.5) * var + y;
			tmp.v[2] = (gsl_rng_uniform(r) - 0.5) * 2 * M_PI;

			int ix = MAP_GXWX(map, tmp.v[0]);
			int iy = MAP_GYWY(map, tmp.v[1]);
			if (MAP_VALID(map, ix, iy))
			//    			&& map->cells[MAP_INDEX(map, ix, iy)].occ_state == -1 )
			{
				correct = 1;
			}
		} while (!correct);

		sample->pose = tmp;

		//    	sample->pose = pf_pdf_gaussian_sample(pdf);

		// Add sample to histogram
		pf_kdtree_insert(set->kdtree, sample->pose, sample->weight);
	}

	gsl_rng_free(r);

	// Re-compute cluster statistics
	pf_cluster_stats(pf, set);

	return;
}

void pf_update_resample_hyps_3(pf_t *pf, map_t *map, int nHyp, hyp_t *hyps) {

	int i, j, nMinPart;
	double total;
	double *randlist;
	pf_sample_set_t *set_a, *set_b;
	pf_sample_t *sample_a, *sample_b;
	pf_pdf_discrete_t *pdf;

	set_a = pf->sets + pf->current_set;
	set_b = pf->sets + (pf->current_set + 1) % 2;

	j = 0;
	int ntemp;
	//int nphyptot = 0;
	// butto particles sulle ipotesi------------------------

	// Setup random environment for UNIFORM distribution
	const gsl_rng_type * T;
	gsl_rng * r;
	gsl_rng_env_setup();
	T = gsl_rng_default;
	r = gsl_rng_alloc(T);
	gsl_rng_set(r, (unsigned long int) time(NULL));

	double rho;

//	printf("Numero Particles per mia ipotesi: %d\n", set_a->sample_count);
	int nReqSamples = (pf->max_samples - set_a->sample_count);
	if (nReqSamples < pf->overHead_samples)
		nReqSamples = pf->max_samples - pf->overHead_samples;
	else
		nReqSamples = set_a->sample_count;

	// Create the discrete distribution to sample from
	// N.B.: all the samples have the same weight
	total = 0;
	randlist = (double*) calloc(set_a->sample_count, sizeof(double));
	for (i = 0; i < set_a->sample_count; i++) {
		total += set_a->samples[i].weight;
		randlist[i] = set_a->samples[i].weight;
	}

	// Initialize the random number generator
	pdf = pf_pdf_discrete_alloc(set_a->sample_count, randlist);

	// Create the kd tree for adaptive sampling
	pf_kdtree_clear(set_b->kdtree);

	//printf("BEGIN set_b->sample_count = %d \n", set_b->sample_count);

	// Draw samples from set a to create set b.
	total = 0;
	set_b->sample_count = 0;
	j = 0;

	while (set_b->sample_count < nReqSamples)//(pf->max_samples - pf->overHead_samples))
	{
		i = pf_pdf_discrete_sample(pdf);
		sample_a = set_a->samples + i;
		//    printf("%d %f\n", i, sample_a->weight);
		assert(sample_a->weight > 0);

		// Add sample to list
		sample_b = set_b->samples + set_b->sample_count++;
		sample_b->pose = sample_a->pose;
		sample_b->weight = 1.0;
		total += sample_b->weight;

		// Add sample to histogram
		pf_kdtree_insert(set_b->kdtree, sample_b->pose, sample_b->weight);

		//fprintf(stderr, "resample %d %d %d\n", set_b->sample_count, set_b->kdtree->leaf_count,
		//        pf_resample_limit(pf, set_b->kdtree->leaf_count));

		// See if we have enough samples yet
		if (set_b->sample_count > pf_resample_limit(pf,
				set_b->kdtree->leaf_count))
			break;
	}

	pf_pdf_discrete_free(pdf);

	//int nNewSample = (pf->max_samples - set_a->sample_count) / nHyp;
	int nNewSample = (pf->max_samples - nReqSamples) / nHyp;

	// TODO: OCCHI 10 hard coded
	// start adding 10 particles for each received hyps
	if (nNewSample > 10)
		nMinPart = 10;
	else
		nMinPart = nNewSample;

	// add random samples in accord with the hypotheses pf_resample_limit(pf, set_b->kdtree->leaf_count)
	for (j = 0; j < nHyp; j++) {
		// azzero set_a
		set_a->sample_count = 0;
		pf_kdtree_clear(set_a->kdtree);

		for (i = 0; i < nMinPart; i++) {
			pf_vector_t tmp;
			int correct = 0; // true if the pose is correct
			rho = hyps[j].pf_pose_cov.m[0][1] / (hyps[j].pf_pose_cov.m[0][0]
					* hyps[j].pf_pose_cov.m[1][1]);
			gsl_ran_bivariate_gaussian(r, hyps[j].pf_pose_cov.m[0][0],
					hyps[j].pf_pose_cov.m[1][1], rho, &tmp.v[0], &tmp.v[1]);
			tmp.v[0] += hyps[j].pf_pose_mean.v[0];
			tmp.v[1] += hyps[j].pf_pose_mean.v[1];

			// not know the heading, then uniform
			tmp.v[2] = 0.0;//(gsl_rng_uniform(r) - 0.5) * 2 * M_PI; //tetha[i]; //

			int ix = MAP_GXWX(map, tmp.v[0]);
			int iy = MAP_GYWY(map, tmp.v[1]);
			if (MAP_VALID(map, ix, iy)
					&& map->cells[MAP_INDEX(map, ix, iy)].occ_state == -1) {
				correct = 1;
			}

			if (correct) {
				//nphyptot++;
				sample_a = set_a->samples + set_a->sample_count++;
				sample_a->pose = tmp;
				sample_a->weight = 1.0;
				//total += sample_a->weight;
				// Add sample to histogram
				pf_kdtree_insert(set_a->kdtree, sample_a->pose,
						sample_a->weight);
			}
			//else
			//printf("FUORI: %f, %f\n", tmp.v[0], tmp.v[1]);
		}

		//esco se # di particles è sufficiente
		while (set_a->sample_count < nNewSample) {
			ntemp = pf_resample_limit_2(pf, set_a->kdtree->leaf_count);
			//printf("MUTUALDATA: Numero di particles richiesti %d\n",ntemp);
			if (set_a->sample_count > ntemp)
				break;

			pf_vector_t tmp;
			int correct = 0; // true if the pose is correct
			rho = hyps[j].pf_pose_cov.m[0][1] / (hyps[j].pf_pose_cov.m[0][0]
					* hyps[j].pf_pose_cov.m[1][1]);
			gsl_ran_bivariate_gaussian(r, hyps[j].pf_pose_cov.m[0][0],
					hyps[j].pf_pose_cov.m[1][1], rho, &tmp.v[0], &tmp.v[1]);

			tmp.v[0] += hyps[j].pf_pose_mean.v[0];
			tmp.v[1] += hyps[j].pf_pose_mean.v[1];

			// not know the heading, then uniform
			tmp.v[2] = 0.0;//(gsl_rng_uniform(r) - 0.5) * 2 * M_PI; //tetha[i]; //

			int ix = MAP_GXWX(map, tmp.v[0]);
			int iy = MAP_GYWY(map, tmp.v[1]);
			if (MAP_VALID(map, ix, iy)
					&& map->cells[MAP_INDEX(map, ix, iy)].occ_state == -1) {
				correct = 1;
			}

			if (correct) {
				//nphyptot++;
				sample_a = set_a->samples + set_a->sample_count++;
				sample_a->pose = tmp;
				sample_a->weight = 1.0;

				// Add sample to histogram
				pf_kdtree_insert(set_a->kdtree, sample_a->pose,
						sample_a->weight);
			}
		}
		//printf("Ho inserito %d particles per una delle ipotesi (in tot %d ipotesi)\n",set_a->sample_count,nHyp);

		//inserisco nuovi particles in set_b
		for (i = 0; i < set_a->sample_count; i++) {
			sample_a = set_a->samples + i;

			sample_b = set_b->samples + set_b->sample_count++;
			sample_b->pose = sample_a->pose;
			sample_b->pose.v[2] = (gsl_rng_uniform(r) - 0.5) * 2 * M_PI;
			sample_b->weight = 1.0;
			//total += sample_b->weight;
			// Add sample to histogram
			pf_kdtree_insert(set_b->kdtree, sample_b->pose, sample_b->weight);
		}
	}
	//printf("In totale ho inserito %d nuovi particles\n",nphyptot);
	//nphyptot = 0;

	//printf("Particles prima del ricampionamento: %d\n",set_b->sample_count);

	free(randlist);
	// Normalizzo
	for (i = 0; i < set_b->sample_count; i++) {
		sample_b = set_b->samples + i;
		sample_b->weight /= set_b->sample_count;
	}

	gsl_rng_free(r);

	pf_cluster_stats(pf, set_b);

	// Use the newly created sample set
	pf->current_set = (pf->current_set + 1) % 2;

	return;
}
