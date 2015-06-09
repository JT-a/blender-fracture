/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) Blender Foundation
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): Martin Felke
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/blenkernel/intern/fracture.c
 *  \ingroup blenkernel
 */

#include <stdio.h>
#include <stdlib.h>

#include "MEM_guardedalloc.h"

#include "BKE_cdderivedmesh.h"
#include "BKE_customdata.h"
#include "BKE_DerivedMesh.h"
#include "BKE_fracture.h"
#include "BKE_fracture_util.h"
#include "BKE_global.h"
#include "BKE_mesh.h"
#include "BKE_modifier.h"
#include "BKE_object.h"
#include "BKE_rigidbody.h"

#include "BLI_kdtree.h"
#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_mempool.h"
#include "BLI_path_util.h"
#include "BLI_rand.h"
#include "BLI_sort.h"
#include "BLI_utildefines.h"

#include "DNA_scene_types.h"
#include "DNA_fracture_types.h"
#include "DNA_gpencil_types.h"
#include "DNA_group_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_rigidbody_types.h"

#include "bmesh.h"

#include "RBI_api.h"
//#include "GPU_glew.h" /* uaahh, direct access to modelview matrix */

/* debug timing */
#define USE_DEBUG_TIMER

#ifdef USE_DEBUG_TIMER
#include "PIL_time.h"
#endif

#ifdef WITH_VORO
#include "../../../../extern/voro++/src/c_interface.hh"
#endif

/* prototypes */
static void add_shard(FracMesh *fm, Shard *s, float mat[4][4]);
static Shard *parse_cell(cell c);
static void parse_cell_verts(cell c, MVert *mvert, int totvert);
static void parse_cell_polys(cell c, MPoly *mpoly, int totpoly, int *r_totloop);
static void parse_cell_loops(cell c, MLoop *mloop, int totloop, MPoly *mpoly, int totpoly);
static void parse_cell_neighbors(cell c, int *neighbors, int totpoly);

static void add_shard(FracMesh *fm, Shard *s, float mat[4][4])
{
	MVert *mv;
	int i = 0;

	for (i = 0, mv = s->mvert; i < s->totvert; i++, mv++ )
	{
		mul_m4_v3(mat, mv->co);
	}

	mul_m4_v3(mat, s->centroid);

	BLI_addtail(&fm->shard_map, s);
	s->shard_id = fm->shard_count;
	fm->shard_count++;
}

static BMesh *shard_to_bmesh(Shard *s)
{
	DerivedMesh *dm_parent;
	BMesh *bm_parent;
	BMIter iter;
	BMFace *f;

	dm_parent = BKE_shard_create_dm(s, true);
	bm_parent = DM_to_bmesh(dm_parent, true);
	BM_mesh_elem_table_ensure(bm_parent, BM_FACE);

	BM_ITER_MESH (f, &iter, bm_parent, BM_FACES_OF_MESH)
	{
		BM_elem_flag_disable(f, BM_ELEM_SELECT);
	}

	dm_parent->needsFree = 1;
	dm_parent->release(dm_parent);
	dm_parent = NULL;

	return bm_parent;
}

static void shard_boundbox(Shard *s, float r_loc[3], float r_size[3])
{
	float min[3], max[3];
	float mloc[3], msize[3];

	if (!r_loc) r_loc = mloc;
	if (!r_size) r_size = msize;

	if (!BKE_shard_calc_minmax(s)) {
		min[0] = min[1] = min[2] = -1.0f;
		max[0] = max[1] = max[2] = 1.0f;
	}

	copy_v3_v3(max, s->max);
	copy_v3_v3(min, s->min);

	mid_v3_v3v3(r_loc, min, max);

	r_size[0] = (max[0] - min[0]) / 2.0f;
	r_size[1] = (max[1] - min[1]) / 2.0f;
	r_size[2] = (max[2] - min[2]) / 2.0f;
}

static int shard_sortsize(const void *s1, const void *s2, void* UNUSED(context))
{
	Shard **sh1 = (Shard **)s1;
	Shard **sh2 = (Shard **)s2;

	float size1[3], size2[3], loc[3];
	float val_a,  val_b;

	if ((*sh1 == NULL) || (*sh2 == NULL)) {
		return -1;
	}

	shard_boundbox(*sh1, loc, size1);
	shard_boundbox(*sh2, loc, size2);

	val_a = size1[0] * size1[1] * size1[2];
	val_b = size2[0] * size2[1] * size2[2];

	/* sort descending */
	if      (val_a < val_b) return 1;
	else if (val_a > val_b) return -1;
	return 0;
}

Shard *BKE_custom_data_to_shard(Shard *s, DerivedMesh *dm)
{
	CustomData_reset(&s->vertData);
	CustomData_reset(&s->loopData);
	CustomData_reset(&s->polyData);

	CustomData_add_layer(&s->vertData, CD_MDEFORMVERT, CD_DUPLICATE, CustomData_get_layer(&dm->vertData, CD_MDEFORMVERT), s->totvert);
	CustomData_add_layer(&s->loopData, CD_MLOOPUV, CD_DUPLICATE, CustomData_get_layer(&dm->loopData, CD_MLOOPUV), s->totloop);
	CustomData_add_layer(&s->polyData, CD_MTEXPOLY, CD_DUPLICATE, CustomData_get_layer(&dm->polyData, CD_MTEXPOLY), s->totpoly);

	return s;
}

/* modified from BKE_mesh_center_median */
bool BKE_fracture_shard_center_median(Shard *shard, float cent[3])
{
	int i = shard->totvert;
	MVert *mvert;
	zero_v3(cent);
	for (mvert = shard->mvert; i--; mvert++) {
		add_v3_v3(cent, mvert->co);
	}
	/* otherwise we get NAN for 0 verts */
	if (shard->totvert) {
		mul_v3_fl(cent, 1.0f / (float)shard->totvert);
	}

	return (shard->totvert != 0);
}

/* modified from BKE_mesh_center_centroid */
bool BKE_fracture_shard_center_centroid(Shard *shard, float cent[3])
{
	int i = shard->totpoly;
	MPoly *mpoly;
	float poly_area;
	float total_area = 0.0f;
	float poly_cent[3];

	zero_v3(cent);

	/* calculate a weighted average of polygon centroids */
	for (mpoly = shard->mpoly; i--; mpoly++) {
		BKE_mesh_calc_poly_center(mpoly, shard->mloop + mpoly->loopstart, shard->mvert, poly_cent);
		poly_area = BKE_mesh_calc_poly_area(mpoly, shard->mloop + mpoly->loopstart, shard->mvert);
		madd_v3_v3fl(cent, poly_cent, poly_area);
		total_area += poly_area;
	}
	/* otherwise we get NAN for 0 polys */
	if (shard->totpoly) {
		mul_v3_fl(cent, 1.0f / total_area);
	}

	/* zero area faces cause this, fallback to median */
	if (UNLIKELY(!is_finite_v3(cent))) {
		return BKE_fracture_shard_center_median(shard, cent);
	}
	copy_v3_v3(shard->centroid, cent);

	return (shard->totpoly != 0);
}

void BKE_shard_free(Shard *s, bool doCustomData)
{
	if (s->mvert) {
		MEM_freeN(s->mvert);
	}
	if (s->mloop) {
		MEM_freeN(s->mloop);
	}
	if (s->mpoly) {
		MEM_freeN(s->mpoly);
	}
	if (s->neighbor_ids) {
		MEM_freeN(s->neighbor_ids);
	}
	if (s->cluster_colors) {
		MEM_freeN(s->cluster_colors);
	}

	if (doCustomData) {
		CustomData_free(&s->vertData, s->totvert);
		CustomData_free(&s->loopData, s->totloop);
		CustomData_free(&s->polyData, s->totpoly);
	}

	MEM_freeN(s);
}

