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
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2006 Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup cmpnodes
 */

#include "node_composite_util.h"

/* **************** INVERT ******************** */
static bNodeSocketTemplate cmp_node_invert_in[] = {
    {SOCK_FLOAT, N_("Fac"), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, PROP_FACTOR},
    {SOCK_RGBA, N_("Color"), 1.0f, 1.0f, 1.0f, 1.0f},
    {-1, ""}};

static bNodeSocketTemplate cmp_node_invert_out[] = {{SOCK_RGBA, N_("Color")}, {-1, ""}};

static void node_composit_init_invert(bNodeTree *UNUSED(ntree), bNode *node)
{
  node->custom1 |= CMP_CHAN_RGB;
}

static int node_composit_gpu_invert(GPUMaterial *mat,
                                    bNode *node,
                                    bNodeExecData *UNUSED(execdata),
                                    GPUNodeStack *in,
                                    GPUNodeStack *out)
{
  float do_rgb = (node->custom1 & CMP_CHAN_RGB) ? 1.0f : 0.0f;
  float do_alpha = (node->custom1 & CMP_CHAN_A) ? 1.0f : 0.0f;

  return GPU_stack_link(
      mat, node, "node_composite_invert", in, out, GPU_constant(&do_rgb), GPU_constant(&do_alpha));
}

/* custom1 = mix type */
void register_node_type_cmp_invert(void)
{
  static bNodeType ntype;

  cmp_node_type_base(&ntype, CMP_NODE_INVERT, "Invert", NODE_CLASS_OP_COLOR, 0);
  node_type_socket_templates(&ntype, cmp_node_invert_in, cmp_node_invert_out);
  node_type_init(&ntype, node_composit_init_invert);
  node_type_gpu(&ntype, node_composit_gpu_invert);

  nodeRegisterType(&ntype);
}
