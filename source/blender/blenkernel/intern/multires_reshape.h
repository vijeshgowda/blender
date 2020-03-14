/*
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
 * along with this program; if not, write to the Free Software  Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2020 Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup bke
 */

#ifndef __BKE_INTERN_MULTIRES_RESHAPE_H__
#define __BKE_INTERN_MULTIRES_RESHAPE_H__

#include "BLI_sys_types.h"

struct Depsgraph;
struct GridPaintMask;
struct MDisps;
struct Mesh;
struct MultiresModifierData;
struct Object;
struct Subdiv;
struct SubdivCCG;

typedef struct MultiresReshapeContext {
  /* Base mesh from original object.
   * NOTE: Does NOT include any leading modifiers in it. */
  struct Mesh *base_mesh;

  /* Subdivision surface created for multires modifier.
   *
   * The coarse mesh of this subdivision surface is a base mesh with all deformation modifiers
   * leading multires applied on it. */
  struct Subdiv *subdiv;
  bool need_free_subdiv;

  struct {
    /* Level at which displacement is being assigned to.
     * It will be propagated up from this level to top.level. */
    int level;

    /* Grid size for reshape.level. */
    int grid_size;
  } reshape;

  struct {
    /* Top level of the displacement grids.
     * The displacement will be propagated up to this level. */
    int level;

    /* Grid size for top.level. */
    int grid_size;
  } top;

  struct {
    /* Copy of original displacement and painting masks. */
    struct MDisps *mdisps;
    struct GridPaintMask *grid_paint_masks;
  } orig;

  /* Number of grids which are required for base_mesh. */
  int num_grids;

  /* Destination displacement and mask.
   * Points to a custom data on a destination mesh. */
  struct MDisps *mdisps;
  struct GridPaintMask *grid_paint_masks;

  /* Indexed by face index, gives first grid index of the face. */
  int *face_start_grid_index;

  /* Indexed by grid index, contains face (poly) index in the base mesh from which the grid has
   * been created (in other words, index of a poly which contains loop corresponding to the grid
   * index). */
  int *grid_to_face_index;

  /* Indexed by ptex face index, gives first grid index of the ptex face.
   *
   * For non-quad base faces ptex face is created for every face corner, so it's similar to a
   * grid in this case. In this case start grid index will be the only one for this ptex face.
   *
   * For quad base faces there is a single ptex face but 4 grids. So in this case there will be
   * 4 grids for the ptex, starting at a value stored in this mapping. */
  int *ptex_start_grid_index;

  /* Indexed by base face index, returns first ptex face index corresponding
   * to that base face. */
  int *face_ptex_offset;
} MultiresReshapeContext;

/* Coordinate which identifies element of a grid.
 * This is directly related on how CD_MDISPS stores dispalcement.
 */
typedef struct GridCoord {
  int grid_index;
  float u, v;
} GridCoord;

/* COordinate within ptex, which is what OpenSubdiv API operates on.  */
typedef struct PTexCoord {
  int ptex_face_index;
  float u, v;
} PTexCoord;

/* Element of a grid data stored in the destination mesh.
 * This is where reshaped coordinates and mask values will be written to. */
typedef struct ReshapeGridElement {
  float *displacement;
  float *mask;
} ReshapeGridElement;

typedef struct ReshapeConstGridElement {
  float displacement[3];
  float mask;
} ReshapeConstGridElement;

/* -------------------------------------------------------------------- */
/** \name Construct/destruct reshape context.
 * \{ */

/* Create subdivision surface descriptor which is configured for surface evaluation at a given
 * multires modifier. */
struct Subdiv *multires_reshape_create_subdiv(struct Depsgraph *depsgraph,
                                              struct Object *object,
                                              const struct MultiresModifierData *mmd);

bool multires_reshape_context_create_from_object(MultiresReshapeContext *reshape_context,
                                                 struct Depsgraph *depsgraph,
                                                 struct Object *object,
                                                 struct MultiresModifierData *mmd);

bool multires_reshape_context_create_from_ccg(MultiresReshapeContext *reshape_context,
                                              struct SubdivCCG *subdiv_ccg,
                                              struct Mesh *base_mesh,
                                              int top_level);

bool multires_reshape_context_create_from_subdivide(MultiresReshapeContext *reshape_context,
                                                    struct Object *object,
                                                    struct MultiresModifierData *mmd,
                                                    int top_level);