float BKE_shard_calc_minmax(Shard *shard)
{
	float min[3], max[3], diff[3];
	int i;
	
	INIT_MINMAX(min, max);
	for (i = 0; i < shard->totvert; i++) {
		minmax_v3v3_v3(min, max, shard->mvert[i].co);
	}
	
	copy_v3_v3(shard->min, min);
	copy_v3_v3(shard->max, max);

	sub_v3_v3v3(diff, max, min);
	return len_v3(diff);
}


/*access shard directly by index / id*/
Shard *BKE_shard_by_id(FracMesh *mesh, ShardID id, DerivedMesh *dm) {
	if (/*(id < mesh->shard_count) && */(id >= 0)) {
		//return mesh->shard_map[id];
		//return (Shard *)BLI_findlink(&mesh->shard_map, id);
		Shard *s = mesh->shard_map.first;
		while (s)
		{
			if (s->shard_id == id)
			{
				return s;
			}
			s = s->next;
		}

		return NULL;
	}
	else if (id == -1 && dm != NULL)
	{
		/* create temporary shard covering the entire mesh */
		Shard *s = BKE_create_fracture_shard(dm->getVertArray(dm), dm->getPolyArray(dm), dm->getLoopArray(dm),
		                                     dm->numVertData, dm->numPolyData, dm->numLoopData, true);
		s = BKE_custom_data_to_shard(s, dm);
		s->flag = SHARD_INTACT;
		s->shard_id = -2;
		return s;
	}
	
	return NULL;
}

void BKE_get_shard_minmax(FracMesh *mesh, ShardID id, float min_r[3], float max_r[3], DerivedMesh *dm)
{
	Shard *shard = BKE_shard_by_id(mesh, id, dm);
	if (shard != NULL) {
		BKE_shard_calc_minmax(shard);
		copy_v3_v3(min_r, shard->min);
		copy_v3_v3(max_r, shard->max);

		if (shard->shard_id == -2)
		{
			BKE_shard_free(shard, true);
		}
	}
	else
	{
		printf("OOOOPS: %d is NULL!!!\n", id);
	}
}

Shard *BKE_create_fracture_shard(MVert *mvert, MPoly *mpoly, MLoop *mloop, int totvert, int totpoly, int totloop, bool copy)
{
	Shard *shard = MEM_mallocN(sizeof(Shard), __func__);
	shard->totvert = totvert;
	shard->totpoly = totpoly;
	shard->totloop = totloop;
	shard->cluster_colors = NULL;
	shard->neighbor_ids = NULL;
	shard->neighbor_count = 0;
	
	if (copy) {
		shard->mvert = MEM_mallocN(sizeof(MVert) * totvert, "shard vertices");
		shard->mpoly = MEM_mallocN(sizeof(MPoly) * totpoly, "shard polys");
		shard->mloop = MEM_mallocN(sizeof(MLoop) * totloop, "shard loops");
		memcpy(shard->mvert, mvert, sizeof(MVert) * totvert);
		memcpy(shard->mpoly, mpoly, sizeof(MPoly) * totpoly);
		memcpy(shard->mloop, mloop, sizeof(MLoop) * totloop);
	}
	else {
		shard->mvert = mvert;
		shard->mpoly = mpoly;
		shard->mloop = mloop;
	}

	shard->shard_id = -1;
	shard->flag = SHARD_INTACT;
	BKE_shard_calc_minmax(shard);

	BKE_fracture_shard_center_centroid(shard, shard->centroid);
	copy_v3_v3(shard->raw_centroid, shard->centroid);
	zero_v3(shard->impact_loc);
	shard->impact_size[0] = 1.0f;
	shard->impact_size[1] = 1.0f;
	shard->impact_size[2] = 1.0f;

	return shard;
}

FracMesh *BKE_create_fracture_container(void)
{
	FracMesh *fmesh;
	
	fmesh = MEM_mallocN(sizeof(FracMesh), __func__);
	fmesh->shard_map.first = NULL;
	fmesh->shard_map.last = NULL;
	fmesh->shard_count = 0;
	fmesh->cancel = 0;
	fmesh->running = 0;
	fmesh->progress_counter = 0;
	fmesh->last_shards = NULL;
	fmesh->last_shard_tree = NULL;
	fmesh->last_expected_shards = 0;
	
	return fmesh;
}

static void handle_fast_bisect(FracMesh *fm, int expected_shards, int algorithm, BMesh** bm_parent, float obmat[4][4],
                               float centroid[3], short inner_material_index, int parent_id, Shard **tempshards, Shard ***tempresults)
{
	int i = 0;

	for (i = 0; i < expected_shards; i++) {
		Shard *s = NULL;
		Shard *s2 = NULL;
		Shard *t;
		int index = 0;

		if (fm->cancel == 1) {
			break;
		}

		printf("Processing shard: %d\n", i);
		t = tempshards[i];

		if (t != NULL) {
			t->parent_id = parent_id;
			t->flag = SHARD_INTACT;
		}

		if (t == NULL || t->totvert == 0 || t->totloop == 0 || t->totpoly == 0) {
			/* invalid shard, stop parsing*/
			break;
		}

		index = (int)(BLI_frand() * (t->totpoly - 1));
		if (index == 0) {
			index = 1;
		}

		printf("Bisecting cell %d...\n", i);
		printf("Bisecting cell %d...\n", i + 1);

		s = BKE_fracture_shard_bisect(*bm_parent, t, obmat, algorithm == MOD_FRACTURE_BISECT_FAST_FILL, false, true, index, centroid, inner_material_index);
		s2 = BKE_fracture_shard_bisect(*bm_parent, t, obmat, algorithm == MOD_FRACTURE_BISECT_FAST_FILL, true, false, index, centroid, inner_material_index);

		if (s != NULL && s2 != NULL && tempresults != NULL) {
			int j = 0;

			fm->progress_counter++;

			s->parent_id = parent_id;
			s->flag = SHARD_INTACT;

			s2->parent_id = parent_id;
			s2->flag = SHARD_INTACT;

			if (*bm_parent != NULL) {
				BM_mesh_free(*bm_parent);
				*bm_parent = NULL;
			}

			(*tempresults)[i] = s;
			(*tempresults)[i + 1] = s2;

			BLI_qsort_r(*tempresults, i + 1, sizeof(Shard *), shard_sortsize, &i);

			while ((*tempresults)[j] == NULL && j < (i + 1)) {
				/* ignore invalid shards */
				j++;
			}

			/* continue splitting if not all expected shards exist yet */
			if ((i + 2) < expected_shards) {
				*bm_parent = shard_to_bmesh((*tempresults)[j]);
				copy_v3_v3(centroid, (*tempresults)[j]->centroid);

				BKE_shard_free((*tempresults)[j], true);
				(*tempresults)[j] = NULL;
			}
			i++;
		}
	}
}

