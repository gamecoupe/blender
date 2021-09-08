/*
 * Copyright 2011-2013 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "render/film.h"
#include "device/device.h"
#include "render/background.h"
#include "render/bake.h"
#include "render/camera.h"
#include "render/integrator.h"
#include "render/mesh.h"
#include "render/object.h"
#include "render/scene.h"
#include "render/stats.h"
#include "render/tables.h"

#include "util/util_algorithm.h"
#include "util/util_foreach.h"
#include "util/util_math.h"
#include "util/util_math_cdf.h"
#include "util/util_time.h"

CCL_NAMESPACE_BEGIN

/* Pixel Filter */

static float filter_func_box(float /*v*/, float /*width*/)
{
  return 1.0f;
}

static float filter_func_gaussian(float v, float width)
{
  v *= 6.0f / width;
  return expf(-2.0f * v * v);
}

static float filter_func_blackman_harris(float v, float width)
{
  v = M_2PI_F * (v / width + 0.5f);
  return 0.35875f - 0.48829f * cosf(v) + 0.14128f * cosf(2.0f * v) - 0.01168f * cosf(3.0f * v);
}

static vector<float> filter_table(FilterType type, float width)
{
  vector<float> filter_table(FILTER_TABLE_SIZE);
  float (*filter_func)(float, float) = NULL;

  switch (type) {
    case FILTER_BOX:
      filter_func = filter_func_box;
      break;
    case FILTER_GAUSSIAN:
      filter_func = filter_func_gaussian;
      width *= 3.0f;
      break;
    case FILTER_BLACKMAN_HARRIS:
      filter_func = filter_func_blackman_harris;
      width *= 2.0f;
      break;
    default:
      assert(0);
  }

  /* Create importance sampling table. */

  /* TODO(sergey): With the even filter table size resolution we can not
   * really make it nice symmetric importance map without sampling full range
   * (meaning, we would need to sample full filter range and not use the
   * make_symmetric argument).
   *
   * Current code matches exactly initial filter table code, but we should
   * consider either making FILTER_TABLE_SIZE odd value or sample full filter.
   */

  util_cdf_inverted(FILTER_TABLE_SIZE,
                    0.0f,
                    width * 0.5f,
                    function_bind(filter_func, _1, width),
                    true,
                    filter_table);

  return filter_table;
}

/* Film */

NODE_DEFINE(Film)
{
  NodeType *type = NodeType::add("film", create);

  SOCKET_FLOAT(exposure, "Exposure", 0.8f);
  SOCKET_FLOAT(pass_alpha_threshold, "Pass Alpha Threshold", 0.0f);

  static NodeEnum filter_enum;
  filter_enum.insert("box", FILTER_BOX);
  filter_enum.insert("gaussian", FILTER_GAUSSIAN);
  filter_enum.insert("blackman_harris", FILTER_BLACKMAN_HARRIS);

  SOCKET_ENUM(filter_type, "Filter Type", filter_enum, FILTER_BOX);
  SOCKET_FLOAT(filter_width, "Filter Width", 1.0f);

  SOCKET_FLOAT(mist_start, "Mist Start", 0.0f);
  SOCKET_FLOAT(mist_depth, "Mist Depth", 100.0f);
  SOCKET_FLOAT(mist_falloff, "Mist Falloff", 1.0f);

  const NodeEnum *pass_type_enum = Pass::get_type_enum();
  SOCKET_ENUM(display_pass, "Display Pass", *pass_type_enum, PASS_COMBINED);

  SOCKET_BOOLEAN(show_active_pixels, "Show Active Pixels", false);

  static NodeEnum cryptomatte_passes_enum;
  cryptomatte_passes_enum.insert("none", CRYPT_NONE);
  cryptomatte_passes_enum.insert("object", CRYPT_OBJECT);
  cryptomatte_passes_enum.insert("material", CRYPT_MATERIAL);
  cryptomatte_passes_enum.insert("asset", CRYPT_ASSET);
  cryptomatte_passes_enum.insert("accurate", CRYPT_ACCURATE);
  SOCKET_ENUM(cryptomatte_passes, "Cryptomatte Passes", cryptomatte_passes_enum, CRYPT_NONE);

  SOCKET_INT(cryptomatte_depth, "Cryptomatte Depth", 0);

  SOCKET_BOOLEAN(use_approximate_shadow_catcher, "Use Approximate Shadow Catcher", false);

  return type;
}