void multires_reshape_context_free(MultiresReshapeContext *reshape_context);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Helper accessors.
 * \{ */

/* For the given grid index get index of face it was created for. */
int multires_reshape_grid_to_face_index(const MultiresReshapeContext *reshape_context,
                                        int grid_index);

/* For the given grid index get corner of a face it was created for. */
int multires_reshape_grid_to_corner(const MultiresReshapeContext *reshape_context, int grid_index);

bool multires_reshape_is_quad_face(const MultiresReshapeContext *reshape_context, int face_index);

/* For the given grid index get index of corresponding ptex face. */
int multires_reshape_grid_to_ptex_index(const MultiresReshapeContext *reshape_context,
                                        int grid_index);

/* Convert normalized coordinate within a grid to a normalized coordinate within a ptex face. */
PTexCoord multires_reshape_grid_coord_to_ptex(const MultiresReshapeContext *reshape_context,
                                              const GridCoord *grid_coord);

/* Convert a normalized coordinate within a ptex face to a normalized coordinate within a grid. */
GridCoord multires_reshape_ptex_coord_to_grid(const MultiresReshapeContext *reshape_context,
                                              const PTexCoord *ptex_coord);

/* Calculate tangent matrix which converts displacement to a object vector.
 * Is calculated for the given surface derivatives at a given base face corner. */
void multires_reshape_tangent_matrix_for_corner(const MultiresReshapeContext *reshape_context,
                                                const int face_index,
                                                const int corner,
                                                const float dPdu[3],
                                                const float dPdv[3],
                                                float r_tangent_matrix[3][3]);

/* Get grid elements which are to be reshaped at a given or ptex coordinate.
 * The data is coming from final custom mdata layers. */
ReshapeGridElement multires_reshape_grid_element_for_grid_coord(
    const MultiresReshapeContext *reshape_context, const GridCoord *grid_coord);
ReshapeGridElement multires_reshape_grid_element_for_ptex_coord(
    const MultiresReshapeContext *reshape_context, const PTexCoord *ptex_coord);

/* Get original grid element for the given coordinate. */
ReshapeConstGridElement multires_reshape_orig_grid_element_for_grid_coord(
    const MultiresReshapeContext *reshape_context, const GridCoord *grid_coord);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sample limit surface of the base mesh.
 * \{ */

/* Evaluate limit surface created from base mesh.
 * This is the limit surface which defines tangent space for MDisps. */
void multires_reshape_evaluate_limit_at_grid(const MultiresReshapeContext *reshape_context,
                                             const GridCoord *grid_coord,
                                             float r_P[3],
                                             float r_tangent_matrix[3][3]);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Custom data preparation.
 * \{ */

/* Make sure custom data is allocated for the given level. */
void multires_reshape_ensure_grids(struct Mesh *mesh, const int level);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Functions specific to reshaping from a set of vertices in a object position.
 * \{ */

/* Returns truth if all coordinates were assigned.
 *
 * False will be returned if the number of vertex coordinates did not match required number of
 * vertices at a reshape level. */
bool multires_reshape_assign_final_coords_from_vertcos(
    const MultiresReshapeContext *reshape_context,
    const float (*vert_coords)[3],
    const int num_vert_coords);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Functions specific to reshaping from CCG.
 * \{ */

/* NOTE: Displacement grids to be at least at a reshape level.
 *
 * Return truth if all coordinates have been updated. */
bool multires_reshape_assign_final_coords_from_ccg(const MultiresReshapeContext *reshape_context,
                                                   struct SubdivCCG *subdiv_ccg);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Functions specific to reshaping from MDISPS.
 * \{ */

/* Reads and writes to the current mesh CD_MDISPS. */
void multires_reshape_assign_final_coords_from_mdisps(
    const MultiresReshapeContext *reshape_context);

/* Reads from original CD_MIDTSPS, writes to the current mesh CD_MDISPS. */
void multires_reshape_assign_final_coords_from_orig_mdisps(
    const MultiresReshapeContext *reshape_context);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Displacement smooth.
 * \{ */

/* Operates on a displacement grids (CD_MDISPS) which contains object space coordinates stopred for
 * the reshape level.
 *
 * The result is grids which are defining mesh with a smooth surface and details starting from
 * reshape level up to top level added back from original displacement grids. */
void multires_reshape_smooth_object_grids_with_details(
    const MultiresReshapeContext *reshape_context);

/* Operates on a displacement grids (CD_MDISPS) which contains object spacecoordinates stopred for
 * the reshape level.
 *
 * Makes it so surface on top level looks smooth. Details are not preserved
 */
void multires_reshape_smooth_object_grids(const MultiresReshapeContext *reshape_context);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Displacement, space conversion.
 * \{ */

/* Store original grid data, so then it's possible to calculate delta from it and add
 * high-frequency content on top of reshaped grids. */
void multires_reshape_store_original_grids(MultiresReshapeContext *reshape_context);

void multires_reshape_object_grids_to_tangent_displacement(
    const MultiresReshapeContext *reshape_context);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Apply base.
 * \{ */

/* Update mesh coordinates to the final positions of displacement in object space.
 * This is effectively desired position of base mesh vertices after caneling out displacement.
 *
 * NOTE: Expects that mesh's CD_MDISPS has been set ot object space positions. */
void multires_reshape_apply_base_update_mesh_coords(MultiresReshapeContext *reshape_context);

/* Perform better fitting of the base mesh so its subdivided version brings vertices to their
 * desired locations. */
void multires_reshape_apply_base_refit_base_mesh(MultiresReshapeContext *reshape_context);

/* Refine subdivision surface to the new positions of the base mesh. */
void multires_reshape_apply_base_refine_subdiv(MultiresReshapeContext *reshape_context);

/** \} */

#endif /* __BKE_INTERN_MULTIRES_RESHAPE_H__ */