static void handle_boolean_fractal(Shard* s, Shard* t, int expected_shards, DerivedMesh* dm_parent, Object *obj, short inner_material_index,
                                   int num_cuts, float fractal, int num_levels, bool smooth,int parent_id, int* i, Shard ***tempresults,
                                   DerivedMesh **dm_p)
{
	/* physics shard and fractalized shard, so we need to booleanize twice */
	/* and we need both halves, so twice again */
	Shard *s2 = NULL;
	int index = 0;
	int max_retries = 20;

	/*continue with "halves", randomly*/
	if ((*i) == 0) {
		*dm_p = dm_parent;
	}

	while (s == NULL || s2 == NULL) {

		float radius;
		float size[3];
		float eul[3];
		float loc[3];
		float one[3] = {1.0f, 1.0f, 1.0f};
		float matrix[4][4];

		/*make a plane as cutter*/
		BKE_object_dimensions_get(obj, size);
		radius = MAX3(size[0], size[1], size[2]);

		loc[0] = (BLI_frand() - 0.5f) * size[0];
		loc[1] = (BLI_frand() - 0.5f) * size[1];
		loc[2] = (BLI_frand() - 0.5f) * size[2];

		eul[0] = BLI_frand() * M_PI;
		eul[1] = BLI_frand() * M_PI;
		eul[2] = BLI_frand() * M_PI;

		//printf("(%f %f %f) (%f %f %f) \n", loc[0], loc[1], loc[2], eul[0], eul[1], eul[2]);

		loc_eul_size_to_mat4(matrix, loc, eul, one);

		/*visual shards next, fractalized cuts */
		s = BKE_fracture_shard_boolean(obj, *dm_p, t, inner_material_index, num_cuts,fractal, &s2, matrix, radius, smooth, num_levels);

		if (index < max_retries)
		{
			printf("Retrying...%d\n", index);
			index++;
		}
		else if (s == NULL || s2 == NULL)
		{
			(*i)++;
			break;
		}
	}

	if ((s != NULL) && (s2 != NULL)) {
		int j = 0;

		s->parent_id = parent_id;
		s->flag = SHARD_INTACT;
		(*tempresults)[(*i)+1] = s;

		s2->parent_id = parent_id;
		s2->flag = SHARD_INTACT;
		(*tempresults)[*i] = s2;

		BLI_qsort_r(*tempresults, (*i) + 1, sizeof(Shard *), shard_sortsize, i);
		while ((*tempresults)[j] == NULL && j < ((*i) + 1)) {
			/* ignore invalid shards */
			j++;
		}

		/* continue splitting if not all expected shards exist yet */
		if (((*i) + 2) < expected_shards) {

			Shard *p = (*tempresults)[j];

			if (*dm_p != dm_parent && *dm_p != NULL) {
				(*dm_p)->needsFree = 1;
				(*dm_p)->release(*dm_p);
			}

			*dm_p = BKE_shard_create_dm(p, true);

			BKE_shard_free((*tempresults)[j], true);
			(*tempresults)[j] = NULL;
		}
		(*i)++; //XXX remember to "double" the shard amount....
	}
}

static bool handle_boolean_bisect(FracMesh *fm, Object *obj, int expected_shards, int algorithm, int parent_id, Shard **tempshards,
                                  DerivedMesh *dm_parent, BMesh* bm_parent, float obmat[4][4], short inner_material_index, int num_cuts,
                                  int num_levels, float fractal, int *i, bool smooth, Shard*** tempresults, DerivedMesh **dm_p)
{
	Shard *s = NULL, *t = NULL;
	if (fm->cancel == 1)
		return true;

	t = tempshards[*i];

	if (t != NULL) {
		t->parent_id = parent_id;
		t->flag = SHARD_INTACT;
	}

	if (t == NULL || t->totvert == 0 || t->totloop == 0 || t->totpoly == 0) {
		/* invalid shard, stop parsing */
		return true;
	}

	printf("Processing shard: %d\n", *i);

	/* XXX TODO, need object for material as well, or atleast a material index... */
	if (algorithm == MOD_FRACTURE_BOOLEAN) {
		s = BKE_fracture_shard_boolean(obj, dm_parent, t, inner_material_index, 0, 0.0f, NULL, NULL, 0.0f, false, 0);
	}
	else if (algorithm == MOD_FRACTURE_BOOLEAN_FRACTAL) {
		handle_boolean_fractal(s, t, expected_shards, dm_parent, obj, inner_material_index, num_cuts, fractal,
		                       num_levels, smooth, parent_id, i, tempresults, dm_p);
	}
	else if (algorithm == MOD_FRACTURE_BISECT || algorithm == MOD_FRACTURE_BISECT_FILL) {
		float co[3] = {0, 0, 0};
		printf("Bisecting cell %d...\n", *i);
		s = BKE_fracture_shard_bisect(bm_parent, t, obmat, algorithm == MOD_FRACTURE_BISECT_FILL, false, true, 0, co, inner_material_index);
	}
	else {
		/* do not fracture case */
		s = t;
	}

	if ((s != NULL) && (algorithm != MOD_FRACTURE_BOOLEAN_FRACTAL)) {
		s->parent_id = parent_id;
		s->flag = SHARD_INTACT;

		(*tempresults)[*i] = s;
	}

	fm->progress_counter++;
	return false;
}

static void do_prepare_cells(FracMesh *fm, cell *cells, int expected_shards, int algorithm, Shard *p, float (*centroid)[3],
                             DerivedMesh **dm_parent, BMesh** bm_parent, Shard ***tempshards, Shard ***tempresults)
{
	int i;
	Shard *s = NULL;
	int *skipmap = MEM_callocN(sizeof(int) * expected_shards, "skipmap");
	int *deletemap = MEM_callocN(sizeof(int) * fm->shard_count, "deletemap");

	if ((algorithm == MOD_FRACTURE_BOOLEAN) || (algorithm == MOD_FRACTURE_BOOLEAN_FRACTAL)) {
		MPoly *mpoly, *mp;
		int totpoly, i;

		*dm_parent = BKE_shard_create_dm(p, true);
		mpoly = (*dm_parent)->getPolyArray(*dm_parent);
		totpoly = (*dm_parent)->getNumPolys(*dm_parent);
		for (i = 0, mp = mpoly; i < totpoly; i++, mp++) {
			mp->flag &= ~ME_FACE_SEL;
		}
	}
	else if (algorithm == MOD_FRACTURE_BISECT || algorithm == MOD_FRACTURE_BISECT_FILL ||
	         algorithm == MOD_FRACTURE_BISECT_FAST || algorithm == MOD_FRACTURE_BISECT_FAST_FILL)
	{
		*bm_parent = shard_to_bmesh(p);
		copy_v3_v3(*centroid, p->centroid);
	}

	if (algorithm == MOD_FRACTURE_BISECT_FAST ||
	    algorithm == MOD_FRACTURE_BISECT_FAST_FILL ||
	    algorithm == MOD_FRACTURE_BOOLEAN_FRACTAL)
	{
		fill_vn_i(deletemap, fm->shard_count, 1);
	}

	if (fm->last_shard_tree)
	{
		if (expected_shards <= fm->last_expected_shards)
		{
			fill_vn_i(deletemap, fm->shard_count, 1);
		}
		else
		{
			fill_vn_i(skipmap, expected_shards, 1);
		}

		for (i = 0; i < expected_shards; i++)
		{
			KDTreeNearest n;
			int l, j;
			float max = 0;
			for (l = 0; l < cells[i].totpoly; l++)
			{
				int index = cells[i].neighbors[l];
				if (index > -1)
				{
					float dist = len_squared_v3v3(cells[index].centroid, cells[i].centroid);
					if (dist > max)
					{
						max = dist;
					}
				}
			}

			j = BLI_kdtree_find_nearest(fm->last_shard_tree, cells[i].centroid, &n);
			if (j > -1)
			{
				Shard *t = fm->last_shards[j];
				float dist = len_squared_v3v3(n.co, cells[i].centroid);
				if (t != NULL && dist < max)
				{
					if (dist < 0.00001) {
						if (fabsf(cells[i].volume - t->raw_volume) < 0.00001) {
							//printf("Tagging skip: %d\n", i);
							skipmap[i] = true;
							deletemap[j] = false;
						}
						else
						{
							deletemap[j] = true;
							skipmap[i] = false;
						}
					}
					else
					{
						skipmap[i] = false;
						deletemap[j] = true;
					}
				}
			}
		}
	}

	//skipping /deletion pass
	for (i = 0; i < expected_shards; i++)
	{
		if (fm->cancel == 1) {
			break;
		}

		if (skipmap[i])
		{
			printf("Skipping shard: %d\n", i);
			(*tempshards)[i] = NULL;
			(*tempresults)[i] = NULL;
		}
		else
		{
			printf("Parsing shard: %d\n", i);
			s = parse_cell(cells[i]);
			(*tempshards)[i] = s;
		}

		(*tempresults)[i] = NULL;
		fm->progress_counter++;
	}

	for (i = 0; i < fm->shard_count; i++)
	{
		if (deletemap[i])
		{
			Shard *t = fm->last_shards[i];
			BLI_remlink_safe(&fm->shard_map, t);
			BKE_shard_free(t, true);
			fm->last_shards[i] = NULL;

			printf("Deleting shard: %d\n", i);
		}
	}

	fm->last_expected_shards = expected_shards;

	MEM_freeN(skipmap);
	MEM_freeN(deletemap);
}