Film::Film() : Node(get_node_type()), filter_table_offset_(TABLE_OFFSET_INVALID)
{
}

Film::~Film()
{
}

void Film::add_default(Scene *scene)
{
  Pass *pass = scene->create_node<Pass>();
  pass->type = PASS_COMBINED;
}

void Film::device_update(Device *device, DeviceScene *dscene, Scene *scene)
{
  if (!is_modified())
    return;

  scoped_callback_timer timer([scene](double time) {
    if (scene->update_stats) {
      scene->update_stats->film.times.add_entry({"update", time});
    }
  });

  device_free(device, dscene, scene);

  KernelFilm *kfilm = &dscene->data.film;

  /* update __data */
  kfilm->exposure = exposure;
  kfilm->pass_alpha_threshold = pass_alpha_threshold;
  kfilm->pass_flag = 0;

  kfilm->use_approximate_shadow_catcher = get_use_approximate_shadow_catcher();

  kfilm->light_pass_flag = 0;
  kfilm->pass_stride = 0;

  /* Mark with PASS_UNUSED to avoid mask test in the kernel. */
  kfilm->pass_background = PASS_UNUSED;
  kfilm->pass_emission = PASS_UNUSED;
  kfilm->pass_ao = PASS_UNUSED;
  kfilm->pass_diffuse_direct = PASS_UNUSED;
  kfilm->pass_diffuse_indirect = PASS_UNUSED;
  kfilm->pass_glossy_direct = PASS_UNUSED;
  kfilm->pass_glossy_indirect = PASS_UNUSED;
  kfilm->pass_transmission_direct = PASS_UNUSED;
  kfilm->pass_transmission_indirect = PASS_UNUSED;
  kfilm->pass_volume_direct = PASS_UNUSED;
  kfilm->pass_volume_indirect = PASS_UNUSED;
  kfilm->pass_volume_direct = PASS_UNUSED;
  kfilm->pass_volume_indirect = PASS_UNUSED;
  kfilm->pass_shadow = PASS_UNUSED;

  /* Mark passes as unused so that the kernel knows the pass is inaccessible. */
  kfilm->pass_denoising_normal = PASS_UNUSED;
  kfilm->pass_denoising_albedo = PASS_UNUSED;
  kfilm->pass_sample_count = PASS_UNUSED;
  kfilm->pass_adaptive_aux_buffer = PASS_UNUSED;
  kfilm->pass_shadow_catcher = PASS_UNUSED;
  kfilm->pass_shadow_catcher_sample_count = PASS_UNUSED;
  kfilm->pass_shadow_catcher_matte = PASS_UNUSED;

  bool have_cryptomatte = false;
  bool have_aov_color = false;
  bool have_aov_value = false;

  for (size_t i = 0; i < scene->passes.size(); i++) {
    const Pass *pass = scene->passes[i];

    if (pass->type == PASS_NONE || !pass->is_written()) {
      continue;
    }

    if (pass->mode == PassMode::DENOISED) {
      /* Generally we only storing offsets of the noisy passes. The display pass is an exception
       * since it is a read operation and not a write. */
      kfilm->pass_stride += pass->get_info().num_components;
      continue;
    }

    /* Can't do motion pass if no motion vectors are available. */
    if (pass->type == PASS_MOTION || pass->type == PASS_MOTION_WEIGHT) {
      if (scene->need_motion() != Scene::MOTION_PASS) {
        kfilm->pass_stride += pass->get_info().num_components;
        continue;
      }
    }

    int pass_flag = (1 << (pass->type % 32));
    if (pass->type <= PASS_CATEGORY_LIGHT_END) {
      kfilm->light_pass_flag |= pass_flag;
    }
    else if (pass->type <= PASS_CATEGORY_DATA_END) {
      kfilm->pass_flag |= pass_flag;
    }
    else {
      assert(pass->type <= PASS_CATEGORY_BAKE_END);
    }

    switch (pass->type) {
      case PASS_COMBINED:
        kfilm->pass_combined = kfilm->pass_stride;
        break;
      case PASS_DEPTH:
        kfilm->pass_depth = kfilm->pass_stride;
        break;
      case PASS_NORMAL:
        kfilm->pass_normal = kfilm->pass_stride;
        break;
      case PASS_POSITION:
        kfilm->pass_position = kfilm->pass_stride;
        break;
      case PASS_ROUGHNESS:
        kfilm->pass_roughness = kfilm->pass_stride;
        break;
      case PASS_UV:
        kfilm->pass_uv = kfilm->pass_stride;
        break;
      case PASS_MOTION:
        kfilm->pass_motion = kfilm->pass_stride;
        break;
      case PASS_MOTION_WEIGHT:
        kfilm->pass_motion_weight = kfilm->pass_stride;
        break;
      case PASS_OBJECT_ID:
        kfilm->pass_object_id = kfilm->pass_stride;
        break;
      case PASS_MATERIAL_ID:
        kfilm->pass_material_id = kfilm->pass_stride;
        break;

      case PASS_MIST:
        kfilm->pass_mist = kfilm->pass_stride;
        break;
      case PASS_EMISSION:
        kfilm->pass_emission = kfilm->pass_stride;
        break;
      case PASS_BACKGROUND:
        kfilm->pass_background = kfilm->pass_stride;
        break;
      case PASS_AO:
        kfilm->pass_ao = kfilm->pass_stride;
        break;
      case PASS_SHADOW:
        kfilm->pass_shadow = kfilm->pass_stride;
        break;

      case PASS_DIFFUSE_COLOR:
        kfilm->pass_diffuse_color = kfilm->pass_stride;
        break;
      case PASS_GLOSSY_COLOR:
        kfilm->pass_glossy_color = kfilm->pass_stride;
        break;
      case PASS_TRANSMISSION_COLOR:
        kfilm->pass_transmission_color = kfilm->pass_stride;
        break;
      case PASS_DIFFUSE_INDIRECT:
        kfilm->pass_diffuse_indirect = kfilm->pass_stride;
        break;
      case PASS_GLOSSY_INDIRECT:
        kfilm->pass_glossy_indirect = kfilm->pass_stride;
        break;
      case PASS_TRANSMISSION_INDIRECT:
        kfilm->pass_transmission_indirect = kfilm->pass_stride;
        break;
      case PASS_VOLUME_INDIRECT:
        kfilm->pass_volume_indirect = kfilm->pass_stride;
        break;
      case PASS_DIFFUSE_DIRECT:
        kfilm->pass_diffuse_direct = kfilm->pass_stride;
        break;
      case PASS_GLOSSY_DIRECT:
        kfilm->pass_glossy_direct = kfilm->pass_stride;
        break;
      case PASS_TRANSMISSION_DIRECT:
        kfilm->pass_transmission_direct = kfilm->pass_stride;
        break;
      case PASS_VOLUME_DIRECT:
        kfilm->pass_volume_direct = kfilm->pass_stride;
        break;

      case PASS_BAKE_PRIMITIVE:
        kfilm->pass_bake_primitive = kfilm->pass_stride;
        break;
      case PASS_BAKE_DIFFERENTIAL:
        kfilm->pass_bake_differential = kfilm->pass_stride;
        break;

      case PASS_RENDER_TIME:
        break;
      case PASS_CRYPTOMATTE:
        kfilm->pass_cryptomatte = have_cryptomatte ?
                                      min(kfilm->pass_cryptomatte, kfilm->pass_stride) :
                                      kfilm->pass_stride;
        have_cryptomatte = true;
        break;

      case PASS_DENOISING_NORMAL:
        kfilm->pass_denoising_normal = kfilm->pass_stride;
        break;
      case PASS_DENOISING_ALBEDO:
        kfilm->pass_denoising_albedo = kfilm->pass_stride;
        break;

      case PASS_SHADOW_CATCHER:
        kfilm->pass_shadow_catcher = kfilm->pass_stride;
        break;
      case PASS_SHADOW_CATCHER_SAMPLE_COUNT:
        kfilm->pass_shadow_catcher_sample_count = kfilm->pass_stride;
        break;
      case PASS_SHADOW_CATCHER_MATTE:
        kfilm->pass_shadow_catcher_matte = kfilm->pass_stride;
        break;

      case PASS_ADAPTIVE_AUX_BUFFER:
        kfilm->pass_adaptive_aux_buffer = kfilm->pass_stride;
        break;
      case PASS_SAMPLE_COUNT:
        kfilm->pass_sample_count = kfilm->pass_stride;
        break;

      case PASS_AOV_COLOR:
        if (!have_aov_color) {
          kfilm->pass_aov_color = kfilm->pass_stride;
          have_aov_color = true;
        }
        break;
      case PASS_AOV_VALUE:
        if (!have_aov_value) {
          kfilm->pass_aov_value = kfilm->pass_stride;
          have_aov_value = true;
        }
        break;
      default:
        assert(false);
        break;
    }

    kfilm->pass_stride += pass->get_info().num_components;
  }

  /* update filter table */
  vector<float> table = filter_table(filter_type, filter_width);
  scene->lookup_tables->remove_table(&filter_table_offset_);
  filter_table_offset_ = scene->lookup_tables->add_table(dscene, table);
  kfilm->filter_table_offset = (int)filter_table_offset_;

  /* mist pass parameters */
  kfilm->mist_start = mist_start;
  kfilm->mist_inv_depth = (mist_depth > 0.0f) ? 1.0f / mist_depth : 0.0f;
  kfilm->mist_falloff = mist_falloff;

  kfilm->cryptomatte_passes = cryptomatte_passes;
  kfilm->cryptomatte_depth = cryptomatte_depth;

  clear_modified();
}

