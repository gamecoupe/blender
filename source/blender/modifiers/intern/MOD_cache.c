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
 * Contributor(s): Lukas Toenne
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/modifiers/intern/MOD_cache.c
 *  \ingroup modifiers
 */

#include <stdio.h>
#include <stdlib.h>

#include "BLI_utildefines.h"
#include "BLI_string.h"

#include "DNA_scene_types.h"
#include "DNA_object_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"

#include "BKE_cdderivedmesh.h"
#include "BKE_DerivedMesh.h"
#include "BKE_scene.h"
#include "BKE_global.h"
#include "BKE_mesh.h"
#include "BKE_main.h"

#include "MEM_guardedalloc.h"

#include "MOD_modifiertypes.h"

#include "MOD_util.h"

struct BMEditMesh;

static void initData(ModifierData *UNUSED(md))
{
	/*CacheModifierData *pcmd = (CacheModifierData *)md;*/
}

static void copyData(ModifierData *md, ModifierData *target)
{
	/*CacheModifierData *pcmd = (CacheModifierData *)md;*/
	CacheModifierData *tpcmd = (CacheModifierData *)target;
	
	modifier_copyData_generic(md, target);
	
	tpcmd->output_dm = NULL;
	tpcmd->flag &= ~(MOD_CACHE_USE_OUTPUT_REALTIME | MOD_CACHE_USE_OUTPUT_RENDER);
}

static void freeData(ModifierData *md)
{
	CacheModifierData *pcmd = (CacheModifierData *)md;
	
	if (pcmd->output_dm) {
		pcmd->output_dm->release(pcmd->output_dm);
		pcmd->output_dm = NULL;
	}
}

static DerivedMesh *pointcache_do(CacheModifierData *pcmd, Object *UNUSED(ob), DerivedMesh *dm, ModifierApplyFlag flag)
{
	bool use_output = (flag & MOD_APPLY_RENDER) ? (pcmd->flag & MOD_CACHE_USE_OUTPUT_RENDER) : (pcmd->flag & MOD_CACHE_USE_OUTPUT_REALTIME);
	if (use_output) {
		if (pcmd->output_dm) {
			pcmd->output_dm->release(pcmd->output_dm);
		}
		
		pcmd->output_dm = CDDM_copy(dm);
	}
	else {
		if (pcmd->output_dm) {
			dm = pcmd->output_dm;
			pcmd->output_dm = NULL;
		}
	}
	
	return dm;
}

static DerivedMesh *applyModifier(ModifierData *md, Object *ob,
                                  DerivedMesh *dm,
                                  ModifierApplyFlag flag)
{
	CacheModifierData *pcmd = (CacheModifierData *)md;

	return pointcache_do(pcmd, ob, dm, flag);
}

static DerivedMesh *applyModifierEM(ModifierData *md, Object *ob,
                                    struct BMEditMesh *UNUSED(editData),
                                    DerivedMesh *dm,
                                    ModifierApplyFlag flag)
{
	CacheModifierData *pcmd = (CacheModifierData *)md;

	return pointcache_do(pcmd, ob, dm, flag);
}

ModifierTypeInfo modifierType_Cache = {
	/* name */              "Cache",
	/* structName */        "CacheModifierData",
	/* structSize */        sizeof(CacheModifierData),
	/* type */              eModifierTypeType_Constructive,
	/* flags */             eModifierTypeFlag_AcceptsMesh |
	                        eModifierTypeFlag_AcceptsCVs |
	                        eModifierTypeFlag_SupportsEditmode,

	/* copyData */          copyData,
	/* deformVerts */       NULL,
	/* deformMatrices */    NULL,
	/* deformVertsEM */     NULL,
	/* deformMatricesEM */  NULL,
	/* applyModifier */     applyModifier,
	/* applyModifierEM */   applyModifierEM,
	/* initData */          initData,
	/* requiredDataMask */  NULL,
	/* freeData */          freeData,
	/* isDisabled */        NULL,
	/* updateDepgraph */    NULL,
	/* dependsOnTime */     NULL,
	/* dependsOnNormals */  NULL,
	/* foreachObjectLink */ NULL,
	/* foreachIDLink */     NULL,
	/* foreachTexLink */    NULL,
};