/* parse the voro++ cell data */
static void parse_cells(cell *cells, int expected_shards, ShardID parent_id, FracMesh *fm, int algorithm, Object *obj, DerivedMesh *dm, short inner_material_index, float mat[4][4], int num_cuts, float fractal, bool smooth, int num_levels, int mode, bool reset)
{
	/*Parse voronoi raw data*/
	int i = 0, j = 0, count = 0;
	Shard *p = BKE_shard_by_id(fm, parent_id, dm); // *t;
	float obmat[4][4]; /* use unit matrix for now */
	float centroid[3], pcentroid[3] = {0,0,0};
	BMesh *bm_parent = NULL;
	DerivedMesh *dm_parent = NULL;
	DerivedMesh *dm_p = NULL;
	Shard **tempshards;
	Shard **tempresults;
	bool do_tree = (algorithm != MOD_FRACTURE_BISECT_FAST &&
					algorithm != MOD_FRACTURE_BISECT_FAST_FILL &&
					algorithm != MOD_FRACTURE_BOOLEAN_FRACTAL);

	if (p == NULL)
	{
		if (fm->last_shard_tree)
		{
			BLI_kdtree_free(fm->last_shard_tree);
			fm->last_shard_tree = NULL;
		}

		if (fm->last_shards)
		{
			MEM_freeN(fm->last_shards);
			fm->last_shards = NULL;
		}

		return;
	}

	if (reset)
	{
		while (fm->shard_map.first)
		{
			Shard *t = fm->shard_map.first;
			BLI_remlink_safe(&fm->shard_map, t);
			printf("Resetting shard: %d\n", t->shard_id);
			BKE_shard_free(t, true);
		}
	}

	if (mode == MOD_FRACTURE_PREFRACTURED && !reset)
	{
		//rebuild tree
		if (!fm->last_shard_tree && (fm->shard_count > 0) && mode == MOD_FRACTURE_PREFRACTURED)
		{
			Shard *t;
			int i = 0;
			count = BLI_listbase_count(&fm->shard_map);
			fm->shard_count = count;
			if (do_tree)
			{
				fm->last_shard_tree = BLI_kdtree_new(fm->shard_count);
			}

			fm->last_shards = MEM_callocN(sizeof(Shard*) * fm->shard_count, "last_shards");

			//fill tree from current shardmap
			for (t = fm->shard_map.first; t; t = t->next)
			{
				t->flag &=~ (SHARD_SKIP | SHARD_DELETE);

				if (do_tree)
				{
					BLI_kdtree_insert(fm->last_shard_tree, i, t->raw_centroid);
				}
				fm->last_shards[i] = t;
				i++;
			}

			if (do_tree)
			{
				BLI_kdtree_balance(fm->last_shard_tree);
			}
		}
	}
	else
	{
		fm->last_shard_tree = NULL;
		fm->last_shards = NULL;
	}

	tempshards = MEM_callocN(sizeof(Shard *) * expected_shards, "tempshards");
	tempresults = MEM_callocN(sizeof(Shard *) * expected_shards, "tempresults");

	p->flag = 0;
	p->flag |= SHARD_FRACTURED;

	if (mode == MOD_FRACTURE_DYNAMIC)
	{
		copy_v3_v3(pcentroid, p->centroid);
		parent_id = p->shard_id;
		//remove parent shard from map as well
		BLI_remlink(&fm->shard_map, p);
		fm->shard_count--;
		p->shard_id = -2;
	}

	unit_m4(obmat);

	do_prepare_cells(fm, cells, expected_shards, algorithm, p, &centroid, &dm_parent, &bm_parent, &tempshards, &tempresults);

	if (fm->last_shard_tree)
	{
		BLI_kdtree_free(fm->last_shard_tree);
		fm->last_shard_tree = NULL;
	}

	if (fm->last_shards)
	{
		MEM_freeN(fm->last_shards);
		fm->last_shards = NULL;
	}

	if (algorithm != MOD_FRACTURE_BISECT_FAST && algorithm != MOD_FRACTURE_BISECT_FAST_FILL) {
		for (i = 0; i < expected_shards; i++) {
			bool stop = handle_boolean_bisect(fm, obj, expected_shards, algorithm, parent_id, tempshards, dm_parent,
			                      bm_parent, obmat, inner_material_index, num_cuts, num_levels, fractal,
			                      &i, smooth, &tempresults, &dm_p);
			//if (stop)
			//	break;
		}
	}
	else {

		if (expected_shards == 1)
		{
			/* do not fracture case */
			tempresults[0] = p;
			p->shard_id = -1;
		}
		else
		{
			handle_fast_bisect(fm, expected_shards, algorithm, &bm_parent, obmat, centroid, inner_material_index, parent_id,
			                   tempshards, &tempresults);
		}
	}

	if (bm_parent != NULL) {
		BM_mesh_free(bm_parent);
		bm_parent = NULL;
	}

	if (dm_parent != NULL) {
		dm_parent->needsFree = 1;
		dm_parent->release(dm_parent);
		dm_parent = NULL;
	}

	/*only used with fractal, and is doubly freed in case of 1 shard (doubled) */
	if (dm_p != NULL && expected_shards > 2) {
		dm_p->needsFree = 1;
		dm_p->release(dm_p);
		dm_p = NULL;
	}


	if (p->shard_id == -2)
	{
		BKE_shard_free(p, true);
	}

	fm->shard_count = 0; /* may be not matching with expected shards, so reset... did increment this for
	                      *progressbar only */

	//keep empty ids... need to catch this later
	if (mode == MOD_FRACTURE_DYNAMIC)
	{
		j = 1;
		if (fm->shard_map.last)
		{
			j += ((Shard*)(fm->shard_map.last))->shard_id;
		}

	}
	else
	{
		j = 0;
	}

	for (i = 0; i < expected_shards; i++) {
		Shard *s = tempresults[i];
		Shard *t = tempshards[i];

		if (s != NULL) {
			add_shard(fm, s, mat);
			s->shard_id += j+1;
			s->parent_id = parent_id;
			//printf("ADDED: %d %d %d\n", i, j, s->shard_id);
			if (parent_id > -1)
			{
				int i = 0;
				MVert *v;

				sub_v3_v3(s->centroid, pcentroid);
				for (i = 0, v = s->mvert; i < s->totvert; i++, v++)
				{
					sub_v3_v3(v->co, pcentroid);
				}
			}
		}

		if (t != NULL) {
			BKE_shard_free(t, false);
		}
	}

	if (fm->shard_count == 0)
	{
		//might happen if all has been skipped, but this distracts the halving method (thinks shardmap is empty)
		//so better correct this here
		fm->shard_count = BLI_listbase_count(&fm->shard_map);
	}

	MEM_freeN(tempshards);
	MEM_freeN(tempresults);
}

static Shard *parse_cell(cell c)
{
	Shard *s;
	MVert *mvert = NULL;
	MPoly *mpoly = NULL;
	MLoop *mloop = NULL;
	int *neighbors = NULL;
	int totpoly = 0, totloop = 0, totvert = 0;
	float centr[3];

	totvert = c.totvert;
	if (totvert > 0) {
		mvert = MEM_callocN(sizeof(MVert) * totvert, __func__);
		parse_cell_verts(c, mvert, totvert);
	}

	totpoly = c.totpoly;
	if (totpoly > 0) {
		mpoly = MEM_callocN(sizeof(MPoly) * totpoly, __func__);
		parse_cell_polys(c, mpoly, totpoly, &totloop);
	}
	else
		totloop = 0;

	if (totloop > 0) {
		mloop = MEM_callocN(sizeof(MLoop) * totloop, __func__);
		parse_cell_loops(c, mloop, totloop, mpoly, totpoly);
	}

	if (totpoly > 0) {
		neighbors = MEM_callocN(sizeof(int) * totpoly, __func__);
		parse_cell_neighbors(c, neighbors, totpoly);
	}

	copy_v3_v3(centr, c.centroid);

	s = BKE_create_fracture_shard(mvert, mpoly, mloop, totvert, totpoly, totloop, false);

	s->flag &= ~(SHARD_SKIP | SHARD_DELETE);
	s->neighbor_ids = neighbors;
	s->neighbor_count = totpoly;
	copy_v3_v3(s->centroid, centr);
	copy_v3_v3(s->raw_centroid, centr);
	s->raw_volume = c.volume;

	return s;
}