void Film::device_free(Device * /*device*/, DeviceScene * /*dscene*/, Scene *scene)
{
  scene->lookup_tables->remove_table(&filter_table_offset_);
}

int Film::get_aov_offset(Scene *scene, string name, bool &is_color)
{
  int offset_color = 0, offset_value = 0;
  foreach (const Pass *pass, scene->passes) {
    if (pass->name == name) {
      if (pass->type == PASS_AOV_VALUE) {
        is_color = false;
        return offset_value;
      }
      else if (pass->type == PASS_AOV_COLOR) {
        is_color = true;
        return offset_color;
      }
    }

    if (pass->type == PASS_AOV_VALUE) {
      offset_value += pass->get_info().num_components;
    }
    else if (pass->type == PASS_AOV_COLOR) {
      offset_color += pass->get_info().num_components;
    }
  }

  return -1;
}

const Pass *Film::get_actual_display_pass(Scene *scene, PassType pass_type, PassMode pass_mode)
{
  const Pass *pass = Pass::find(scene->passes, pass_type, pass_mode);

  /* Fall back to noisy pass if no denoised one is found. */
  if (pass == nullptr && pass_mode == PassMode::DENOISED) {
    pass = Pass::find(scene->passes, pass_type, PassMode::NOISY);
  }

  return get_actual_display_pass(scene, pass);
}

