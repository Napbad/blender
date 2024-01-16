/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 */

#include "BKE_image.h"
#include "BKE_lib_id.hh"
#include "BKE_node.hh"
#include "BKE_studiolight.h"

#include "NOD_shader.h"

#include "GPU_material.h"

#include "eevee_instance.hh"

namespace blender::eevee {

/* -------------------------------------------------------------------- */
/** \name Viewport Override Node-Tree
 * \{ */

LookdevWorld::LookdevWorld()
{
  bNodeTree *ntree = ntreeAddTree(nullptr, "Lookdev World Nodetree", ntreeType_Shader->idname);

  bNode *coordinate = nodeAddStaticNode(nullptr, ntree, SH_NODE_TEX_COORD);
  bNodeSocket *coordinate_out = nodeFindSocket(coordinate, SOCK_OUT, "Generated");

  bNode *rotate = nodeAddStaticNode(nullptr, ntree, SH_NODE_VECTOR_ROTATE);
  rotate->custom1 = NODE_VECTOR_ROTATE_TYPE_AXIS_Z;
  bNodeSocket *rotate_vector_in = nodeFindSocket(rotate, SOCK_IN, "Vector");
  angle_socket_ = static_cast<bNodeSocketValueFloat *>(
      static_cast<void *>(nodeFindSocket(rotate, SOCK_IN, "Angle")->default_value));
  bNodeSocket *rotate_out = nodeFindSocket(rotate, SOCK_OUT, "Vector");

  bNode *environment = nodeAddStaticNode(nullptr, ntree, SH_NODE_TEX_ENVIRONMENT);
  environment_node_ = environment;
  NodeTexImage *environment_storage = static_cast<NodeTexImage *>(environment->storage);
  bNodeSocket *environment_vector_in = nodeFindSocket(environment, SOCK_IN, "Vector");
  bNodeSocket *environment_out = nodeFindSocket(environment, SOCK_OUT, "Color");

  bNode *background = nodeAddStaticNode(nullptr, ntree, SH_NODE_BACKGROUND);
  bNodeSocket *background_out = nodeFindSocket(background, SOCK_OUT, "Background");
  bNodeSocket *background_color_in = nodeFindSocket(background, SOCK_IN, "Color");
  intensity_socket_ = static_cast<bNodeSocketValueFloat *>(
      static_cast<void *>(nodeFindSocket(background, SOCK_IN, "Strength")->default_value));

  bNode *output = nodeAddStaticNode(nullptr, ntree, SH_NODE_OUTPUT_WORLD);
  bNodeSocket *output_in = nodeFindSocket(output, SOCK_IN, "Surface");

  nodeAddLink(ntree, coordinate, coordinate_out, rotate, rotate_vector_in);
  nodeAddLink(ntree, rotate, rotate_out, environment, environment_vector_in);
  nodeAddLink(ntree, environment, environment_out, background, background_color_in);
  nodeAddLink(ntree, background, background_out, output, output_in);
  nodeSetActive(ntree, output);

  /* Create a dummy image data block to hold GPU textures generated by studio-lights. */
  STRNCPY(image.id.name, "IMLookdev");
  BKE_libblock_init_empty(&image.id);
  image.type = IMA_TYPE_IMAGE;
  image.source = IMA_SRC_GENERATED;
  ImageTile *base_tile = BKE_image_get_tile(&image, 0);
  base_tile->gen_x = 1;
  base_tile->gen_y = 1;
  base_tile->gen_type = IMA_GENTYPE_BLANK;
  copy_v4_fl(base_tile->gen_color, 0.0f);
  /* TODO: This works around the issue that the first time the texture is accessed the image would
   * overwrite the set GPU texture. A better solution would be to use image data-blocks as part of
   * the studio-lights, but that requires a larger refactoring. */
  BKE_image_get_gpu_texture(&image, &environment_storage->iuser, nullptr);

  /* Create a dummy image data block to hold GPU textures generated by studio-lights. */
  STRNCPY(world.id.name, "WOLookdev");
  BKE_libblock_init_empty(&world.id);
  world.use_nodes = true;
  world.nodetree = ntree;
}

LookdevWorld::~LookdevWorld()
{
  BKE_libblock_free_datablock(&image.id, 0);
  BKE_libblock_free_datablock(&world.id, 0);
}

bool LookdevWorld::sync(const LookdevParameters &new_parameters)
{
  const bool parameters_changed = assign_if_different(parameters_, new_parameters);

  if (parameters_changed) {
    intensity_socket_->value = parameters_.intensity;
    angle_socket_->value = parameters_.rot_z;

    GPU_TEXTURE_FREE_SAFE(image.gputexture[TEXTARGET_2D][0]);
    environment_node_->id = nullptr;

    StudioLight *sl = BKE_studiolight_find(parameters_.hdri.c_str(),
                                           STUDIOLIGHT_ORIENTATIONS_MATERIAL_MODE);
    if (sl) {
      BKE_studiolight_ensure_flag(sl, STUDIOLIGHT_EQUIRECT_RADIANCE_GPUTEXTURE);
      GPUTexture *texture = sl->equirect_radiance_gputexture;
      if (texture != nullptr) {
        GPU_texture_ref(texture);
        image.gputexture[TEXTARGET_2D][0] = texture;
        environment_node_->id = &image.id;
      }
    }

    GPU_material_free(&world.gpumaterial);
  }
  return parameters_changed;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Lookdev
 *
 * \{ */

LookdevModule::LookdevModule(Instance &inst) : inst_(inst) {}

LookdevModule::~LookdevModule() {}

void LookdevModule::init(const rcti *visible_rect)
{
  visible_rect_ = *visible_rect;
  enabled_ = inst_.is_viewport() && inst_.overlays_enabled() && inst_.use_lookdev_overlay();

  if (enabled_) {
    const int2 extent_dummy(1);
    constexpr eGPUTextureUsage usage = GPU_TEXTURE_USAGE_SHADER_WRITE |
                                       GPU_TEXTURE_USAGE_SHADER_READ;
    dummy_cryptomatte_tx_.ensure_2d(GPU_RGBA32F, extent_dummy, usage);
    dummy_aov_color_tx_.ensure_2d_array(GPU_RGBA16F, extent_dummy, 1, usage);
    dummy_aov_value_tx_.ensure_2d_array(GPU_R16F, extent_dummy, 1, usage);
  }
}

float LookdevModule::calc_viewport_scale()
{
  const float viewport_scale = clamp_f(
      BLI_rcti_size_x(&visible_rect_) / (2000.0f * UI_SCALE_FAC), 0.5f, 1.0f);
  return viewport_scale;
}

static eDRWLevelOfDetail calc_level_of_detail(const float viewport_scale)
{
  float res_scale = clamp_f(
      (U.lookdev_sphere_size / 400.0f) * viewport_scale * UI_SCALE_FAC, 0.1f, 1.0f);

  if (res_scale > 0.7f) {
    return DRW_LOD_HIGH;
  }
  else if (res_scale > 0.25f) {
    return DRW_LOD_MEDIUM;
  }
  return DRW_LOD_LOW;
}

static int calc_sphere_size(const float viewport_scale)
{
  const int sphere_radius = U.lookdev_sphere_size * UI_SCALE_FAC * viewport_scale;
  const int sphere_size = sphere_radius * 2;
  return sphere_size;
}

void LookdevModule::sync()
{
  for (Sphere &sphere : spheres_) {
    sphere.pass.init();
  }
  display_ps_.init();

  if (!enabled_) {
    return;
  }
  const float viewport_scale = calc_viewport_scale();
  const int sphere_size = calc_sphere_size(viewport_scale);
  const int2 extent(sphere_size, sphere_size);

  const eGPUTextureFormat depth_format = GPU_DEPTH_COMPONENT24;
  const eGPUTextureFormat color_format = GPU_RGBA16F;

  depth_tx_.ensure_2d(depth_format, extent);
  for (int index : IndexRange(num_spheres)) {
    if (spheres_[index].color_tx_.ensure_2d(color_format, extent)) {
      if (inst_.sampling.finished_viewport()) {
        inst_.sampling.reset();
      }
    }

    spheres_[index].framebuffer.ensure(GPU_ATTACHMENT_TEXTURE(depth_tx_),
                                       GPU_ATTACHMENT_TEXTURE(spheres_[index].color_tx_));
  }

  float4 position = inst_.camera.data_get().viewinv *
                    float4(0.0, 0.0, -inst_.camera.data_get().clip_near, 1.0);
  float4x4 model_m4 = float4x4::identity();
  model_m4 = math::translate(model_m4, float3(position));
  model_m4 = math::scale(model_m4, float3(sphere_scale));

  ResourceHandle handle = inst_.manager->resource_handle(model_m4);
  GPUBatch *geom = DRW_cache_sphere_get(calc_level_of_detail(viewport_scale));

  sync_pass(spheres_[0].pass, geom, inst_.materials.metallic_mat, handle);
  sync_pass(spheres_[1].pass, geom, inst_.materials.diffuse_mat, handle);
  sync_display();
}

void LookdevModule::sync_pass(PassSimple &pass,
                              GPUBatch *geom,
                              ::Material *mat,
                              ResourceHandle res_handle)
{
  pass.clear_depth(1.0f);
  pass.clear_color(float4(0.0, 0.0, 0.0, 1.0));

  const DRWState state = DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH |
                         DRW_STATE_DEPTH_LESS_EQUAL | DRW_STATE_CULL_BACK;

  GPUMaterial *gpumat = inst_.shaders.material_shader_get(
      mat, mat->nodetree, MAT_PIPE_FORWARD, MAT_GEOM_MESH, MAT_PROBE_NONE);
  pass.state_set(state);
  pass.material_set(*inst_.manager, gpumat);

  pass.bind_texture(RBUFS_UTILITY_TEX_SLOT, inst_.pipelines.utility_tx);
  pass.bind_image("rp_cryptomatte_img", dummy_cryptomatte_tx_);
  pass.bind_image("rp_color_img", dummy_aov_color_tx_);
  pass.bind_image("rp_value_img", dummy_aov_value_tx_);
  pass.bind_image("aov_color_img", dummy_aov_color_tx_);
  pass.bind_image("aov_value_img", dummy_aov_value_tx_);
  pass.bind_resources(inst_.uniform_data);
  pass.bind_resources(inst_.hiz_buffer.front);
  pass.bind_resources(inst_.reflection_probes);
  pass.bind_resources(inst_.irradiance_cache);
  pass.bind_resources(inst_.shadows);
  pass.bind_resources(inst_.volume.result);
  pass.bind_resources(inst_.cryptomatte);

  pass.draw(geom, res_handle, 0);
}

void LookdevModule::sync_display()
{
  PassSimple &pass = display_ps_;

  const DRWState state = DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_ALWAYS |
                         DRW_STATE_BLEND_ALPHA;
  pass.state_set(state);
  pass.shader_set(inst_.shaders.static_shader_get(LOOKDEV_DISPLAY));
  pass.push_constant("viewportSize", float2(DRW_viewport_size_get()));
  pass.push_constant("invertedViewportSize", float2(DRW_viewport_invert_size_get()));
  pass.push_constant("anchor", int2(visible_rect_.xmax, visible_rect_.ymin));
  pass.bind_texture("metallic_tx", &spheres_[0].color_tx_);
  pass.bind_texture("diffuse_tx", &spheres_[1].color_tx_);

  pass.draw_procedural(GPU_PRIM_TRIS, 2, 6);
}

void LookdevModule::draw(View &view)
{
  if (!enabled_) {
    return;
  }
  for (Sphere &sphere : spheres_) {
    sphere.framebuffer.bind();
    inst_.manager->submit(sphere.pass, view);
  }
}

void LookdevModule::display()
{
  if (!enabled_) {
    return;
  }

  BLI_assert(inst_.is_viewport());

  DefaultFramebufferList *dfbl = DRW_viewport_framebuffer_list_get();
  GPU_framebuffer_bind(dfbl->default_fb);
  inst_.manager->submit(display_ps_);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Parameters
 * \{ */

LookdevParameters::LookdevParameters() {}

LookdevParameters::LookdevParameters(const ::View3D *v3d)
{
  if (v3d == nullptr) {
    return;
  }

  const ::View3DShading &shading = v3d->shading;
  show_scene_world = shading.type == OB_RENDER ? shading.flag & V3D_SHADING_SCENE_WORLD_RENDER :
                                                 shading.flag & V3D_SHADING_SCENE_WORLD;
  if (!show_scene_world) {
    rot_z = shading.studiolight_rot_z;
    background_opacity = shading.studiolight_background;
    blur = shading.studiolight_blur;
    intensity = shading.studiolight_intensity;
    hdri = StringRefNull(shading.lookdev_light);
  }
}

bool LookdevParameters::operator==(const LookdevParameters &other) const
{
  return hdri == other.hdri && rot_z == other.rot_z &&
         background_opacity == other.background_opacity && blur == other.blur &&
         intensity == other.intensity && show_scene_world == other.show_scene_world;
}

bool LookdevParameters::operator!=(const LookdevParameters &other) const
{
  return !(*this == other);
}

/** \} */

}  // namespace blender::eevee