static void parse_cell_verts(cell c, MVert *mvert, int totvert)
{
	int i;

	for (i = 0; i < totvert; i++) {
		float *co = mvert[i].co;
		copy_v3_v3(co, c.verts[i]);
	}
}

static void parse_cell_polys(cell c, MPoly *mpoly, int totpoly, int *r_totloop)
{
	int i;
	int totloop = 0;

	for (i = 0; i < totpoly; ++i) {
		int numloop;

		numloop = c.poly_totvert[i];

		mpoly[i].loopstart = totloop;
		mpoly[i].totloop = numloop;

		totloop += numloop;
	}

	*r_totloop = totloop;
}

static void parse_cell_loops(cell c, MLoop *mloop, int UNUSED(totloop), MPoly *mpoly, int totpoly)
{
	int i, k;

	for (i = 0; i < totpoly; ++i) {
		int loopstart = mpoly[i].loopstart;
		int numloop = mpoly[i].totloop;

		for (k = 0; k < numloop; ++k) {
			int index;

			index = c.poly_indices[i][k];

			/* note: invert vertex order here,
			 * otherwise normals are pointing inward
			 */
			mloop[loopstart + (numloop - 1) - k].v = index;
		}
	}
}

static void parse_cell_neighbors(cell c, int *neighbors, int totpoly)
{
	int i;

	for (i = 0; i < totpoly; i++) {
		int n;
		n = c.neighbors[i];
		neighbors[i] = n;
	}
}

static void stroke_to_faces(FractureModifierData *fmd, BMesh** bm, bGPDstroke *gps, int inner_material_index)
{
	BMVert *lastv1 = NULL;
	BMVert *lastv2 = NULL;
	int p = 0;
	float thresh = (float)fmd->fracture->grease_decimate / 100.0f;
	float half[3] = {0, 0, 1};

	for (p = 0; p < gps->totpoints; p++) {

		if ((BLI_frand() < thresh) || (p == 0) || (p == gps->totpoints-1)) {
			BMVert *v1, *v2;
			float point[3] = {0, 0, 0};

			point[0] = gps->points[p].x;
			point[1] = gps->points[p].y;
			point[2] = gps->points[p].z;

			v1 = BM_vert_create(*bm, point, NULL, 0);

			if (lastv1)
			{
				BMFace* f;
				float nvec[3], co1[3], co2[3];

				/*also "extrude" this along the normal, no...use global axises instead*/
				if (fmd->fracture->cutter_axis == MOD_FRACTURE_CUTTER_X)
				{
					nvec[0] = 1.0f;
					nvec[1] = 0.0f;
					nvec[2] = 0.0f;
				}

				if (fmd->fracture->cutter_axis == MOD_FRACTURE_CUTTER_Y)
				{
					nvec[0] = 0.0f;
					nvec[1] = 1.0f;
					nvec[2] = 0.0f;
				}

				if (fmd->fracture->cutter_axis == MOD_FRACTURE_CUTTER_Z)
				{
					nvec[0] = 0.0f;
					nvec[1] = 0.0f;
					nvec[2] = 1.0f;
				}

				mul_v3_fl(nvec, fmd->fracture->grease_offset);
				mul_v3_v3fl(half, nvec, 0.5f);

				add_v3_v3v3(co1, v1->co, nvec);
				v2 = BM_vert_create(*bm, co1, NULL, 0);

				if (!lastv2)
				{
					add_v3_v3v3(co2, lastv1->co, nvec);
					lastv2 = BM_vert_create(*bm, co2, NULL, 0);
				}

				f = BM_face_create_quad_tri(*bm, lastv1, v1, v2, lastv2, NULL, 0);
				f->mat_nr = inner_material_index;
				lastv2 = v2;
			}

			lastv1 = v1;
		}
	}

	{
		/* move the stroke mesh a bit out, half of offset */
		BMIter iter;
		BMVert *v;

		BM_ITER_MESH(v, &iter, *bm, BM_VERTS_OF_MESH)
		{
			sub_v3_v3(v->co, half);
		}
	}
}

static void do_intersect(FractureModifierData *fmd, Object* ob, Shard *t, short inner_mat_index,
                         bool is_zero, float mat[4][4], int **shard_counts, int* count,
                         int k, DerivedMesh **dm_parent, bool keep_other_shard)
{
	/*just keep appending items at the end here */
	MPoly *mpoly, *mp;
	int totpoly;
	Shard *parent = NULL;
	Shard *s = NULL, *s2 = NULL;
	int shards = 0, j = 0;

	if (is_zero == false) {
		parent = BLI_findlink(&fmd->fracture->frac_mesh->shard_map, k);
		*dm_parent = BKE_shard_create_dm(parent, true);
	}

	mpoly = (*dm_parent)->getPolyArray(*dm_parent);
	totpoly = (*dm_parent)->getNumPolys(*dm_parent);

	for (j = 0, mp = mpoly; j < totpoly; j++, mp++) {
		mp->flag &= ~ME_FACE_SEL;
	}

	if (keep_other_shard)
	{
		s = BKE_fracture_shard_boolean(ob, *dm_parent, t, inner_mat_index, 0, 0.0f, &s2, NULL, 0.0f, false, 0);
	}
	else
	{
		s = BKE_fracture_shard_boolean(ob, *dm_parent, t, inner_mat_index, 0, 0.0f, NULL, NULL, 0.0f, false, 0);
	}

	//printf("Fractured: %d\n", k);

	if (s != NULL) {
		add_shard(fmd->fracture->frac_mesh, s, mat);
		shards++;
		s = NULL;
	}

	if (s2 != NULL) {
		add_shard(fmd->fracture->frac_mesh, s2, mat);
		shards++;
		s2 = NULL;
	}

	if ((is_zero && ob->derivedFinal == NULL) || !is_zero) {
		if (is_zero) {
			*count = 0;
		}

		(*dm_parent)->needsFree = 1;
		(*dm_parent)->release(*dm_parent);
		*dm_parent = NULL;
	}

	if (is_zero) {
		shards = 0;
	}

	(*shard_counts)[k] = shards;
	//printf("k, shards: %d %d \n", k, shards);
	shards = 0;
}



static void intersect_shards_by_dm(FractureModifierData *fmd, DerivedMesh *d, Object *ob, Object *ob2, short inner_mat_index, float mat[4][4],
                                   bool keep_other_shard)
{
	Shard *t = NULL;
	int i = 0, count = 0, k = 0;
	float imat[4][4];
	int* shard_counts = NULL;
	bool is_zero = false;
	MVert *mv;
	DerivedMesh *dm_parent = NULL;

	t = BKE_create_fracture_shard(d->getVertArray(d), d->getPolyArray(d), d->getLoopArray(d),
	                              d->getNumVerts(d), d->getNumPolys(d), d->getNumLoops(d), true);
	t = BKE_custom_data_to_shard(t, d);


	invert_m4_m4(imat, ob->obmat);
	for (i = 0, mv = t->mvert; i < t->totvert; mv++, i++){
		if (ob2)
			mul_m4_v3(ob2->obmat, mv->co);
		mul_m4_v3(imat, mv->co);
	}

	count = fmd->fracture->frac_mesh->shard_count;

	/*TODO, pass modifier mesh here !!! */
	if (count == 0 && keep_other_shard) {

		if (ob->derivedFinal != NULL) {
			dm_parent = CDDM_copy(ob->derivedFinal);
		}

		if (dm_parent == NULL) {
			dm_parent = CDDM_from_mesh(ob->data);
		}

		count = 1;
		is_zero = true;
	}

	shard_counts = MEM_mallocN(sizeof(int) * count, "shard_counts");

	for (k = 0; k < count; k++) {
		do_intersect(fmd, ob, t, inner_mat_index, is_zero, mat, &shard_counts, &count, k, &dm_parent, keep_other_shard);
	}

	for (k = 0; k < count; k++)
	{
		int cnt = shard_counts[k];

		if (cnt > 0)
		{
			if (keep_other_shard)
			{
				/*clean up old entries here to avoid unnecessary shards*/
				Shard *first = fmd->fracture->frac_mesh->shard_map.first;
				BLI_remlink_safe(&fmd->fracture->frac_mesh->shard_map,first);
				BKE_shard_free(first, true);
				first = NULL;
			}

			/* keep asynchronous by intent, to keep track of original shard count */
			fmd->fracture->frac_mesh->shard_count--;
		}
	}

	MEM_freeN(shard_counts);
	shard_counts = NULL;

	BKE_shard_free(t, true);
}