const Pass *Film::get_actual_display_pass(Scene *scene, const Pass *pass)
{
  if (!pass) {
    return nullptr;
  }

  if (pass->type == PASS_COMBINED && scene->has_shadow_catcher()) {
    const Pass *shadow_catcher_matte_pass = Pass::find(
        scene->passes, PASS_SHADOW_CATCHER_MATTE, pass->mode);
    if (shadow_catcher_matte_pass) {
      pass = shadow_catcher_matte_pass;
    }
  }

  return pass;
}

void Film::update_passes(Scene *scene, bool add_sample_count_pass)
{
  const Background *background = scene->background;
  const BakeManager *bake_manager = scene->bake_manager;
  const ObjectManager *object_manager = scene->object_manager;
  Integrator *integrator = scene->integrator;

  if (!is_modified() && !object_manager->need_update() && !integrator->is_modified()) {
    return;
  }

  /* Remove auto generated passes and recreate them. */
  remove_auto_passes(scene);

  /* Display pass for viewport. */
  const PassType display_pass = get_display_pass();
  add_auto_pass(scene, display_pass);

  /* Assumption is that a combined pass always exists for now, for example
   * adaptive sampling is always based on a combined pass. But we should
   * try to lift this limitation in the future for faster rendering of
   * individual passes. */
  if (display_pass != PASS_COMBINED) {
    add_auto_pass(scene, PASS_COMBINED);
  }

  /* Create passes needed for adaptive sampling. */
  const AdaptiveSampling adaptive_sampling = integrator->get_adaptive_sampling();
  if (adaptive_sampling.use) {
    add_auto_pass(scene, PASS_SAMPLE_COUNT);
    add_auto_pass(scene, PASS_ADAPTIVE_AUX_BUFFER);
  }

  /* Create passes needed for denoising. */
  const bool use_denoise = integrator->get_use_denoise();
  if (use_denoise) {
    if (integrator->get_use_denoise_pass_normal()) {
      add_auto_pass(scene, PASS_DENOISING_NORMAL);
    }
    if (integrator->get_use_denoise_pass_albedo()) {
      add_auto_pass(scene, PASS_DENOISING_ALBEDO);
    }
  }

  /* Create passes for shadow catcher. */
  if (scene->has_shadow_catcher()) {
    const bool need_background = get_use_approximate_shadow_catcher() &&
                                 !background->get_transparent();

    add_auto_pass(scene, PASS_SHADOW_CATCHER);
    add_auto_pass(scene, PASS_SHADOW_CATCHER_SAMPLE_COUNT);
    add_auto_pass(scene, PASS_SHADOW_CATCHER_MATTE);

    if (need_background) {
      add_auto_pass(scene, PASS_BACKGROUND);
    }
  }
  else if (Pass::contains(scene->passes, PASS_SHADOW_CATCHER)) {
    add_auto_pass(scene, PASS_SHADOW_CATCHER);
    add_auto_pass(scene, PASS_SHADOW_CATCHER_SAMPLE_COUNT);
  }

  const vector<Pass *> passes_immutable = scene->passes;
  for (const Pass *pass : passes_immutable) {
    const PassInfo info = Pass::get_info(pass->type, pass->include_albedo);
    /* Add utility passes needed to generate some light passes. */
    if (info.divide_type != PASS_NONE) {
      add_auto_pass(scene, info.divide_type);
    }
    if (info.direct_type != PASS_NONE) {
      add_auto_pass(scene, info.direct_type);
    }
    if (info.indirect_type != PASS_NONE) {
      add_auto_pass(scene, info.indirect_type);
    }

    /* NOTE: Enable all denoised passes when storage is requested.
     * This way it is possible to tweak denoiser parameters later on. */
    if (info.support_denoise && use_denoise) {
      add_auto_pass(scene, pass->type, PassMode::DENOISED);
    }
  }

  if (bake_manager->get_baking()) {
    add_auto_pass(scene, PASS_BAKE_PRIMITIVE, "BakePrimitive");
    add_auto_pass(scene, PASS_BAKE_DIFFERENTIAL, "BakeDifferential");
  }

  if (add_sample_count_pass) {
    if (!Pass::contains(scene->passes, PASS_SAMPLE_COUNT)) {
      add_auto_pass(scene, PASS_SAMPLE_COUNT);
    }
  }

  /* Remove duplicates and initialize internal pass info. */
  finalize_passes(scene, use_denoise);

  /* Flush scene updates. */
  const bool have_uv_pass = Pass::contains(scene->passes, PASS_UV);
  const bool have_motion_pass = Pass::contains(scene->passes, PASS_MOTION);
  const bool have_ao_pass = Pass::contains(scene->passes, PASS_AO);

  if (have_uv_pass != prev_have_uv_pass) {
    scene->geometry_manager->tag_update(scene, GeometryManager::UV_PASS_NEEDED);
    foreach (Shader *shader, scene->shaders)
      shader->need_update_uvs = true;
  }
  if (have_motion_pass != prev_have_motion_pass) {
    scene->geometry_manager->tag_update(scene, GeometryManager::MOTION_PASS_NEEDED);
  }
  if (have_ao_pass != prev_have_ao_pass) {
    scene->integrator->tag_update(scene, Integrator::AO_PASS_MODIFIED);
  }

  prev_have_uv_pass = have_uv_pass;
  prev_have_motion_pass = have_motion_pass;
  prev_have_ao_pass = have_ao_pass;

  tag_modified();

  /* Debug logging. */
  if (VLOG_IS_ON(2)) {
    VLOG(2) << "Effective scene passes:";
    for (const Pass *pass : scene->passes) {
      VLOG(2) << "- " << *pass;
    }
  }
}

