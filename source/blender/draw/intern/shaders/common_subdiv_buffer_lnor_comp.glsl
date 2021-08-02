
/* To be compile with common_subdiv_lib.glsl */
layout(local_size_x = 1) in;

layout(std430, binding = 1) readonly buffer inputVertexData
{
  PosNorLoop pos_nor[];
};

layout(std430, binding = 2) readonly buffer extraCoarseFaceData
{
  uint extra_coarse_face_data[];
};

layout(std430, binding = 3) writeonly buffer outputLoopNormals
{
  vec3 output_lnor[];
};

uniform int coarse_poly_count;

void main()
{
  /* We execute for each quad, so the start index of the loop is quad_index * 4. */
  uint quad_index = gl_GlobalInvocationID.x;
  uint start_loop_index = quad_index * 4;

  uint coarse_quad_index = coarse_polygon_index_from_subdiv_quad_index(quad_index, coarse_poly_count);

  if (((extra_coarse_face_data[coarse_quad_index] >> 31) & 0x1) != 0) {
    /* Face is smooth, use vertex normals. */
    for (int i = 0; i < 4; i++) {
      output_lnor[start_loop_index + i] = get_vertex_nor(pos_nor[start_loop_index + i]);
    }
  }
  else {
    /* Face is flat shaded, compute flat face normal from an inscribed triangle. */
    vec3 verts[3];
    for (int i = 0; i < 3; i++) {
      verts[i] = get_vertex_pos(pos_nor[start_loop_index + i]);
    }

    vec3 face_normal = normalize(cross(verts[1] - verts[0], verts[2] - verts[0]));
    for (int i = 0; i < 4; i++) {
      output_lnor[start_loop_index + i] = face_normal;
    }
  }
}