void BKE_fracture_shard_by_greasepencil(FractureModifierData *fmd, Object *obj, short inner_material_index, float mat[4][4])
{
	bGPDlayer *gpl;
	bGPDframe *gpf;
	bGPDstroke *gps;

	if ((obj->gpd) && (obj->gpd->layers.first)) {

		float imat[4][4];
		invert_m4_m4(imat, mat);
		for (gpl = obj->gpd->layers.first; gpl; gpl = gpl->next) {
			for (gpf = gpl->frames.first; gpf; gpf = gpf->next) {
				for (gps = gpf->strokes.first; gps; gps = gps->next) {
					BMesh *bm = BM_mesh_create(&bm_mesh_allocsize_default);
					DerivedMesh *dm = NULL;


					/*create stroke mesh */
					stroke_to_faces(fmd, &bm, gps, inner_material_index);
					dm = CDDM_from_bmesh(bm, true);
#if 0
					{
						/*create debug mesh*/
						Object* o;
						o = BKE_object_add(G.main, fmd->modifier.scene, OB_MESH);
						BM_mesh_bm_to_me(bm, o->data, true);
					}
#endif

					BM_mesh_free(bm);

					/*do intersection*/
					intersect_shards_by_dm(fmd, dm, obj, NULL, inner_material_index, mat, true);

					dm->needsFree = 1;
					dm->release(dm);
					dm = NULL;
				}
			}
		}
	}
}

void BKE_fracture_shard_by_planes(FractureModifierData *fmd, Object *obj, short inner_material_index, float mat[4][4])
{
	if (fmd->fracture->frac_algorithm == MOD_FRACTURE_BOOLEAN && fmd->fracture->cutter_group != NULL && obj->type == OB_MESH)
	{
		GroupObject* go;
		float imat[4][4];

		invert_m4_m4(imat, obj->obmat);

		for (go = fmd->fracture->cutter_group->gobject.first; go; go = go->next)
		{
			Object* ob = go->ob;

			printf("Cutting with %s ...\n", ob->id.name);
			/*simple case....one cutter object per object*/
			if (ob->type == OB_MESH) {

				FractureModifierData *fmd2 = (FractureModifierData*)modifiers_findByType(ob, eModifierType_Fracture);
				if (fmd2 && BLI_listbase_count(&fmd2->fracture->meshIslands) > 0)
				{
					MeshIsland* mi = NULL;
					int j = 0;
					//another inner loop over fracture settings necessary, TODO
					for (mi = fmd2->fracture->meshIslands.first; mi; mi = mi->next)
					{
						DerivedMesh *dm = CDDM_copy(mi->physics_mesh);
						MVert *mv = dm->getVertArray(dm), *v = NULL;
						int totvert = dm->getNumVerts(dm);
						int i = 0;

						//printf("Cutting with %s, island %d...\n", ob->id.name, j);
						for (i = 0, v = mv; i < totvert; i++, v++)
						{
							add_v3_v3(v->co, mi->centroid);
						}

						intersect_shards_by_dm(fmd, dm, obj, ob, inner_material_index, mat, false);

						dm->needsFree = 1;
						dm->release(dm);
						dm = NULL;
						j++;
					}

					/*now delete first shards, those are the old ones */
					while (fmd->fracture->frac_mesh->shard_count > 0)
					{
						/*clean up old entries here to avoid unnecessary shards*/
						Shard *first = fmd->fracture->frac_mesh->shard_map.first;
						BLI_remlink_safe(&fmd->fracture->frac_mesh->shard_map,first);
						BKE_shard_free(first, true);
						first = NULL;
						fmd->fracture->frac_mesh->shard_count--;
					}

					/* re-synchronize counts, was possibly different before */
					fmd->fracture->frac_mesh->shard_count = BLI_listbase_count(&fmd->fracture->frac_mesh->shard_map);
				}
				else
				{

					DerivedMesh *d;
					d = ob->derivedFinal;
					if (d == NULL) {
						d = CDDM_from_mesh(ob->data);
					}

					intersect_shards_by_dm(fmd, d, obj, ob, inner_material_index, mat, true);

					if (ob->derivedFinal == NULL)
					{	/*was copied before */
						d->needsFree = 1;
						d->release(d);
						d = NULL;
					}
				}
			}
		}
	}
}

void BKE_fracture_shard_by_points(FracMesh *fmesh, ShardID id, FracPointCloud *pointcloud, int algorithm, Object *obj, DerivedMesh *dm, short
                                  inner_material_index, float mat[4][4], int num_cuts, float fractal, bool smooth, int num_levels, int mode, bool reset) {
	int n_size = 8;
	
	Shard *shard;
	
	float min[3], max[3];
	float theta = 0.1f; /* TODO, container enlargement, because boundbox exact container and boolean might create artifacts */
	int p;
	
	container *voro_container;
	particle_order *voro_particle_order;
	cell *voro_cells;

#ifdef USE_DEBUG_TIMER
	double time_start;
#endif
	
	shard = BKE_shard_by_id(fmesh, id, dm);
	if (!shard || shard->flag & SHARD_FRACTURED)
		return;

	printf("Fracturing with %d points...\n", pointcloud->totpoints);
	/* calculate bounding box with theta margin */
	copy_v3_v3(min, shard->min);
	copy_v3_v3(max, shard->max);

	if (shard->shard_id == -2) {
		BKE_shard_free(shard, true);
	}

	add_v3_fl(min, -theta);
	add_v3_fl(max, theta);

	mul_m4_v3(mat, min);
	mul_m4_v3(mat, max);

	
	voro_container = container_new(min[0], max[0], min[1], max[1], min[2], max[2],
	                               n_size, n_size, n_size, false, false, false,
	                               pointcloud->totpoints);
	
	voro_particle_order = particle_order_new();
	for (p = 0; p < pointcloud->totpoints; p++) {
		float *co = pointcloud->points[p].co;
		container_put(voro_container, voro_particle_order, p, co[0], co[1], co[2]);
	}
	


#ifdef USE_DEBUG_TIMER
	time_start = PIL_check_seconds_timer();
#endif

	/* we expect as many raw cells as we have particles */
	voro_cells = cells_new(pointcloud->totpoints);

	/*Compute directly...*/
	container_compute_cells(voro_container, voro_cells);

	/*Evaluate result*/
	parse_cells(voro_cells, pointcloud->totpoints, id, fmesh, algorithm, obj, dm, inner_material_index, mat,
	            num_cuts, fractal, smooth, num_levels, mode, reset);

	/*Free structs in C++ area of memory */
	cells_free(voro_cells, pointcloud->totpoints);
	particle_order_free(voro_particle_order);
	container_free(voro_container);

#ifdef USE_DEBUG_TIMER
	printf("Fracture done, %g\n", PIL_check_seconds_timer() - time_start);
#endif

}

