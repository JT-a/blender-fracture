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
 * The Original Code is Copyright (C) 2008, Blender Foundation
 * This is a new part of Blender (with some old code)
 *
 * Contributor(s): Joshua Leung
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file ED_keyframing.h
 *  \ingroup editors
 */

#ifndef __ED_KEYFRAMING_H__
#define __ED_KEYFRAMING_H__

#ifdef __cplusplus
extern "C" {
#endif

struct Main;
struct ListBase;
struct ID;
struct Scene;

struct KeyingSet;

struct bAction;
struct FCurve;
struct BezTriple;

struct bPoseChannel;
struct bConstraint;

struct bContext;
struct wmOperatorType;
struct ReportList;

struct PointerRNA;
struct PropertyRNA;
struct EnumPropertyItem;

#include "RNA_types.h"

/* ************ Keyframing Management **************** */

/* Get the active settings for keyframing settings from context (specifically the given scene) 
 *	- incl_mode: include settings from keyframing mode in the result (i.e. replace only)
 */
short ANIM_get_keyframing_flags(struct Scene *scene, short incl_mode);

/* -------- */

/* Get (or add relevant data to be able to do so) the Active Action for the given 
 * Animation Data block, given an ID block where the Animation Data should reside.
 */
struct bAction *verify_adt_action(struct ID *id, short add);

/* Get (or add relevant data to be able to do so) F-Curve from the given Action. 
 * This assumes that all the destinations are valid.
 */
struct FCurve *verify_fcurve(struct bAction *act, const char group[], struct PointerRNA *ptr,
                             const char rna_path[], const int array_index, short add);

/* -------- */

/* Lesser Keyframing API call:
 *  Update integer/discrete flags of the FCurve (used when creating/inserting keyframes,
 *  but also through RNA when editing an ID prop, see T37103).
 */
void update_autoflags_fcurve(struct FCurve *fcu, struct bContext *C, struct ReportList *reports,
                             struct PointerRNA *ptr);

/* -------- */

/* Lesser Keyframing API call:
 *  Use this when validation of necessary animation data isn't necessary as it already
 *  exists, and there is a beztriple that can be directly copied into the array.
 */
int insert_bezt_fcurve(struct FCurve *fcu, struct BezTriple *bezt, short flag);

/* Main Keyframing API call: 
 *  Use this when validation of necessary animation data isn't necessary as it
 *  already exists. It will insert a keyframe using the current value being keyframed.
 *  Returns the index at which a keyframe was added (or -1 if failed)
 */
int insert_vert_fcurve(struct FCurve *fcu, float x, float y, short flag);

/* -------- */

/* Secondary Keyframing API calls: 
 *	Use this to insert a keyframe using the current value being keyframed, in the 
 *	nominated F-Curve (no creation of animation data performed). Returns success.
 */
bool insert_keyframe_direct(struct ReportList *reports, struct PointerRNA ptr, struct PropertyRNA *prop, struct FCurve *fcu, float cfra, short flag);

/* -------- */

/* Main Keyframing API calls: 
 *	Use this to create any necessary animation data, and then insert a keyframe
 *	using the current value being keyframed, in the relevant place. Returns success.
 */
short insert_keyframe(struct ReportList *reports, struct ID *id, struct bAction *act, const char group[], const char rna_path[], int array_index, float cfra, short flag);

/* Main Keyframing API call: 
 *  Use this to delete keyframe on current frame for relevant channel. Will perform checks just in case.
 */
short delete_keyframe(struct ReportList *reports, struct ID *id, struct bAction *act, const char group[], const char rna_path[], int array_index, float cfra, short flag);

/* ************ Keying Sets ********************** */

/* forward decl. for this struct which is declared a bit later... */
struct KeyingSetInfo;
struct ExtensionRNA;

/* Polling Callback for KeyingSets */
typedef int (*cbKeyingSet_Poll)(struct KeyingSetInfo *ksi, struct bContext *C);
/* Context Iterator Callback for KeyingSets */
typedef void (*cbKeyingSet_Iterator)(struct KeyingSetInfo *ksi, struct bContext *C, struct KeyingSet *ks);
/* Property Specifier Callback for KeyingSets (called from iterators) */
typedef void (*cbKeyingSet_Generate)(struct KeyingSetInfo *ksi, struct bContext *C, struct KeyingSet *ks, struct PointerRNA *ptr); 


/* Callback info for 'Procedural' KeyingSets to use */
typedef struct KeyingSetInfo {
	struct KeyingSetInfo *next, *prev;
	
	/* info */
	/* identifier used for class name, which KeyingSet instances reference as "Typeinfo Name" */
	char idname[64];
	/* identifier so that user can hook this up to a KeyingSet (used as label). */
	char name[64];
	/* short help/description. */
	char description[240]; /* RNA_DYN_DESCR_MAX */
	/* keying settings */
	short keyingflag;
	
	/* polling callbacks */
	/* callback for polling the context for whether the right data is available */
	cbKeyingSet_Poll poll;
	
	/* generate callbacks */
	/* iterator to use to go through collections of data in context
	 *  - this callback is separate from the 'adding' stage, allowing
	 *    BuiltIn KeyingSets to be manually specified to use
	 */
	cbKeyingSet_Iterator iter;
	/* generator to use to add properties based on the data found by iterator */
	cbKeyingSet_Generate generate;
	
	/* RNA integration */
	struct ExtensionRNA ext;
} KeyingSetInfo;

/* -------- */

/* Add another data source for Relative Keying Sets to be evaluated with */
void ANIM_relative_keyingset_add_source(ListBase *dsources, struct ID *id, struct StructRNA *srna, void *data);


/* mode for modify_keyframes */
typedef enum eModifyKey_Modes {
	MODIFYKEY_MODE_INSERT = 0,
	MODIFYKEY_MODE_DELETE,
} eModifyKey_Modes;

/* return codes for errors (with Relative KeyingSets) */
typedef enum eModifyKey_Returns {
	/* context info was invalid for using the Keying Set */
	MODIFYKEY_INVALID_CONTEXT = -1,
	/* there isn't any typeinfo for generating paths from context */
	MODIFYKEY_MISSING_TYPEINFO = -2,
} eModifyKey_Returns;

/* poll the current KeyingSet, updating it's set of paths (if "builtin"/"relative") for context changes */
short ANIM_validate_keyingset(struct bContext *C, ListBase *dsources, struct KeyingSet *ks);

/* use the specified KeyingSet to add/remove various Keyframes on the specified frame */
int ANIM_apply_keyingset(struct bContext *C, ListBase *dsources, struct bAction *act, struct KeyingSet *ks, short mode, float cfra);

/* -------- */

/* Get the first builtin KeyingSet with the given name, which occurs after the given one (or start of list if none given) */
struct KeyingSet *ANIM_builtin_keyingset_get_named(struct KeyingSet *prevKS, const char name[]);

/* Find KeyingSet type info given a name */
KeyingSetInfo *ANIM_keyingset_info_find_name(const char name[]);

/* Find a given ID in the KeyingSet */
bool ANIM_keyingset_find_id(struct KeyingSet *ks, ID *id);

/* for RNA type registrations... */
void ANIM_keyingset_info_register(KeyingSetInfo *ksi);
void ANIM_keyingset_info_unregister(struct Main *bmain, KeyingSetInfo *ksi);

/* cleanup on exit */
void ANIM_keyingset_infos_exit(void);

/* -------- */

/* Get the active KeyingSet for the given scene */
struct KeyingSet *ANIM_scene_get_active_keyingset(struct Scene *scene);

/* Get the index of the Keying Set provided, for the given Scene */
int ANIM_scene_get_keyingset_index(struct Scene *scene, struct KeyingSet *ks);

/* Get Keying Set to use for Auto-Keyframing some transforms */
struct KeyingSet *ANIM_get_keyingset_for_autokeying(struct Scene *scene, const char *tranformKSName);

/* Dynamically populate an enum of Keying Sets */
struct EnumPropertyItem *ANIM_keying_sets_enum_itemf(struct bContext *C, struct PointerRNA *ptr, struct PropertyRNA *prop, bool *r_free);

/* Check if KeyingSet can be used in the current context */
bool ANIM_keyingset_context_ok_poll(struct bContext *C, struct KeyingSet *ks);

/* ************ Drivers ********************** */

/* Flags for use by driver creation calls */
typedef enum eCreateDriverFlags {
	CREATEDRIVER_WITH_DEFAULT_DVAR  = (1 << 0),   /* create drivers with a default variable for nicer UI */
	CREATEDRIVER_WITH_FMODIFIER     = (1 << 1),   /* create drivers with Generator FModifier (for backwards compat) */
} eCreateDriverFlags;

/* -------- */

/* Low-level call to add a new driver F-Curve. This shouldn't be used directly for most tools,
 * although there are special cases where this approach is preferable.
 */
struct FCurve *verify_driver_fcurve(struct ID *id, const char rna_path[], const int array_index, short add);

/* -------- */

/* Returns whether there is a driver in the copy/paste buffer to paste */
bool ANIM_driver_can_paste(void);

/* Main Driver Management API calls:
 *  Add a new driver for the specified property on the given ID block
 */
int ANIM_add_driver(struct ReportList *reports, struct ID *id, const char rna_path[], int array_index, short flag, int type);

/* Main Driver Management API calls:
 *  Remove the driver for the specified property on the given ID block (if available)
 */
bool ANIM_remove_driver(struct ReportList *reports, struct ID *id, const char rna_path[], int array_index, short flag);

/* Main Driver Management API calls:
 *  Make a copy of the driver for the specified property on the given ID block
 */
bool ANIM_copy_driver(struct ReportList *reports, struct ID *id, const char rna_path[], int array_index, short flag);

/* Main Driver Management API calls:
 *  Add a new driver for the specified property on the given ID block or replace an existing one
 *	with the driver + driver-curve data from the buffer 
 */
bool ANIM_paste_driver(struct ReportList *reports, struct ID *id, const char rna_path[], int array_index, short flag);

/* ************ Auto-Keyframing ********************** */
/* Notes:
 * - All the defines for this (User-Pref settings and Per-Scene settings)
 *  are defined in DNA_userdef_types.h
 * - Scene settings take presidence over those for userprefs, with old files
 *  inheriting userpref settings for the scene settings
 * - "On/Off + Mode" are stored per Scene, but "settings" are currently stored
 *  as userprefs
 */

/* Auto-Keying macros for use by various tools */
/* check if auto-keyframing is enabled (per scene takes presidence) */
#define IS_AUTOKEY_ON(scene)  ((scene) ? (scene->toolsettings->autokey_mode & AUTOKEY_ON) : (U.autokey_mode & AUTOKEY_ON))
/* check the mode for auto-keyframing (per scene takes presidence)  */
#define IS_AUTOKEY_MODE(scene, mode)  ((scene) ? (scene->toolsettings->autokey_mode == AUTOKEY_MODE_##mode) : (U.autokey_mode == AUTOKEY_MODE_##mode))
/* check if a flag is set for auto-keyframing (per scene takes presidence) */
#define IS_AUTOKEY_FLAG(scene, flag) \
	((scene) ? \
		((scene->toolsettings->autokey_flag & AUTOKEY_FLAG_##flag) || (U.autokey_flag & AUTOKEY_FLAG_##flag)) \
	 : \
		(U.autokey_flag & AUTOKEY_FLAG_##flag))

/* auto-keyframing feature - checks for whether anything should be done for the current frame */
bool autokeyframe_cfra_can_key(struct Scene *scene, struct ID *id);

/* ************ Keyframe Checking ******************** */

/* Lesser Keyframe Checking API call:
 *	- Used for the buttons to check for keyframes...
 */
bool fcurve_frame_has_keyframe(struct FCurve *fcu, float frame, short filter);

/* Main Keyframe Checking API call:
 * Checks whether a keyframe exists for the given ID-block one the given frame.
 *  - It is recommended to call this method over the other keyframe-checkers directly,
 *    in case some detail of the implementation changes...
 *	- frame: the value of this is quite often result of BKE_scene_frame_get()
 */
bool id_frame_has_keyframe(struct ID *id, float frame, short filter);

/* filter flags for id_cfra_has_keyframe 
 *
 * WARNING: do not alter order of these, as also stored in files
 *	(for v3d->keyflags)
 */
typedef enum eAnimFilterFlags {
	/* general */
	ANIMFILTER_KEYS_LOCAL   = (1 << 0),       /* only include locally available anim data */
	ANIMFILTER_KEYS_MUTED   = (1 << 1),       /* include muted elements */
	ANIMFILTER_KEYS_ACTIVE  = (1 << 2),       /* only include active-subelements */

	/* object specific */
	ANIMFILTER_KEYS_NOMAT   = (1 << 9),   /* don't include material keyframes */
	ANIMFILTER_KEYS_NOSKEY  = (1 << 10),  /* don't include shape keys (for geometry) */
} eAnimFilterFlags;

/* utility funcs for auto keyframe */
bool ED_autokeyframe_object(struct bContext *C, struct Scene *scene, struct Object *ob, struct KeyingSet *ks);
bool ED_autokeyframe_pchan(struct bContext *C, struct Scene *scene, struct Object *ob, struct bPoseChannel *pchan, struct KeyingSet *ks);

/* Names for builtin keying sets so we don't confuse these with labels/text,
 * defined in python script: keyingsets_builtins.py */
#define ANIM_KS_LOCATION_ID         "Location"
#define ANIM_KS_ROTATION_ID         "Rotation"
#define ANIM_KS_SCALING_ID          "Scaling"
#define ANIM_KS_LOC_ROT_SCALE_ID    "LocRotScale"
#define ANIM_KS_AVAILABLE_ID        "Available"
#define ANIM_KS_WHOLE_CHARACTER_ID  "WholeCharacter"

#ifdef __cplusplus
}
#endif

#endif /*  __ED_KEYFRAMING_H__ */