void Film::add_auto_pass(Scene *scene, PassType type, const char *name)
{
  add_auto_pass(scene, type, PassMode::NOISY, name);
}

void Film::add_auto_pass(Scene *scene, PassType type, PassMode mode, const char *name)
{
  Pass *pass = new Pass();
  pass->type = type;
  pass->mode = mode;
  pass->name = (name) ? name : "";
  pass->is_auto_ = true;

  pass->set_owner(scene);
  scene->passes.push_back(pass);
}

void Film::remove_auto_passes(Scene *scene)
{
  /* Remove all passes which were automatically created. */
  vector<Pass *> new_passes;

  for (Pass *pass : scene->passes) {
    if (!pass->is_auto_) {
      new_passes.push_back(pass);
    }
    else {
      delete pass;
    }
  }

  scene->passes = new_passes;
}

static bool compare_pass_order(const Pass *a, const Pass *b)
{
  const int num_components_a = a->get_info().num_components;
  const int num_components_b = b->get_info().num_components;

  if (num_components_a == num_components_b) {
    return (a->type < b->type);
  }

  return num_components_a > num_components_b;
}

void Film::finalize_passes(Scene *scene, const bool use_denoise)
{
  /* Remove duplicate passes. */
  vector<Pass *> new_passes;

  for (Pass *pass : scene->passes) {
    /* Disable denoising on passes if denoising is disabled, or if the
     * pass does not support it. */
    pass->mode = (use_denoise && pass->get_info().support_denoise) ? pass->mode : PassMode::NOISY;

    /* Merge duplicate passes. */
    bool duplicate_found = false;
    for (Pass *new_pass : new_passes) {
      /* If different type or denoising, don't merge. */
      if (new_pass->type != pass->type || new_pass->mode != pass->mode) {
        continue;
      }

      /* If both passes have a name and the names are different, don't merge.
       * If either pass has a name, we'll use that name. */
      if (!pass->name.empty() && !new_pass->name.empty() && pass->name != new_pass->name) {
        continue;
      }

      if (!pass->name.empty() && new_pass->name.empty()) {
        new_pass->name = pass->name;
      }

      new_pass->is_auto_ &= pass->is_auto_;
      duplicate_found = true;
      break;
    }

    if (!duplicate_found) {
      new_passes.push_back(pass);
    }
    else {
      delete pass;
    }
  }

  /* Order from by components and type, This is required to for AOVs and cryptomatte passes,
   * which the kernel assumes to be in order. Note this must use stable sort so cryptomatte
   * passes remain in the right order. */
  stable_sort(new_passes.begin(), new_passes.end(), compare_pass_order);

  scene->passes = new_passes;
}

uint Film::get_kernel_features(const Scene *scene) const
{
  uint kernel_features = 0;

  for (const Pass *pass : scene->passes) {
    if (!pass->is_written()) {
      continue;
    }

    if (pass->mode == PassMode::DENOISED || pass->type == PASS_DENOISING_NORMAL ||
        pass->type == PASS_DENOISING_ALBEDO) {
      kernel_features |= KERNEL_FEATURE_DENOISING;
    }

    if (pass->type != PASS_NONE && pass->type != PASS_COMBINED &&
        pass->type <= PASS_CATEGORY_LIGHT_END) {
      kernel_features |= KERNEL_FEATURE_LIGHT_PASSES;

      if (pass->type == PASS_SHADOW) {
        kernel_features |= KERNEL_FEATURE_SHADOW_PASS;
      }
    }

    if (pass->type == PASS_AO) {
      kernel_features |= KERNEL_FEATURE_NODE_RAYTRACE;
    }
  }

  return kernel_features;
}

CCL_NAMESPACE_END