void BKE_fracmesh_free(FracMesh *fm, bool doCustomData)
{
	if (fm == NULL) {
		return;
	}

	while (fm->shard_map.first) {
		Shard* s = (Shard*)fm->shard_map.first;
		BLI_remlink_safe(&fm->shard_map, s);
		BKE_shard_free(s, doCustomData);
	}

	if (fm->last_shard_tree)
	{
		BLI_kdtree_free(fm->last_shard_tree);
		fm->last_shard_tree = NULL;
	}

	if (fm->last_shards)
	{
		MEM_freeN(fm->last_shards);
		fm->last_shards = NULL;
	}
}


static void do_marking(FractureModifierData *fmd, DerivedMesh *result)
{
	MEdge *medge = result->getEdgeArray(result);
	MPoly *mpoly = result->getPolyArray(result), *mp = NULL;
	MLoop *mloop = result->getLoopArray(result);
	MVert *mvert = result->getVertArray(result);
	int totpoly = result->getNumPolys(result);
	int i = 0;
	for (i = 0, mp = mpoly; i < totpoly; i++, mp++)
	{
		if (mp->flag & ME_FACE_SEL)
		{
			int j = 0;
			for (j = 0; j < mp->totloop; j++)
			{
				MLoop ml;
				ml = mloop[mp->loopstart + j];
				medge[ml.e].flag |= ME_SHARP;
				mvert[ml.v].flag |= ME_VERT_TMP_TAG;
			}

			if (fmd->fracture->flag & FM_FLAG_USE_SMOOTH)
				mp->flag |= ME_SMOOTH;
		}
		else
		{
			/*remove verts from unselected faces again*/
			int j = 0;
			for (j = 0; j < mp->totloop; j++)
			{
				MLoop ml;
				ml = mloop[mp->loopstart + j];
				mvert[ml.v].flag &= ~ME_VERT_TMP_TAG;
			}
		}
	}
}

static DerivedMesh* do_create(FractureModifierData *fmd, int num_verts, int num_loops, int num_polys, bool doCustomData)
{
	int shard_count = (fmd->fracture->flag & FM_FLAG_SHARDS_TO_ISLANDS) ? BLI_listbase_count(&fmd->fracture->islandShards) : fmd->fracture->frac_mesh->shard_count;
	ListBase *shardlist;
	Shard *shard;

	int vertstart, polystart, loopstart;

	MVert *mverts;
	MPoly *mpolys;
	MLoop *mloops;

	DerivedMesh *result = CDDM_new(num_verts, 0, 0, num_loops, num_polys);
	mverts = CDDM_get_verts(result);
	mloops = CDDM_get_loops(result);
	mpolys = CDDM_get_polys(result);

	if (doCustomData && shard_count > 0) {
		Shard *s;
		if (fmd->fracture->flag & FM_FLAG_SHARDS_TO_ISLANDS) {
			s = (Shard *)fmd->fracture->islandShards.first;
		}
		else {
			s = (Shard *)fmd->fracture->frac_mesh->shard_map.first;
		}

		CustomData_merge(&s->vertData, &result->vertData, CD_MASK_MDEFORMVERT, CD_CALLOC, num_verts);
		CustomData_merge(&s->polyData, &result->polyData, CD_MASK_MTEXPOLY, CD_CALLOC, num_polys);
		CustomData_merge(&s->loopData, &result->loopData, CD_MASK_MLOOPUV, CD_CALLOC, num_loops);
	}

	vertstart = polystart = loopstart = 0;
	if (fmd->fracture->flag & FM_FLAG_SHARDS_TO_ISLANDS) {
		shardlist = &fmd->fracture->islandShards;
	}
	else {
		shardlist = &fmd->fracture->frac_mesh->shard_map;
	}

	for (shard = shardlist->first; shard; shard = shard->next)
	{
		MPoly *mp;
		MLoop *ml;
		int i;

		memcpy(mverts + vertstart, shard->mvert, shard->totvert * sizeof(MVert));
		memcpy(mpolys + polystart, shard->mpoly, shard->totpoly * sizeof(MPoly));


		for (i = 0, mp = mpolys + polystart; i < shard->totpoly; ++i, ++mp) {
			/* adjust loopstart index */
			mp->loopstart += loopstart;
		}

		memcpy(mloops + loopstart, shard->mloop, shard->totloop * sizeof(MLoop));

		for (i = 0, ml = mloops + loopstart; i < shard->totloop; ++i, ++ml) {
			/* adjust vertex index */
			ml->v += vertstart;
		}

		if (doCustomData) {
			if (shard->totvert > 1) {
				CustomData_copy_data(&shard->vertData, &result->vertData, 0, vertstart, shard->totvert);
			}

			if (shard->totloop > 0) {
				CustomData_copy_data(&shard->loopData, &result->loopData, 0, loopstart, shard->totloop);
			}

			if (shard->totpoly > 0) {
				CustomData_copy_data(&shard->polyData, &result->polyData, 0, polystart, shard->totpoly);
			}
		}

		vertstart += shard->totvert;
		polystart += shard->totpoly;
		loopstart += shard->totloop;
	}

	return result;
}

/* DerivedMesh */
static DerivedMesh *create_dm(FractureModifierData *fmd, bool doCustomData)
{
	Shard *s;
	int num_verts, num_polys, num_loops;
	DerivedMesh *result;
	
	num_verts = num_polys = num_loops = 0;

	if (fmd->fracture->flag & FM_FLAG_SHARDS_TO_ISLANDS) {
		for (s = fmd->fracture->islandShards.first; s; s = s->next) {
			num_verts += s->totvert;
			num_polys += s->totpoly;
			num_loops += s->totloop;
		}
	}
	else {
		for (s = fmd->fracture->frac_mesh->shard_map.first; s; s = s->next) {
			num_verts += s->totvert;
			num_polys += s->totpoly;
			num_loops += s->totloop;
		}
	}
	
	result = do_create(fmd, num_verts, num_loops, num_polys, doCustomData);

	CustomData_free(&result->edgeData, 0);
	CDDM_calc_edges(result);

	do_marking(fmd, result);
	
	result->dirty |= DM_DIRTY_NORMALS;
	CDDM_calc_normals_mapping(result);
	return result;
}

void BKE_fracture_create_dm(FractureModifierData *fmd, bool doCustomData)
{
	DerivedMesh *dm_final = NULL;
	
	if (fmd->fracture->dm) {
		fmd->fracture->dm->needsFree = 1;
		fmd->fracture->dm->release(fmd->fracture->dm);
		fmd->fracture->dm = NULL;
	}
	
	dm_final = create_dm(fmd, doCustomData);
	fmd->fracture->dm = dm_final;
}

DerivedMesh *BKE_shard_create_dm(Shard *s, bool doCustomData)
{
	DerivedMesh *dm;
	MVert *mverts;
	MLoop *mloops;
	MPoly *mpolys;
	
	dm  = CDDM_new(s->totvert, 0, 0, s->totloop, s->totpoly);

	mverts = CDDM_get_verts(dm);
	mloops = CDDM_get_loops(dm);
	mpolys = CDDM_get_polys(dm);

	memcpy(mverts, s->mvert, s->totvert * sizeof(MVert));
	memcpy(mloops, s->mloop, s->totloop * sizeof(MLoop));
	memcpy(mpolys, s->mpoly, s->totpoly * sizeof(MPoly));

	CDDM_calc_edges(dm);

	dm->dirty |= DM_DIRTY_NORMALS;
	CDDM_calc_normals_mapping(dm);

	if (doCustomData) {
		if (s->totvert > 1) {
			CustomData_add_layer(&dm->vertData, CD_MDEFORMVERT, CD_DUPLICATE, CustomData_get_layer(&s->vertData, CD_MDEFORMVERT), s->totvert);
		}
		if (s->totloop > 0) {
			CustomData_add_layer(&dm->loopData, CD_MLOOPUV, CD_DUPLICATE, CustomData_get_layer(&s->loopData, CD_MLOOPUV), s->totloop);
		}
		if (s->totpoly > 0) {
			CustomData_add_layer(&dm->polyData, CD_MTEXPOLY, CD_DUPLICATE, CustomData_get_layer(&s->polyData, CD_MTEXPOLY), s->totpoly);
		}
	}

	return dm;
}

