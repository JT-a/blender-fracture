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
 * The Original Code is Copyright (C) 2013 Blender Foundation
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): Joshua Leung, Sergej Reich
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file BKE_rigidbody.h
 *  \ingroup blenkernel
 *  \brief API for Blender-side Rigid Body stuff
 */
 

#ifndef __BKE_RIGIDBODY_H__
#define __BKE_RIGIDBODY_H__

struct RigidBodyWorld;
struct RigidBodyOb;
struct RigidBodyShardCon;
struct RigidBodyShardOb;

struct Scene;
struct Object;
struct Group;
struct MeshIsland;
struct FractureModifierData;

/* -------------- */
/* Memory Management */

void BKE_rigidbody_free_world(struct Scene *scene);
void BKE_rigidbody_free_object(struct Object *ob);
void BKE_rigidbody_free_constraint(struct Object *ob);

/* ...... */
struct RigidBodyWorld *BKE_rigidbody_world_copy(struct RigidBodyWorld *rbw);
struct RigidBodyOb *BKE_rigidbody_copy_object(struct Object *ob, struct Object *obN);
struct RigidBodyCon *BKE_rigidbody_copy_constraint(struct Object *ob);
void BKE_rigidbody_relink_constraint(struct RigidBodyCon *con);

/* -------------- */
/* Setup */

/* create Blender-side settings data - physics objects not initialized yet */
struct RigidBodyWorld *BKE_rigidbody_create_world(struct Scene *scene);
struct RigidBodyOb *BKE_rigidbody_create_object(struct Object *ob, short type);
struct RigidBodyCon *BKE_rigidbody_create_constraint(struct Object *ob, short type);
struct RigidBodyShardOb *BKE_rigidbody_create_shard(struct Object *ob, struct MeshIsland *mi);
struct RigidBodyShardCon *BKE_rigidbody_create_shard_constraint(short type);

void BKE_rigidbody_world_groups_relink(struct RigidBodyWorld *rbw);
void BKE_rigidbody_set_initial_transform(struct Object *ob, struct MeshIsland *mi, struct RigidBodyShardOb *rbo);


/* 'validate' (i.e. make new or replace old) Physics-Engine objects */
void BKE_rigidbody_validate_sim_world(struct Scene *scene, struct RigidBodyWorld *rbw, bool rebuild);
void BKE_rigidbody_validate_sim_shard_constraint(struct RigidBodyWorld *rbw, struct Object *ob, struct RigidBodyShardCon *rbc, short rebuild);
void BKE_rigidbody_validate_sim_shard(struct RigidBodyWorld *rbw, struct MeshIsland *mi, struct Object *ob, short rebuild, int transfer_speeds);
void BKE_rigidbody_validate_sim_shard_shape(struct MeshIsland *mi, struct Object *ob, short rebuild);

/* move the islands of the visible mesh according to shard rigidbody movement */
void BKE_rigidbody_update_cell(struct MeshIsland *mi, struct Object* ob, float loc[3], float rot[4]);

void BKE_rigidbody_calc_center_of_mass(struct Object *ob, float r_com[3]);

/* -------------- */
/* Utilities */

struct RigidBodyWorld *BKE_rigidbody_get_world(struct Scene *scene);
void BKE_rigidbody_remove_object(struct Scene *scene, struct Object *ob);
void BKE_rigidbody_remove_constraint(struct Scene *scene, struct Object *ob);
float BKE_rigidbody_calc_volume(struct DerivedMesh *dm, struct RigidBodyOb *rbo);
void BKE_rigidbody_calc_shard_mass(struct Object* ob, struct MeshIsland* mi);
void BKE_rigidbody_calc_threshold(float max_con_mass, struct Object* rmd, struct RigidBodyShardCon *con);
float BKE_rigidbody_calc_max_con_mass(struct Object* ob);
float BKE_rigidbody_calc_min_con_dist(struct Object* ob);
void BKE_rigidbody_start_dist_angle(struct RigidBodyShardCon* con);
void BKE_rigidbody_remove_shard_con(struct Scene* scene, struct RigidBodyShardCon* con);
void BKE_rigidbody_remove_shard(struct Scene* scene, struct MeshIsland *mi);
/* -------------- */
/* Utility Macros */

/* get mass of Rigid Body Object to supply to RigidBody simulators */
#define RBO_GET_MASS(rbo) \
	((rbo && ((rbo->type == RBO_TYPE_PASSIVE) || (rbo->flag & RBO_FLAG_KINEMATIC) || (rbo->flag & RBO_FLAG_DISABLED))) ? (0.0f) : (rbo->mass))
/* get collision margin for Rigid Body Object, triangle mesh and cone shapes cannot embed margin, convex hull always uses custom margin */
#define RBO_GET_MARGIN(rbo) \
	((rbo->flag & RBO_FLAG_USE_MARGIN || rbo->shape == RB_SHAPE_CONVEXH || rbo->shape == RB_SHAPE_TRIMESH || rbo->shape == RB_SHAPE_CONE) ? (rbo->margin) : (0.04f))

/* -------------- */
/* Simulation */

void BKE_rigidbody_aftertrans_update(struct Object *ob, float loc[3], float rot[3], float quat[4], float rotAxis[3], float rotAngle);
void BKE_rigidbody_sync_transforms(struct RigidBodyWorld *rbw, struct Object *ob, float ctime);
bool BKE_rigidbody_check_sim_running(struct RigidBodyWorld *rbw, struct Object *ob, float ctime);
void BKE_rigidbody_cache_reset(struct RigidBodyWorld *rbw);
void BKE_rigidbody_rebuild_world(struct Scene *scene, float ctime);
void BKE_rigidbody_do_simulation(struct Scene *scene, float ctime);

#endif /* __BKE_RIGIDBODY_H__ */