void BKE_get_next_entries(FractureModifierData *fmd)
{
	/*meshislands and shards SHOULD be synchronized !!!!*/
	if (fmd->fracture->current_mi_entry->next != NULL) { // && fmd->current_mi_entry->next->is_new == false) {

		fmd->fracture->current_mi_entry = fmd->fracture->current_mi_entry->next;
		fmd->fracture->current_shard_entry = fmd->fracture->current_shard_entry->next;

		fmd->fracture->meshIslands = fmd->fracture->current_mi_entry->meshIslands;
		fmd->fracture->frac_mesh = fmd->fracture->current_shard_entry->frac_mesh;
		fmd->fracture->visible_mesh_cached = fmd->fracture->current_mi_entry->visible_dm;
	}
}

void BKE_get_prev_entries(FractureModifierData *fmd)
{
	/*meshislands and shards SHOULD be synchronized !!!!*/
	if (fmd->fracture->current_mi_entry->prev != NULL) {

		fmd->fracture->current_mi_entry = fmd->fracture->current_mi_entry->prev;
		fmd->fracture->current_shard_entry = fmd->fracture->current_shard_entry->prev;

		fmd->fracture->meshIslands = fmd->fracture->current_mi_entry->meshIslands;
		fmd->fracture->frac_mesh = fmd->fracture->current_shard_entry->frac_mesh;
		fmd->fracture->visible_mesh_cached = fmd->fracture->current_mi_entry->visible_dm;
	}
}

bool BKE_lookup_mesh_state(FractureModifierData *fmd, int frame, int do_lookup)
{
	bool changed = false;
	bool forward = false;
	bool backward = false;

	backward = ((fmd->last_frame > frame) && fmd->fracture->current_mi_entry && fmd->fracture->current_mi_entry->prev);
	forward = ((fmd->last_frame < frame) && (fmd->fracture->current_mi_entry) && (fmd->fracture->current_mi_entry->next != NULL) &&
	           (fmd->fracture->current_mi_entry->next->is_new == false));

	if (backward)
	{
		if (do_lookup)
		{
			while (fmd->fracture->current_mi_entry && fmd->fracture->current_mi_entry->prev &&
				   frame <= fmd->fracture->current_mi_entry->prev->frame)
			{
				printf("Jumping backward because %d is smaller than %d\n", frame, fmd->fracture->current_mi_entry->prev->frame);
				changed = true;
				BKE_free_constraints(fmd);
				BKE_get_prev_entries(fmd);
			}
		}
	}
	else if (forward)
	{
		if (do_lookup)
		{
			while ((fmd->fracture->current_mi_entry) && (fmd->fracture->current_mi_entry->next != NULL) &&
				   (fmd->fracture->current_mi_entry->next->is_new == false) &&
				   frame > fmd->fracture->current_mi_entry->frame)
			{
				printf("Jumping forward because %d is greater/equal than %d\n", frame, fmd->fracture->current_mi_entry->frame);
				changed = true;
				BKE_free_constraints(fmd);
				BKE_get_next_entries(fmd);
			}
		}
	}

	if (do_lookup)
	{
		return changed;
	}
	else
	{
		if (forward || backward)
		{
			fmd->modifier.scene->rigidbody_world->flag |= RBW_FLAG_REFRESH_MODIFIERS;
			fmd->modifier.scene->rigidbody_world->flag |= RBW_FLAG_OBJECT_CHANGED;
		}

		return forward || backward;
	}
}

void BKE_match_vertex_coords(MeshIsland* mi, MeshIsland *par, Object *ob, int frame, bool is_parent)
{
	float loc[3] = {0.0f, 0.0f, 0.0f};
	float rot[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	int j = 0;
	//float irot[4];
	float centr[3] = {0.0f, 0.0f, 0.0f};

	invert_m4_m4(ob->imat, ob->obmat);

	loc[0] = par->locs[3*frame];
	loc[1] = par->locs[3*frame+1];
	loc[2] = par->locs[3*frame+2];

	rot[0] = par->rots[4*frame];
	rot[1] = par->rots[4*frame+1];
	rot[2] = par->rots[4*frame+2];
	rot[3] = par->rots[4*frame+3];

	mi->locs[0] = loc[0];
	mi->locs[1] = loc[1];
	mi->locs[2] = loc[2];

	mi->rots[0] = rot[0];
	mi->rots[1] = rot[1];
	mi->rots[2] = rot[2];
	mi->rots[3] = rot[3];

	mul_m4_v3(ob->imat, loc);
	//mat4_to_quat(irot, ob->imat);
	//mul_qt_qtqt(rot, rot, irot);

	mul_qt_qtqt(rot, rot, par->rot);

	if (is_parent)
	{
		copy_v3_v3(centr, mi->centroid);
		mul_qt_v3(rot, centr);
		add_v3_v3(centr, loc);
	}
	else
	{
		copy_v3_v3(centr, loc);
	}

	for (j = 0; j < mi->vertex_count; j++)
	{
		float co[3];

		//first add vert to centroid, then rotate
		copy_v3_v3(co, mi->vertices_cached[j]->co);
		sub_v3_v3(co, mi->centroid);
		mul_qt_v3(rot, co);
		add_v3_v3(co, centr);
		copy_v3_v3(mi->vertices_cached[j]->co, co);

		co[0] = mi->vertco[3*j];
		co[1] = mi->vertco[3*j+1];
		co[2] = mi->vertco[3*j+2];

		sub_v3_v3(co, mi->centroid);
		mul_qt_v3(rot, co);
		add_v3_v3(co, centr);

		mi->vertco[3*j]   = co[0];
		mi->vertco[3*j+1] = co[1];
		mi->vertco[3*j+2] = co[2];
	}

	//init rigidbody properly ?
	copy_v3_v3(mi->centroid, centr);
	copy_qt_qt(mi->rot, rot);
}

void BKE_free_constraints(FractureModifierData *fmd)
{
	MeshIsland *mi = NULL;
	RigidBodyShardCon *rbsc = NULL;

	for (mi = fmd->fracture->meshIslands.first; mi; mi = mi->next) {
		if (mi->participating_constraints != NULL) {
			MEM_freeN(mi->participating_constraints);
			mi->participating_constraints = NULL;
			mi->participating_constraint_count = 0;
		}
	}

	while (fmd->constraint->meshConstraints.first) {
		rbsc = fmd->constraint->meshConstraints.first;
		BLI_remlink(&fmd->constraint->meshConstraints, rbsc);
		if (fmd->fracture_mode == MOD_FRACTURE_DYNAMIC)
		{
			BKE_rigidbody_remove_shard_con(fmd->modifier.scene, rbsc);
		}
		MEM_freeN(rbsc);
		rbsc = NULL;
	}

	fmd->constraint->meshConstraints.first = NULL;
	fmd->constraint->meshConstraints.last = NULL;
}

struct ConstraintSetting* BKE_fracture_constraint_setting_new(struct FractureModifierData *fmd, char name[64])
{
	ConstraintSetting* cs = MEM_callocN(sizeof(ConstraintSetting), "constraint_setting_new");
	memcpy(cs->name, name, 64);

	//set useful defaults here...
	BLI_addtail(&fmd->constraint_settings, cs);
	fmd->active_constraint_setting = BLI_findindex(&fmd->constraint_settings, cs);

	return cs;
}

void BKE_fracture_constraint_setting_remove(struct FractureModifierData *fmd, struct ConstraintSetting *setting)
{
	int index = BLI_findindex(&fmd->constraint_settings, setting);
	if (index > 0)
	{
		BLI_remlink(&fmd->constraint_settings, setting);
		fmd->active_constraint_setting = index-1;
	}
}

void BKE_fracture_constraint_setting_remove_all(struct FractureModifierData *fmd)
{
	/* keep the 1st setting as default */
	ConstraintSetting *cs = fmd->constraint_settings.first;

	while (cs->next)
	{
		cs = cs->next;
		BLI_remlink_safe(&fmd->constraint_settings, cs);
	}

	fmd->active_constraint_setting = 0;
}
