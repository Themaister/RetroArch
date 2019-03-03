/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2019 - Hans-Kristian Arntzen
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "shader_gl_core.h"
#include "glslang_util.h"

#include <vector>
#include <memory>
#include <functional>
#include <utility>
#include <algorithm>
#include <string.h>

#include <compat/strl.h>
#include <formats/image.h>
#include <retro_miscellaneous.h>

#include "slang_reflection.h"
#include "slang_reflection.hpp"
#include "spirv_glsl.hpp"

#include "../video_driver.h"
#include "../../verbosity.h"
#include "../../msg_hash.h"

using namespace std;

template <typename P>
static bool gl_core_shader_set_unique_map(unordered_map<string, P> &m, const string &name, const P &p)
{
   auto itr = m.find(name);
   if (itr != end(m))
   {
      RARCH_ERR("[slang]: Alias \"%s\" already exists.\n",
            name.c_str());
      return false;
   }

   m[name] = p;
   return true;
}

static GLuint gl_core_compile_shader(GLenum stage, const string &source)
{
   GLuint shader = glCreateShader(stage);

   const char *ptr = source.c_str();
   glShaderSource(shader, 1, &ptr, nullptr);
   glCompileShader(shader);

   GLint status;
   glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
   if (!status)
   {
      GLint length;
      glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
      if (length > 0)
      {
         vector<char> buffer(length + 1);
         glGetShaderInfoLog(shader, length, &length, buffer.data());
         RARCH_ERR("[GLCore]: Failed to compile shader: %s\n", buffer.data());
         glDeleteShader(shader);
         return 0;
      }
   }

   return shader;
}

GLuint gl_core_cross_compile_program(const uint32_t *vertex, size_t vertex_size,
                                     const uint32_t *fragment, size_t fragment_size)
{
   GLuint program = 0;
   try
   {
      spirv_cross::CompilerGLSL vertex_compiler(vertex, vertex_size / 4);
      spirv_cross::CompilerGLSL fragment_compiler(fragment, fragment_size / 4);
      spirv_cross::CompilerGLSL::Options opts;
      opts.es = false;
      opts.version = 150;
      opts.fragment.default_float_precision = spirv_cross::CompilerGLSL::Options::Precision::Highp;
      opts.fragment.default_int_precision = spirv_cross::CompilerGLSL::Options::Precision::Highp;
      opts.enable_420pack_extension = false;
      vertex_compiler.set_common_options(opts);
      fragment_compiler.set_common_options(opts);

      auto vertex_resources = vertex_compiler.get_shader_resources();
      auto fragment_resources = fragment_compiler.get_shader_resources();

      for (auto &res : vertex_resources.stage_inputs)
      {
         uint32_t location = vertex_compiler.get_decoration(res.id, spv::DecorationLocation);
         vertex_compiler.set_name(res.id, string("RARCH_ATTRIBUTE_") + to_string(location));
         vertex_compiler.unset_decoration(res.id, spv::DecorationLocation);
      }

      for (auto &res : vertex_resources.stage_outputs)
      {
         uint32_t location = vertex_compiler.get_decoration(res.id, spv::DecorationLocation);
         vertex_compiler.set_name(res.id, string("RARCH_VARYING_") + to_string(location));
         vertex_compiler.unset_decoration(res.id, spv::DecorationLocation);
      }

      for (auto &res : fragment_resources.stage_inputs)
      {
         uint32_t location = fragment_compiler.get_decoration(res.id, spv::DecorationLocation);
         fragment_compiler.set_name(res.id, string("RARCH_VARYING_") + to_string(location));
         fragment_compiler.unset_decoration(res.id, spv::DecorationLocation);
      }

      if (vertex_resources.push_constant_buffers.size() > 1)
      {
         RARCH_ERR("[GLCore]: Cannot have more than one push constant buffer.\n");
         return 0;
      }

      for (auto &res : vertex_resources.push_constant_buffers)
      {
         vertex_compiler.flatten_buffer_block(res.id);
         vertex_compiler.set_name(res.id, "RARCH_PUSH");
         vertex_compiler.set_name(res.base_type_id, "RARCH_PUSH");
      }

      if (vertex_resources.uniform_buffers.size() > 1)
      {
         RARCH_ERR("[GLCore]: Cannot have more than one uniform buffer.\n");
         return 0;
      }

      for (auto &res : vertex_resources.uniform_buffers)
      {
         vertex_compiler.flatten_buffer_block(res.id);
         vertex_compiler.set_name(res.id, "RARCH_UBO");
         vertex_compiler.set_name(res.base_type_id, "RARCH_UBO");
         vertex_compiler.unset_decoration(res.id, spv::DecorationDescriptorSet);
         vertex_compiler.unset_decoration(res.id, spv::DecorationBinding);
      }

      if (fragment_resources.push_constant_buffers.size() > 1)
      {
         RARCH_ERR("[GLCore]: Cannot have more than one push constant block.\n");
         return 0;
      }

      for (auto &res : fragment_resources.push_constant_buffers)
      {
         fragment_compiler.flatten_buffer_block(res.id);
         fragment_compiler.set_name(res.id, "RARCH_PUSH");
         fragment_compiler.set_name(res.base_type_id, "RARCH_PUSH");
      }

      if (fragment_resources.uniform_buffers.size() > 1)
      {
         RARCH_ERR("[GLCore]: Cannot have more than one uniform buffer.\n");
         return 0;
      }

      for (auto &res : fragment_resources.uniform_buffers)
      {
         fragment_compiler.flatten_buffer_block(res.id);
         fragment_compiler.set_name(res.id, "RARCH_UBO");
         fragment_compiler.set_name(res.base_type_id, "RARCH_UBO");
         fragment_compiler.unset_decoration(res.id, spv::DecorationDescriptorSet);
         fragment_compiler.unset_decoration(res.id, spv::DecorationBinding);
      }

      std::vector<uint32_t> texture_binding_fixups;
      for (auto &res : fragment_resources.sampled_images)
      {
         uint32_t binding = fragment_compiler.get_decoration(res.id, spv::DecorationBinding);
         fragment_compiler.set_name(res.id, string("RARCH_TEXTURE_") + to_string(binding));
         fragment_compiler.unset_decoration(res.id, spv::DecorationDescriptorSet);
         fragment_compiler.unset_decoration(res.id, spv::DecorationBinding);
         texture_binding_fixups.push_back(binding);
      }

      auto vertex_source = vertex_compiler.compile();
      auto fragment_source = fragment_compiler.compile();
      GLuint vertex_shader = gl_core_compile_shader(GL_VERTEX_SHADER, vertex_source);
      GLuint fragment_shader = gl_core_compile_shader(GL_FRAGMENT_SHADER, fragment_source);

      if (!vertex_shader || !fragment_shader)
      {
         RARCH_ERR("[GLCore]: One or more shaders failed to compile.\n");
         if (vertex_shader)
            glDeleteShader(vertex_shader);
         if (fragment_shader)
            glDeleteShader(fragment_shader);
         return 0;
      }

      program = glCreateProgram();
      glAttachShader(program, vertex_shader);
      glAttachShader(program, fragment_shader);
      for (auto &res : vertex_resources.stage_inputs)
      {
         uint32_t location = vertex_compiler.get_decoration(res.id, spv::DecorationLocation);
         glBindAttribLocation(program, location, (string("RARCH_ATTRIBUTE_") + to_string(location)).c_str());
      }
      glLinkProgram(program);
      glDeleteShader(vertex_shader);
      glDeleteShader(fragment_shader);

      GLint status;
      glGetProgramiv(program, GL_LINK_STATUS, &status);
      if (!status)
      {
         GLint length;
         glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
         if (length > 0)
         {
            vector<char> buffer(length + 1);
            glGetProgramInfoLog(program, length, &length, buffer.data());
            RARCH_ERR("[GLCore]: Failed to link program: %s\n", buffer.data());
            glDeleteProgram(program);
            return 0;
         }
      }

      glUseProgram(program);

      // Force proper bindings for textures.
      for (auto &binding : texture_binding_fixups)
      {
         GLint location = glGetUniformLocation(program, (string("RARCH_TEXTURE_") + to_string(binding)).c_str());
         if (location >= 0)
            glUniform1i(location, binding);
      }

      glUseProgram(0);
   }
   catch (const exception &e)
   {
      RARCH_ERR("[GLCore]: Failed to cross compile program: %s\n", e.what());
      if (program != 0)
         glDeleteProgram(program);
      return 0;
   }

   return program;
}

namespace gl_core
{
static const uint32_t opaque_vert[] =
#include "../drivers/vulkan_shaders/opaque.vert.inc"
;

static const uint32_t opaque_frag[] =
#include "../drivers/vulkan_shaders/opaque.frag.inc"
;

struct ConfigDeleter
{
   void operator()(config_file_t *conf)
   {
      if (conf)
         config_file_free(conf);
   }
};

static unsigned num_miplevels(unsigned width, unsigned height)
{
   unsigned size = MAX(width, height);
   unsigned levels = 0;
   while (size) {
      levels++;
      size >>= 1;
   }
   return levels;
}

static void build_identity_matrix(float *data)
{
   data[0] = 1.0f;
   data[1] = 0.0f;
   data[2] = 0.0f;
   data[3] = 0.0f;
   data[4] = 0.0f;
   data[5] = 1.0f;
   data[6] = 0.0f;
   data[7] = 0.0f;
   data[8] = 0.0f;
   data[9] = 0.0f;
   data[10] = 1.0f;
   data[11] = 0.0f;
   data[12] = 0.0f;
   data[13] = 0.0f;
   data[14] = 0.0f;
   data[15] = 1.0f;
}

static void build_vec4(float *data, unsigned width, unsigned height)
{
   data[0] = float(width);
   data[1] = float(height);
   data[2] = 1.0f / float(width);
   data[3] = 1.0f / float(height);
}

struct Size2D
{
   unsigned width, height;
};

struct Texture
{
   gl_core_filter_chain_texture texture;
   gl_core_filter_chain_filter filter;
   gl_core_filter_chain_filter mip_filter;
   gl_core_filter_chain_address address;
};

static gl_core_filter_chain_address wrap_to_address(gfx_wrap_type type)
{
   switch (type)
   {
      default:
      case RARCH_WRAP_EDGE:
         return GL_CORE_FILTER_CHAIN_ADDRESS_CLAMP_TO_EDGE;

      case RARCH_WRAP_BORDER:
         return GL_CORE_FILTER_CHAIN_ADDRESS_CLAMP_TO_BORDER;

      case RARCH_WRAP_REPEAT:
         return GL_CORE_FILTER_CHAIN_ADDRESS_REPEAT;

      case RARCH_WRAP_MIRRORED_REPEAT:
         return GL_CORE_FILTER_CHAIN_ADDRESS_MIRRORED_REPEAT;
   }
}

static GLenum address_to_gl(gl_core_filter_chain_address type)
{
   switch (type)
   {
      default:
      case GL_CORE_FILTER_CHAIN_ADDRESS_CLAMP_TO_EDGE:
         return GL_CLAMP_TO_EDGE;

      case GL_CORE_FILTER_CHAIN_ADDRESS_CLAMP_TO_BORDER:
         return GL_CLAMP_TO_BORDER;

      case GL_CORE_FILTER_CHAIN_ADDRESS_REPEAT:
         return GL_REPEAT;

      case GL_CORE_FILTER_CHAIN_ADDRESS_MIRRORED_REPEAT:
         return GL_MIRRORED_REPEAT;
   }
}

static GLenum convert_filter_to_mag_gl(gl_core_filter_chain_filter filter)
{
   switch (filter)
   {
      case GL_CORE_FILTER_CHAIN_LINEAR:
         return GL_LINEAR;

      default:
      case GL_CORE_FILTER_CHAIN_NEAREST:
         return GL_NEAREST;
   }
}

static GLenum convert_filter_to_min_gl(gl_core_filter_chain_filter filter, gl_core_filter_chain_filter mipfilter)
{
   if (filter == GL_CORE_FILTER_CHAIN_LINEAR && mipfilter == GL_CORE_FILTER_CHAIN_LINEAR)
      return GL_LINEAR_MIPMAP_LINEAR;
   else if (filter == GL_CORE_FILTER_CHAIN_LINEAR)
      return GL_LINEAR_MIPMAP_NEAREST;
   else if (mipfilter == GL_CORE_FILTER_CHAIN_LINEAR)
      return GL_NEAREST_MIPMAP_LINEAR;
   else
      return GL_NEAREST_MIPMAP_NEAREST;
}

static GLenum convert_glslang_format(glslang_format fmt)
{
#undef FMT
#define FMT(x, r) case SLANG_FORMAT_##x: return GL_##r
   switch (fmt)
   {
      FMT(R8_UNORM, R8);
      FMT(R8_SINT, R8I);
      FMT(R8_UINT, R8UI);
      FMT(R8G8_UNORM, RG8);
      FMT(R8G8_SINT, RG8I);
      FMT(R8G8_UINT, RG8UI);
      FMT(R8G8B8A8_UNORM, RGBA8);
      FMT(R8G8B8A8_SINT, RGBA8I);
      FMT(R8G8B8A8_UINT, RGBA8UI);
      FMT(R8G8B8A8_SRGB, SRGB8_ALPHA8);

      FMT(A2B10G10R10_UNORM_PACK32, RGB10_A2);
      FMT(A2B10G10R10_UINT_PACK32, RGB10_A2UI);

      FMT(R16_UINT, R16UI);
      FMT(R16_SINT, R16I);
      FMT(R16_SFLOAT, R16F);
      FMT(R16G16_UINT, RG16UI);
      FMT(R16G16_SINT, RG16I);
      FMT(R16G16_SFLOAT, RG16F);
      FMT(R16G16B16A16_UINT, RGBA16UI);
      FMT(R16G16B16A16_SINT, RGBA16I);
      FMT(R16G16B16A16_SFLOAT, RGBA16F);

      FMT(R32_UINT, R32UI);
      FMT(R32_SINT, R32I);
      FMT(R32_SFLOAT, R32F);
      FMT(R32G32_UINT, RG32UI);
      FMT(R32G32_SINT, RG32I);
      FMT(R32G32_SFLOAT, RG32F);
      FMT(R32G32B32A32_UINT, RGBA32UI);
      FMT(R32G32B32A32_SINT, RGBA32I);
      FMT(R32G32B32A32_SFLOAT, RGBA32F);

      default:
         return 0;
   }
}

class StaticTexture
{
public:
   StaticTexture(string id,
                 GLuint image,
                 unsigned width, unsigned height,
                 bool linear,
                 bool mipmap,
                 GLenum address);
   ~StaticTexture();

   StaticTexture(StaticTexture&&) = delete;
   void operator=(StaticTexture&&) = delete;

   void set_id(string name)
   {
      id = move(name);
   }

   const string &get_id() const
   {
      return id;
   }

   const Texture &get_texture() const
   {
      return texture;
   }

private:
   string id;
   GLuint image;
   Texture texture;
};

StaticTexture::StaticTexture(string id_, GLuint image_, unsigned width, unsigned height, bool linear, bool mipmap,
                             GLenum address)
   : id(std::move(id_)), image(image_)
{
   texture.texture.width = width;
   texture.texture.height = height;
   texture.texture.format = 0;
   texture.texture.image = image;

   glBindTexture(GL_TEXTURE_2D, image);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, address);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, address);

   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
   if (linear && mipmap)
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
   else if (linear)
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   else
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

   glBindTexture(GL_TEXTURE_2D, 0);
}

StaticTexture::~StaticTexture()
{
   if (image != 0)
      glDeleteTextures(1, &image);
}

struct CommonResources
{
   CommonResources();
   ~CommonResources();

   vector<Texture> original_history;
   vector<Texture> framebuffer_feedback;
   vector<Texture> pass_outputs;
   vector<unique_ptr<StaticTexture>> luts;

   unordered_map<string, slang_texture_semantic_map> texture_semantic_map;
   unordered_map<string, slang_texture_semantic_map> texture_semantic_uniform_map;
   unique_ptr<video_shader> shader_preset;

   GLuint quad_program = 0;
   GLuint quad_vbo = 0;
   GLint quad_ubo_index = 0;
   void draw_quad(GLuint program) const;
};

CommonResources::CommonResources()
{
   static float quad_data[] = {
      -1.0f, -1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
      +1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f,
      -1.0f, +1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f,
      +1.0f, +1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f,
   };

   glGenBuffers(1, &quad_vbo);
   glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
   glBufferData(GL_ARRAY_BUFFER, sizeof(quad_data), quad_data, GL_STATIC_DRAW);
   glBindBuffer(GL_ARRAY_BUFFER, 0);

   quad_program = gl_core_cross_compile_program(opaque_vert, sizeof(opaque_vert), opaque_frag, sizeof(opaque_frag));
   quad_ubo_index = glGetUniformLocation(quad_program, "RARCH_UBO");
}

CommonResources::~CommonResources()
{
   if (quad_program != 0)
      glDeleteProgram(quad_program);
   if (quad_vbo != 0)
      glDeleteBuffers(1, &quad_vbo);
}

void CommonResources::draw_quad(GLuint program) const
{
   glUseProgram(program);

   if (quad_ubo_index >= 0)
   {
      float mvp[16];
      build_identity_matrix(mvp);
      glUniform4fv(quad_ubo_index, 4, mvp);
   }

   glDisable(GL_CULL_FACE);
   glDisable(GL_BLEND);
   glDisable(GL_DEPTH_TEST);
   glEnableVertexAttribArray(0);
   glEnableVertexAttribArray(1);
   glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
   glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 8 * sizeof(float), reinterpret_cast<void *>(uintptr_t(0)));
   glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 8 * sizeof(float), reinterpret_cast<void *>(uintptr_t(4 * sizeof(float))));
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
   glBindBuffer(GL_ARRAY_BUFFER, 0);
   glDisableVertexAttribArray(0);
   glDisableVertexAttribArray(1);
   glUseProgram(0);
}

class Framebuffer
{
public:
   Framebuffer(GLenum format, unsigned max_levels);

   ~Framebuffer();
   Framebuffer(Framebuffer&&) = delete;
   void operator=(Framebuffer&&) = delete;

   void set_size(const Size2D &size, GLenum format = 0);

   const Size2D &get_size() const { return size; }
   GLenum get_format() const { return format; }
   GLuint get_image() const { return image; }
   GLuint get_framebuffer() const { return framebuffer; }

   void clear();
   void copy(const CommonResources &common, GLuint image);

   unsigned get_levels() const { return levels; }
   void generate_mips();

private:
   GLuint image = 0;
   Size2D size;
   GLenum format;
   unsigned max_levels;
   unsigned levels = 0;

   GLuint framebuffer = 0;

   void init();
   bool complete = false;
};

Framebuffer::Framebuffer(GLenum format_, unsigned max_levels_)
   : size({1, 1}), format(format_), max_levels(max_levels_)
{
   glGenFramebuffers(1, &framebuffer);

   // Need to bind to create.
   glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
   glBindFramebuffer(GL_FRAMEBUFFER, 0);

   if (format == 0)
      format = GL_RGBA8;
}

void Framebuffer::set_size(const Size2D &size_, GLenum format_)
{
   size = size_;
   if (format_ != 0)
      format = format_;

   init();
}

void Framebuffer::init()
{
   glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
   if (image != 0)
   {
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
      glDeleteTextures(1, &image);
   }

   glGenTextures(1, &image);
   glBindTexture(GL_TEXTURE_2D, image);
   glTexStorage2D(GL_TEXTURE_2D, std::min(max_levels, num_miplevels(size.width, size.height)),
                  format,
                  size.width, size.height);

   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, image, 0);
   auto status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
   bool fallback = false;

   if (status != GL_FRAMEBUFFER_COMPLETE)
   {
      switch (status)
      {
         case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
            RARCH_ERR("[GLCore]: Incomplete attachment.\n");
            break;

         case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
            RARCH_ERR("[GLCore]: Incomplete, missing attachment.\n");
            break;

         case GL_FRAMEBUFFER_UNSUPPORTED:
            RARCH_ERR("[GLCore]: Unsupported FBO, falling back to RGBA8.\n");
            fallback = true;
            break;
      }

      complete = false;
   }
   else
      complete = true;

   if (fallback)
   {
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
      glDeleteTextures(1, &image);
      glGenTextures(1, &image);
      glBindTexture(GL_TEXTURE_2D, image);
      glTexStorage2D(GL_TEXTURE_2D, std::min(max_levels, num_miplevels(size.width, size.height)),
                     GL_RGBA8,
                     size.width, size.height);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, image, 0);
      status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
      complete = status == GL_FRAMEBUFFER_COMPLETE;
   }

   glBindFramebuffer(GL_FRAMEBUFFER, 0);
   glBindTexture(GL_TEXTURE_2D, 0);
}

void Framebuffer::clear()
{
   if (complete)
   {
      glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
      glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
      glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
      glClear(GL_COLOR_BUFFER_BIT);
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
   }
}

void Framebuffer::generate_mips()
{
   glBindTexture(GL_TEXTURE_2D, image);
   glGenerateMipmap(GL_TEXTURE_2D);
   glBindTexture(GL_TEXTURE_2D, 0);
}

void Framebuffer::copy(const CommonResources &common, GLuint image)
{
   if (!complete)
      return;

   glBindFramebuffer(GL_FRAMEBUFFER, image);
   glActiveTexture(GL_TEXTURE0);
   glBindTexture(GL_TEXTURE_2D, image);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   glViewport(0, 0, size.width, size.height);
   glClear(GL_COLOR_BUFFER_BIT);
   common.draw_quad(common.quad_program);
   glBindTexture(GL_TEXTURE_2D, 0);
   glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

Framebuffer::~Framebuffer()
{
   if (framebuffer != 0)
      glDeleteFramebuffers(1, &framebuffer);
   if (image != 0)
      glDeleteTextures(1, &image);
}

class Pass
{
public:
   explicit Pass(bool final_pass) :
         final_pass(final_pass)
   {}

   ~Pass();

   Pass(Pass&&) = delete;
   void operator=(Pass&&) = delete;

   const Framebuffer &get_framebuffer() const
   {
      return *framebuffer;
   }

   Framebuffer *get_feedback_framebuffer()
   {
      return framebuffer_feedback.get();
   }

   void set_pass_info(const gl_core_filter_chain_pass_info &info);

   void set_shader(GLenum stage,
                   const uint32_t *spirv,
                   size_t spirv_words);

   bool build();
   bool init_feedback();

   void build_commands(
         const Texture &original,
         const Texture &source,
         const gl_core_viewport &vp,
         const float *mvp);

   void set_frame_count(uint64_t count)
   {
      frame_count = count;
   }

   void set_frame_count_period(unsigned period)
   {
      frame_count_period = period;
   }

   void set_name(const char *name)
   {
      pass_name = name;
   }

   const string &get_name() const
   {
      return pass_name;
   }

   gl_core_filter_chain_filter get_source_filter() const
   {
      return pass_info.source_filter;
   }

   gl_core_filter_chain_filter get_mip_filter() const
   {
      return pass_info.mip_filter;
   }

   gl_core_filter_chain_address get_address_mode() const
   {
      return pass_info.address;
   }

   void set_common_resources(CommonResources *common)
   {
      this->common = common;
   }

   const slang_reflection &get_reflection() const
   {
      return reflection;
   }

   void set_pass_number(unsigned pass)
   {
      pass_number = pass;
   }

   void add_parameter(unsigned parameter_index, const std::string &id);

   void end_frame();
   void allocate_buffers();

private:
   bool final_pass;

   Size2D get_output_size(const Size2D &original_size,
                          const Size2D &max_source) const;

   GLuint pipeline = 0;
   CommonResources *common = nullptr;

   Size2D current_framebuffer_size = {};
   gl_core_viewport current_viewport;
   gl_core_filter_chain_pass_info pass_info;

   vector<uint32_t> vertex_shader;
   vector<uint32_t> fragment_shader;
   unique_ptr<Framebuffer> framebuffer;
   unique_ptr<Framebuffer> framebuffer_feedback;

   bool init_pipeline();

   void set_texture(unsigned binding,
                    const Texture &texture);

   void set_semantic_texture(slang_texture_semantic semantic,
                             const Texture &texture);
   void set_semantic_texture_array(slang_texture_semantic semantic, unsigned index,
                                   const Texture &texture);

   void set_uniform_buffer(unsigned binding, const void *data, size_t range);

   slang_reflection reflection;

   std::vector<uint8_t> uniforms;
   GLint uniforms_location = -1;

   void build_semantics(uint8_t *buffer,
                        const float *mvp, const Texture &original, const Texture &source);
   void build_semantic_vec4(uint8_t *data, slang_semantic semantic,
                            unsigned width, unsigned height);
   void build_semantic_uint(uint8_t *data, slang_semantic semantic, uint32_t value);
   void build_semantic_parameter(uint8_t *data, unsigned index, float value);
   void build_semantic_texture_vec4(uint8_t *data,
                                    slang_texture_semantic semantic,
                                    unsigned width, unsigned height);
   void build_semantic_texture_array_vec4(uint8_t *data,
                                          slang_texture_semantic semantic, unsigned index,
                                          unsigned width, unsigned height);
   void build_semantic_texture(uint8_t *buffer,
                               slang_texture_semantic semantic, const Texture &texture);
   void build_semantic_texture_array(uint8_t *buffer,
                                     slang_texture_semantic semantic, unsigned index, const Texture &texture);

   uint64_t frame_count = 0;
   unsigned frame_count_period = 0;
   unsigned pass_number = 0;

   size_t ubo_offset = 0;
   string pass_name;

   struct Parameter
   {
      string id;
      unsigned index;
      unsigned semantic_index;
   };

   vector<Parameter> parameters;
   vector<Parameter> filtered_parameters;
   vector<uint32_t> push_constant_buffer;
   GLint push_constant_buffer_location = -1;
};

bool Pass::build()
{
   unordered_map<string, slang_semantic_map> semantic_map;
   unsigned i;
   unsigned j = 0;

   framebuffer.reset();
   framebuffer_feedback.reset();

   if (!final_pass)
   {
      framebuffer = unique_ptr<Framebuffer>(
            new Framebuffer(pass_info.rt_format, pass_info.max_levels));
   }

   for (auto &param : parameters)
   {
      if (!gl_core_shader_set_unique_map(semantic_map, param.id,
               slang_semantic_map{ SLANG_SEMANTIC_FLOAT_PARAMETER, j }))
         return false;
      j++;
   }

   reflection                              = slang_reflection{};
   reflection.pass_number                  = pass_number;
   reflection.texture_semantic_map         = &common->texture_semantic_map;
   reflection.texture_semantic_uniform_map = &common->texture_semantic_uniform_map;
   reflection.semantic_map                 = &semantic_map;

   if (!slang_reflect_spirv(vertex_shader, fragment_shader, &reflection))
      return false;

   /* Filter out parameters which we will never use anyways. */
   filtered_parameters.clear();

   for (i = 0; i < reflection.semantic_float_parameters.size(); i++)
   {
      if (reflection.semantic_float_parameters[i].uniform ||
          reflection.semantic_float_parameters[i].push_constant)
         filtered_parameters.push_back(parameters[i]);
   }

   if (!init_pipeline())
      return false;

   return true;
}

bool Pass::init_pipeline()
{
   pipeline = gl_core_cross_compile_program(vertex_shader.data(), vertex_shader.size() * sizeof(uint32_t),
                                            fragment_shader.data(), fragment_shader.size() * sizeof(uint32_t));

   if (!pipeline)
      return false;

   uniforms_location = glGetUniformLocation(pipeline, "RARCH_UBO");
   push_constant_buffer_location = glGetUniformLocation(pipeline, "RARCH_PUSH");
   uniforms.resize(reflection.ubo_size);
   push_constant_buffer.resize(reflection.push_constant_size);
   return true;
}

void Pass::set_pass_info(const gl_core_filter_chain_pass_info &info)
{
   pass_info = info;
}

Size2D Pass::get_output_size(const Size2D &original, const Size2D &source) const
{
   float width, height;
   switch (pass_info.scale_type_x)
   {
      case GL_CORE_FILTER_CHAIN_SCALE_ORIGINAL:
         width = float(original.width) * pass_info.scale_x;
         break;

      case GL_CORE_FILTER_CHAIN_SCALE_SOURCE:
         width = float(source.width) * pass_info.scale_x;
         break;

      case GL_CORE_FILTER_CHAIN_SCALE_VIEWPORT:
         width = current_viewport.width * pass_info.scale_x;
         break;

      case GL_CORE_FILTER_CHAIN_SCALE_ABSOLUTE:
         width = pass_info.scale_x;
         break;

      default:
         width = 0.0f;
   }

   switch (pass_info.scale_type_y)
   {
      case GL_CORE_FILTER_CHAIN_SCALE_ORIGINAL:
         height = float(original.height) * pass_info.scale_y;
         break;

      case GL_CORE_FILTER_CHAIN_SCALE_SOURCE:
         height = float(source.height) * pass_info.scale_y;
         break;

      case GL_CORE_FILTER_CHAIN_SCALE_VIEWPORT:
         height = current_viewport.height * pass_info.scale_y;
         break;

      case GL_CORE_FILTER_CHAIN_SCALE_ABSOLUTE:
         height = pass_info.scale_y;
         break;

      default:
         height = 0.0f;
   }

   return { unsigned(roundf(width)), unsigned(roundf(height)) };
}

void Pass::end_frame()
{
   if (framebuffer_feedback)
      swap(framebuffer, framebuffer_feedback);
}

void Pass::build_semantic_vec4(uint8_t *data, slang_semantic semantic,
      unsigned width, unsigned height)
{
   auto &refl = reflection.semantics[semantic];

   if (data && refl.uniform)
      build_vec4(
            reinterpret_cast<float *>(data + refl.ubo_offset),
            width,
            height);

   if (refl.push_constant)
      build_vec4(
            reinterpret_cast<float *>(push_constant_buffer.data() + (refl.push_constant_offset >> 2)),
            width,
            height);
}

void Pass::build_semantic_parameter(uint8_t *data, unsigned index, float value)
{
   auto &refl = reflection.semantic_float_parameters[index];

   /* We will have filtered out stale parameters. */
   if (data && refl.uniform)
      *reinterpret_cast<float*>(data + refl.ubo_offset) = value;

   if (refl.push_constant)
      *reinterpret_cast<float*>(push_constant_buffer.data() + (refl.push_constant_offset >> 2)) = value;
}

void Pass::build_semantic_uint(uint8_t *data, slang_semantic semantic,
                               uint32_t value)
{
   auto &refl = reflection.semantics[semantic];

   if (data && refl.uniform)
      *reinterpret_cast<uint32_t*>(data + reflection.semantics[semantic].ubo_offset) = value;

   if (refl.push_constant)
      *reinterpret_cast<uint32_t*>(push_constant_buffer.data() + (refl.push_constant_offset >> 2)) = value;
}

void Pass::build_semantic_texture(uint8_t *buffer,
      slang_texture_semantic semantic, const Texture &texture)
{
   build_semantic_texture_vec4(buffer, semantic,
         texture.texture.width, texture.texture.height);
   set_semantic_texture(semantic, texture);
}

void Pass::set_semantic_texture_array(
      slang_texture_semantic semantic, unsigned index,
      const Texture &texture)
{
   if (index < reflection.semantic_textures[semantic].size() &&
         reflection.semantic_textures[semantic][index].texture)
      set_texture(reflection.semantic_textures[semantic][index].binding, texture);
}

void Pass::build_semantic_texture_array_vec4(uint8_t *data, slang_texture_semantic semantic,
      unsigned index, unsigned width, unsigned height)
{
   auto &refl = reflection.semantic_textures[semantic];
   if (index >= refl.size())
      return;

   if (data && refl[index].uniform)
      build_vec4(
            reinterpret_cast<float *>(data + refl[index].ubo_offset),
            width,
            height);

   if (refl[index].push_constant)
      build_vec4(
            reinterpret_cast<float *>(push_constant_buffer.data() + (refl[index].push_constant_offset >> 2)),
            width,
            height);
}

void Pass::build_semantic_texture_vec4(uint8_t *data, slang_texture_semantic semantic,
      unsigned width, unsigned height)
{
   build_semantic_texture_array_vec4(data, semantic, 0, width, height);
}

bool Pass::init_feedback()
{
   if (final_pass)
      return false;

   framebuffer_feedback = unique_ptr<Framebuffer>(
         new Framebuffer(pass_info.rt_format, pass_info.max_levels));
   return true;
}

Pass::~Pass()
{
   if (pipeline != 0)
      glDeleteProgram(pipeline);
}

void Pass::set_shader(GLenum stage,
      const uint32_t *spirv,
      size_t spirv_words)
{
   if (stage == GL_VERTEX_SHADER)
   {
      vertex_shader.clear();
      vertex_shader.insert(end(vertex_shader),
            spirv, spirv + spirv_words);
   }
   else if (stage == GL_FRAGMENT_SHADER)
   {
      fragment_shader.clear();
      fragment_shader.insert(end(fragment_shader),
            spirv, spirv + spirv_words);
   }
}

void Pass::add_parameter(unsigned index, const std::string &id)
{
   parameters.push_back({ id, index, unsigned(parameters.size()) });
}

void Pass::set_texture(unsigned binding, const Texture &texture)
{
   glActiveTexture(GL_TEXTURE0 + binding);
   glBindTexture(GL_TEXTURE_2D, texture.texture.image);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, convert_filter_to_mag_gl(texture.filter));
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, convert_filter_to_min_gl(texture.filter, texture.mip_filter));
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, address_to_gl(texture.address));
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, address_to_gl(texture.address));
}

void Pass::set_semantic_texture(slang_texture_semantic semantic, const Texture &texture)
{
   if (reflection.semantic_textures[semantic][0].texture)
      set_texture(reflection.semantic_textures[semantic][0].binding, texture);
}

void Pass::build_semantic_texture_array(uint8_t *buffer,
      slang_texture_semantic semantic, unsigned index, const Texture &texture)
{
   build_semantic_texture_array_vec4(buffer, semantic, index,
         texture.texture.width, texture.texture.height);
   set_semantic_texture_array(semantic, index, texture);
}

void Pass::build_semantics(uint8_t *buffer,
                           const float *mvp, const Texture &original, const Texture &source)
{
   /* MVP */
   if (buffer && reflection.semantics[SLANG_SEMANTIC_MVP].uniform)
   {
      size_t offset = reflection.semantics[SLANG_SEMANTIC_MVP].ubo_offset;
      if (mvp)
         memcpy(buffer + offset, mvp, sizeof(float) * 16);
      else
         build_identity_matrix(reinterpret_cast<float *>(buffer + offset));
   }

   if (reflection.semantics[SLANG_SEMANTIC_MVP].push_constant)
   {
      size_t offset = reflection.semantics[SLANG_SEMANTIC_MVP].push_constant_offset;
      if (mvp)
         memcpy(push_constant_buffer.data() + (offset >> 2), mvp, sizeof(float) * 16);
      else
         build_identity_matrix(reinterpret_cast<float *>(push_constant_buffer.data() + (offset >> 2)));
   }

   /* Output information */
   build_semantic_vec4(buffer, SLANG_SEMANTIC_OUTPUT,
                       current_framebuffer_size.width, current_framebuffer_size.height);
   build_semantic_vec4(buffer, SLANG_SEMANTIC_FINAL_VIEWPORT,
                       unsigned(current_viewport.width), unsigned(current_viewport.height));

   build_semantic_uint(buffer, SLANG_SEMANTIC_FRAME_COUNT,
                       frame_count_period ? uint32_t(frame_count % frame_count_period) : uint32_t(frame_count));

   /* Standard inputs */
   build_semantic_texture(buffer, SLANG_TEXTURE_SEMANTIC_ORIGINAL, original);
   build_semantic_texture(buffer, SLANG_TEXTURE_SEMANTIC_SOURCE, source);

   /* ORIGINAL_HISTORY[0] is an alias of ORIGINAL. */
   build_semantic_texture_array(buffer, SLANG_TEXTURE_SEMANTIC_ORIGINAL_HISTORY, 0, original);

   /* Parameters. */
   for (auto &param : filtered_parameters)
   {
      float value = common->shader_preset->parameters[param.index].current;
      build_semantic_parameter(buffer, param.semantic_index, value);
   }

   /* Previous inputs. */
   unsigned i = 0;
   for (auto &texture : common->original_history)
   {
      build_semantic_texture_array(buffer,
                                   SLANG_TEXTURE_SEMANTIC_ORIGINAL_HISTORY, i + 1,
                                   texture);
      i++;
   }

   /* Previous passes. */
   i = 0;
   for (auto &texture : common->pass_outputs)
   {
      build_semantic_texture_array(buffer,
                                   SLANG_TEXTURE_SEMANTIC_PASS_OUTPUT, i,
                                   texture);
      i++;
   }

   /* Feedback FBOs. */
   i = 0;
   for (auto &texture : common->framebuffer_feedback)
   {
      build_semantic_texture_array(buffer,
                                   SLANG_TEXTURE_SEMANTIC_PASS_FEEDBACK, i,
                                   texture);
      i++;
   }

   /* LUTs. */
   i = 0;
   for (auto &lut : common->luts)
   {
      build_semantic_texture_array(buffer,
                                   SLANG_TEXTURE_SEMANTIC_USER, i,
                                   lut->get_texture());
      i++;
   }
}

void Pass::build_commands(
      const Texture &original,
      const Texture &source,
      const gl_core_viewport &vp,
      const float *mvp)
{
   current_viewport = vp;
   auto size = get_output_size(
         { original.texture.width, original.texture.height },
         { source.texture.width, source.texture.height });

   if (framebuffer &&
       (size.width  != framebuffer->get_size().width ||
        size.height != framebuffer->get_size().height))
   {
      framebuffer->set_size(size);
   }
   current_framebuffer_size = size;

   build_semantics(uniforms.data(), mvp, original, source);

   glUseProgram(pipeline);

   if (uniforms_location >= 0)
   {
      glUniform4fv(uniforms_location,
                   GLsizei((reflection.ubo_size + 15) / 16),
                   reinterpret_cast<const float *>(uniforms.data()));
   }

   if (push_constant_buffer_location >= 0)
   {
      glUniform4fv(push_constant_buffer_location,
                   GLsizei((reflection.push_constant_size + 15) / 16),
                   reinterpret_cast<const float *>(push_constant_buffer.data()));
   }

   /* The final pass is always executed inside
    * another render pass since the frontend will
    * want to overlay various things on top for
    * the passes that end up on-screen. */
   if (!final_pass)
   {
      glBindFramebuffer(GL_FRAMEBUFFER, framebuffer->get_framebuffer());
      glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
      glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
      glClear(GL_COLOR_BUFFER_BIT);
   }

   if (final_pass)
   {
      glViewport(current_viewport.x, current_viewport.y,
                 current_viewport.width, current_viewport.height);
   }
   else
   {
      glViewport(0, 0, current_viewport.width, current_viewport.height);
   }

   if (framebuffer->get_format() == GL_SRGB8_ALPHA8)
      glEnable(GL_FRAMEBUFFER_SRGB);
   else
      glDisable(GL_FRAMEBUFFER_SRGB);

   common->draw_quad(pipeline);
   glDisable(GL_FRAMEBUFFER_SRGB);

   glBindFramebuffer(GL_FRAMEBUFFER, 0);

   if (!final_pass)
   {
      if (framebuffer->get_levels() > 1)
         framebuffer->generate_mips();
   }
}

}

struct gl_core_filter_chain
{
public:
   inline void set_shader_preset(unique_ptr<video_shader> shader)
   {
      common.shader_preset = move(shader);
   }

   inline video_shader *get_shader_preset()
   {
      return common.shader_preset.get();
   }

   void set_pass_info(unsigned pass,
                      const gl_core_filter_chain_pass_info &info);
   void set_shader(unsigned pass, GLenum stage,
                   const uint32_t *spirv, size_t spirv_words);

   bool init();

   void set_input_texture(const gl_core_filter_chain_texture &texture);
   void build_offscreen_passes(const gl_core_viewport &vp);
   void build_viewport_pass(const gl_core_viewport &vp, const float *mvp);
   void end_frame();

   void set_frame_count(uint64_t count);
   void set_frame_count_period(unsigned pass, unsigned period);
   void set_pass_name(unsigned pass, const char *name);

   void add_static_texture(unique_ptr<gl_core::StaticTexture> texture);
   void add_parameter(unsigned pass, unsigned parameter_index, const std::string &id);

private:
   vector<unique_ptr<gl_core::Pass>> passes;
   vector<gl_core_filter_chain_pass_info> pass_info;
   vector<vector<function<void ()>>> deferred_calls;
   gl_core::CommonResources common;

   gl_core_filter_chain_texture input_texture = {};

   bool init_history();
   bool init_feedback();
   bool init_alias();
   vector<unique_ptr<gl_core::Framebuffer>> original_history;
   void update_history();
   bool require_clear = false;
   void clear_history_and_feedback();
   void update_feedback_info();
   void update_history_info();
};

void gl_core_filter_chain::clear_history_and_feedback()
{
   for (auto &texture : original_history)
      texture->clear();
   for (auto &pass : passes)
   {
      auto *fb = pass->get_feedback_framebuffer();
      if (fb)
         fb->clear();
   }
}

void gl_core_filter_chain::update_history_info()
{
   unsigned i = 0;
   for (auto &texture : original_history)
   {
      auto &source          = common.original_history[i];
      source.texture.image  = texture->get_image();
      source.texture.width  = texture->get_size().width;
      source.texture.height = texture->get_size().height;
      source.filter         = passes.front()->get_source_filter();
      source.mip_filter     = passes.front()->get_mip_filter();
      source.address        = passes.front()->get_address_mode();
      i++;
   }
}

void gl_core_filter_chain::update_feedback_info()
{
   if (common.framebuffer_feedback.empty())
      return;

   for (unsigned i = 0; i < passes.size() - 1; i++)
   {
      auto fb = passes[i]->get_feedback_framebuffer();
      if (!fb)
         continue;

      auto &source          = common.framebuffer_feedback[i];
      source.texture.image  = fb->get_image();
      source.texture.width  = fb->get_size().width;
      source.texture.height = fb->get_size().height;
      source.filter         = passes[i]->get_source_filter();
      source.mip_filter     = passes[i]->get_mip_filter();
      source.address        = passes[i]->get_address_mode();
   }
}

void gl_core_filter_chain::build_offscreen_passes(const gl_core_viewport &vp)
{
   /* First frame, make sure our history and feedback textures are in a clean state. */
   if (require_clear)
   {
      clear_history_and_feedback();
      require_clear = false;
   }

   update_history_info();
   update_feedback_info();

   const gl_core::Texture original = {
         input_texture,
         passes.front()->get_source_filter(),
         passes.front()->get_mip_filter(),
         passes.front()->get_address_mode(),
   };
   gl_core::Texture source = original;

   for (unsigned i = 0; i < passes.size() - 1; i++)
   {
      passes[i]->build_commands(original, source, vp, nullptr);

      auto &fb = passes[i]->get_framebuffer();
      source.texture.image = fb.get_image();
      source.texture.width    = fb.get_size().width;
      source.texture.height   = fb.get_size().height;
      source.filter           = passes[i + 1]->get_source_filter();
      source.mip_filter       = passes[i + 1]->get_mip_filter();
      source.address          = passes[i + 1]->get_address_mode();

      common.pass_outputs[i] = source;
   }
}

void gl_core_filter_chain::update_history()
{
   unique_ptr<gl_core::Framebuffer> tmp;
   unique_ptr<gl_core::Framebuffer> &back = original_history.back();
   swap(back, tmp);

   if (input_texture.width != tmp->get_size().width ||
       input_texture.height != tmp->get_size().height ||
       (input_texture.format != 0 && input_texture.format != tmp->get_format()))
   {
      tmp->set_size({ input_texture.width, input_texture.height }, input_texture.format);
   }

   tmp->copy(common, input_texture.image);

   /* Should ring buffer, but we don't have *that* many passes. */
   move_backward(begin(original_history), end(original_history) - 1, end(original_history));
   swap(original_history.front(), tmp);
}

void gl_core_filter_chain::end_frame()
{
   /* If we need to keep old frames, copy it after fragment is complete.
    * TODO: We can improve pipelining by figuring out which
    * pass is the last that reads from
    * the history and dispatch the copy earlier. */
   if (!original_history.empty())
   {
      update_history();
   }
}

void gl_core_filter_chain::build_viewport_pass(
      const gl_core_viewport &vp, const float *mvp)
{
   /* First frame, make sure our history and feedback textures are in a clean state. */
   if (require_clear)
   {
      clear_history_and_feedback();
      require_clear = false;
   }

   gl_core::Texture source;
   const gl_core::Texture original = {
         input_texture,
         passes.front()->get_source_filter(),
         passes.front()->get_mip_filter(),
         passes.front()->get_address_mode(),
   };

   if (passes.size() == 1)
   {
      source = {
            input_texture,
            passes.back()->get_source_filter(),
            passes.back()->get_mip_filter(),
            passes.back()->get_address_mode(),
      };
   }
   else
   {
      auto &fb = passes[passes.size() - 2]->get_framebuffer();
      source.texture.image = fb.get_image();
      source.texture.width   = fb.get_size().width;
      source.texture.height  = fb.get_size().height;
      source.filter          = passes.back()->get_source_filter();
      source.mip_filter      = passes.back()->get_mip_filter();
      source.address         = passes.back()->get_address_mode();
   }

   passes.back()->build_commands(original, source, vp, mvp);

   /* For feedback FBOs, swap current and previous. */
   for (auto &pass : passes)
      pass->end_frame();
}

bool gl_core_filter_chain::init_history()
{
   original_history.clear();
   common.original_history.clear();

   size_t required_images = 0;
   for (auto &pass : passes)
   {
      required_images =
            max(required_images,
                pass->get_reflection().semantic_textures[SLANG_TEXTURE_SEMANTIC_ORIGINAL_HISTORY].size());
   }

   if (required_images < 2)
   {
      RARCH_LOG("[GLCore]: Not using frame history.\n");
      return true;
   }

   /* We don't need to store array element #0,
    * since it's aliased with the actual original. */
   required_images--;
   original_history.reserve(required_images);
   common.original_history.resize(required_images);

   for (unsigned i = 0; i < required_images; i++)
   {
      original_history.emplace_back(new gl_core::Framebuffer(0, 1));
   }

   RARCH_LOG("[GLCore]: Using history of %u frames.\n", required_images);

   /* On first frame, we need to clear the textures to
    * a known state, but we need
    * a command buffer for that, so just defer to first frame.
    */
   require_clear = true;
   return true;
}

bool gl_core_filter_chain::init_feedback()
{
   common.framebuffer_feedback.clear();

   bool use_feedbacks = false;

   /* Final pass cannot have feedback. */
   for (unsigned i = 0; i < passes.size() - 1; i++)
   {
      bool use_feedback = false;
      for (auto &pass : passes)
      {
         auto &r = pass->get_reflection();
         auto &feedbacks = r.semantic_textures[SLANG_TEXTURE_SEMANTIC_PASS_FEEDBACK];
         if (i < feedbacks.size() && feedbacks[i].texture)
         {
            use_feedback = true;
            use_feedbacks = true;
            break;
         }
      }

      if (use_feedback && !passes[i]->init_feedback())
         return false;

      if (use_feedback)
         RARCH_LOG("[GLCore]: Using framebuffer feedback for pass #%u.\n", i);
   }

   if (!use_feedbacks)
   {
      RARCH_LOG("[GLCore]: Not using framebuffer feedback.\n");
      return true;
   }

   common.framebuffer_feedback.resize(passes.size() - 1);
   require_clear = true;
   return true;
}

bool gl_core_filter_chain::init_alias()
{
   common.texture_semantic_map.clear();
   common.texture_semantic_uniform_map.clear();

   unsigned i = 0;
   for (auto &pass : passes)
   {
      auto &name = pass->get_name();
      if (name.empty())
         continue;

      unsigned i = &pass - passes.data();

      if (!gl_core_shader_set_unique_map(common.texture_semantic_map, name,
               slang_texture_semantic_map{ SLANG_TEXTURE_SEMANTIC_PASS_OUTPUT, i }))
         return false;

      if (!gl_core_shader_set_unique_map(common.texture_semantic_uniform_map, name + "Size",
               slang_texture_semantic_map{ SLANG_TEXTURE_SEMANTIC_PASS_OUTPUT, i }))
         return false;

      if (!gl_core_shader_set_unique_map(common.texture_semantic_map, name + "Feedback",
               slang_texture_semantic_map{ SLANG_TEXTURE_SEMANTIC_PASS_FEEDBACK, i }))
         return false;

      if (!gl_core_shader_set_unique_map(common.texture_semantic_uniform_map, name + "FeedbackSize",
               slang_texture_semantic_map{ SLANG_TEXTURE_SEMANTIC_PASS_FEEDBACK, i }))
         return false;
   }

   for (auto &lut : common.luts)
   {
      unsigned i = &lut - common.luts.data();
      if (!gl_core_shader_set_unique_map(common.texture_semantic_map, lut->get_id(),
               slang_texture_semantic_map{ SLANG_TEXTURE_SEMANTIC_USER, i }))
         return false;

      if (!gl_core_shader_set_unique_map(common.texture_semantic_uniform_map, lut->get_id() + "Size",
               slang_texture_semantic_map{ SLANG_TEXTURE_SEMANTIC_USER, i }))
         return false;
   }

   return true;
}

void gl_core_filter_chain::set_pass_info(unsigned pass, const gl_core_filter_chain_pass_info &info)
{
   if (pass >= pass_info.size())
      pass_info.resize(pass + 1);
   pass_info[pass] = info;
}

void gl_core_filter_chain::set_shader(unsigned pass, GLenum stage, const uint32_t *spirv, size_t spirv_words)
{
   if (pass >= passes.size())
      passes.resize(pass + 1);
   passes[pass]->set_shader(stage, spirv, spirv_words);
}

void gl_core_filter_chain::add_parameter(unsigned pass, unsigned index, const std::string &id)
{
   passes[pass]->add_parameter(index, id);
}

bool gl_core_filter_chain::init()
{
   if (!init_alias())
      return false;

   for (unsigned i = 0; i < passes.size(); i++)
   {
      auto &pass = passes[i];
      RARCH_LOG("[slang]: Building pass #%u (%s)\n", i,
            pass->get_name().empty() ?
            msg_hash_to_str(MENU_ENUM_LABEL_VALUE_NOT_AVAILABLE) :
            pass->get_name().c_str());

      pass->set_pass_info(pass_info[i]);
      if (!pass->build())
         return false;
   }

   require_clear = false;
   if (!init_history())
      return false;
   if (!init_feedback())
      return false;
   common.pass_outputs.resize(passes.size());
   return true;
}

void gl_core_filter_chain::set_input_texture(
      const gl_core_filter_chain_texture &texture)
{
   input_texture = texture;
}

void gl_core_filter_chain::add_static_texture(unique_ptr<gl_core::StaticTexture> texture)
{
   common.luts.push_back(move(texture));
}

void gl_core_filter_chain::set_frame_count(uint64_t count)
{
   for (auto &pass : passes)
      pass->set_frame_count(count);
}

void gl_core_filter_chain::set_frame_count_period(unsigned pass, unsigned period)
{
   passes[pass]->set_frame_count_period(period);
}

void gl_core_filter_chain::set_pass_name(unsigned pass, const char *name)
{
   passes[pass]->set_name(name);
}

static unique_ptr<gl_core::StaticTexture> gl_core_filter_chain_load_lut(
      gl_core_filter_chain *chain,
      const video_shader_lut *shader)
{
   texture_image image = {};
   image.supports_rgba = true;

   if (!image_texture_load(&image, shader->path))
      return {};

   unsigned levels = shader->mipmap ? gl_core::num_miplevels(image.width, image.height) : 1;
   GLuint tex = 0;
   glGenTextures(1, &tex);
   glBindTexture(GL_TEXTURE_2D, tex);
   glTexStorage2D(GL_TEXTURE_2D, levels,
                  GL_RGBA8, image.width, image.height);
   glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                   image.width, image.height,
                   GL_RGBA, GL_UNSIGNED_BYTE, image.pixels);

   if (levels > 1)
      glGenerateMipmap(GL_TEXTURE_2D);
   glBindTexture(GL_TEXTURE_2D, 0);

   if (image.pixels)
      image_texture_free(&image);

   return unique_ptr<gl_core::StaticTexture>(new gl_core::StaticTexture(shader->id,
                                                                        tex, image.width, image.height,
                                                                        shader->filter != RARCH_FILTER_NEAREST,
                                                                        levels > 1,
                                                                        gl_core::address_to_gl(gl_core::wrap_to_address(shader->wrap))));
}

static bool gl_core_filter_chain_load_luts(
      gl_core_filter_chain *chain,
      video_shader *shader)
{
   for (unsigned i = 0; i < shader->luts; i++)
   {
      auto image = gl_core_filter_chain_load_lut(chain, &shader->lut[i]);
      if (!image)
      {
         RARCH_ERR("[Vulkan]: Failed to load LUT \"%s\".\n", shader->lut[i].path);
         return false;
      }

      chain->add_static_texture(move(image));
   }

   return true;
}

gl_core_filter_chain_t *gl_core_filter_chain_create_default(
      gl_core_filter_chain_filter filter)
{
   struct gl_core_filter_chain_pass_info pass_info;

   unique_ptr<gl_core_filter_chain> chain{ new gl_core_filter_chain() };
   if (!chain)
      return nullptr;

   pass_info.scale_type_x  = GL_CORE_FILTER_CHAIN_SCALE_VIEWPORT;
   pass_info.scale_type_y  = GL_CORE_FILTER_CHAIN_SCALE_VIEWPORT;
   pass_info.scale_x       = 1.0f;
   pass_info.scale_y       = 1.0f;
   pass_info.rt_format     = 0;
   pass_info.source_filter = filter;
   pass_info.mip_filter    = GL_CORE_FILTER_CHAIN_NEAREST;
   pass_info.address       = GL_CORE_FILTER_CHAIN_ADDRESS_CLAMP_TO_EDGE;
   pass_info.max_levels    = 0;

   chain->set_pass_info(0, pass_info);

   chain->set_shader(0, GL_VERTEX_SHADER,
         gl_core::opaque_vert,
         sizeof(gl_core::opaque_vert) / sizeof(uint32_t));
   chain->set_shader(0, GL_FRAGMENT_SHADER,
         gl_core::opaque_frag,
         sizeof(gl_core::opaque_frag) / sizeof(uint32_t));

   if (!chain->init())
      return nullptr;

   return chain.release();
}

gl_core_filter_chain_t *gl_core_filter_chain_create_from_preset(
      const char *path, gl_core_filter_chain_filter filter)
{
   unsigned i;
   unique_ptr<video_shader> shader{ new video_shader() };
   if (!shader)
      return nullptr;

   unique_ptr<config_file_t, gl_core::ConfigDeleter> conf{ config_file_new(path) };
   if (!conf)
      return nullptr;

   if (!video_shader_read_conf_cgp(conf.get(), shader.get()))
      return nullptr;

   video_shader_resolve_relative(shader.get(), path);

   bool last_pass_is_fbo = shader->pass[shader->passes - 1].fbo.valid;

   unique_ptr<gl_core_filter_chain> chain{ new gl_core_filter_chain() };
   if (!chain)
      return nullptr;

   if (shader->luts && !gl_core_filter_chain_load_luts(chain.get(), shader.get()))
      return nullptr;

   shader->num_parameters = 0;

   for (i = 0; i < shader->passes; i++)
   {
      glslang_output output;
      struct gl_core_filter_chain_pass_info pass_info;
      const video_shader_pass *pass      = &shader->pass[i];
      const video_shader_pass *next_pass =
         i + 1 < shader->passes ? &shader->pass[i + 1] : nullptr;

      pass_info.scale_type_x  = GL_CORE_FILTER_CHAIN_SCALE_ORIGINAL;
      pass_info.scale_type_y  = GL_CORE_FILTER_CHAIN_SCALE_ORIGINAL;
      pass_info.scale_x       = 0.0f;
      pass_info.scale_y       = 0.0f;
      pass_info.rt_format     = 0;
      pass_info.source_filter = GL_CORE_FILTER_CHAIN_LINEAR;
      pass_info.mip_filter    = GL_CORE_FILTER_CHAIN_LINEAR;
      pass_info.address       = GL_CORE_FILTER_CHAIN_ADDRESS_REPEAT;
      pass_info.max_levels    = 0;

      if (!glslang_compile_shader(pass->source.path, &output))
      {
         RARCH_ERR("Failed to compile shader: \"%s\".\n",
               pass->source.path);
         return nullptr;
      }

      for (auto &meta_param : output.meta.parameters)
      {
         if (shader->num_parameters >= GFX_MAX_PARAMETERS)
         {
            RARCH_ERR("[Vulkan]: Exceeded maximum number of parameters.\n");
            return nullptr;
         }

         auto itr = find_if(shader->parameters, shader->parameters + shader->num_parameters,
               [&](const video_shader_parameter &param)
               {
                  return meta_param.id == param.id;
               });

         if (itr != shader->parameters + shader->num_parameters)
         {
            /* Allow duplicate #pragma parameter, but
             * only if they are exactly the same. */
            if (meta_param.desc != itr->desc ||
                meta_param.initial != itr->initial ||
                meta_param.minimum != itr->minimum ||
                meta_param.maximum != itr->maximum ||
                meta_param.step != itr->step)
            {
               RARCH_ERR("[Vulkan]: Duplicate parameters found for \"%s\", but arguments do not match.\n",
                     itr->id);
               return nullptr;
            }
            chain->add_parameter(i, itr - shader->parameters, meta_param.id);
         }
         else
         {
            auto &param = shader->parameters[shader->num_parameters];
            strlcpy(param.id, meta_param.id.c_str(), sizeof(param.id));
            strlcpy(param.desc, meta_param.desc.c_str(), sizeof(param.desc));
            param.current = meta_param.initial;
            param.initial = meta_param.initial;
            param.minimum = meta_param.minimum;
            param.maximum = meta_param.maximum;
            param.step = meta_param.step;
            chain->add_parameter(i, shader->num_parameters, meta_param.id);
            shader->num_parameters++;
         }
      }

      chain->set_shader(i,
            GL_VERTEX_SHADER,
            output.vertex.data(),
            output.vertex.size());

      chain->set_shader(i,
            GL_FRAGMENT_SHADER,
            output.fragment.data(),
            output.fragment.size());

      chain->set_frame_count_period(i, pass->frame_count_mod);

      if (!output.meta.name.empty())
         chain->set_pass_name(i, output.meta.name.c_str());

      /* Preset overrides. */
      if (*pass->alias)
         chain->set_pass_name(i, pass->alias);

      if (pass->filter == RARCH_FILTER_UNSPEC)
         pass_info.source_filter = filter;
      else
      {
         pass_info.source_filter =
            pass->filter == RARCH_FILTER_LINEAR ? GL_CORE_FILTER_CHAIN_LINEAR :
            GL_CORE_FILTER_CHAIN_NEAREST;
      }
      pass_info.address    = gl_core::wrap_to_address(pass->wrap);
      pass_info.max_levels = 1;

      /* TODO: Expose max_levels in slangp.
       * CGP format is a bit awkward in that it uses mipmap_input,
       * so we much check if next pass needs the mipmapping.
       */
      if (next_pass && next_pass->mipmap)
         pass_info.max_levels = ~0u;

      pass_info.mip_filter = pass->filter != RARCH_FILTER_NEAREST && pass_info.max_levels > 1
         ? GL_CORE_FILTER_CHAIN_LINEAR : GL_CORE_FILTER_CHAIN_NEAREST;

      bool explicit_format = output.meta.rt_format != SLANG_FORMAT_UNKNOWN;

      /* Set a reasonable default. */
      if (output.meta.rt_format == SLANG_FORMAT_UNKNOWN)
         output.meta.rt_format = SLANG_FORMAT_R8G8B8A8_UNORM;

      if (!pass->fbo.valid)
      {
         pass_info.scale_type_x = i + 1 == shader->passes
            ? GL_CORE_FILTER_CHAIN_SCALE_VIEWPORT
            : GL_CORE_FILTER_CHAIN_SCALE_SOURCE;
         pass_info.scale_type_y = i + 1 == shader->passes
            ? GL_CORE_FILTER_CHAIN_SCALE_VIEWPORT
            : GL_CORE_FILTER_CHAIN_SCALE_SOURCE;
         pass_info.scale_x = 1.0f;
         pass_info.scale_y = 1.0f;

         if (i + 1 == shader->passes)
         {
            pass_info.rt_format = 0;

            if (explicit_format)
               RARCH_WARN("[slang]: Using explicit format for last pass in chain,"
                     " but it is not rendered to framebuffer, using swapchain format instead.\n");
         }
         else
         {
            pass_info.rt_format = gl_core::convert_glslang_format(output.meta.rt_format);
            RARCH_LOG("[slang]: Using render target format %s for pass output #%u.\n",
                  glslang_format_to_string(output.meta.rt_format), i);
         }
      }
      else
      {
         /* Preset overrides shader.
          * Kinda ugly ... */
         if (pass->fbo.srgb_fbo)
            output.meta.rt_format = SLANG_FORMAT_R8G8B8A8_SRGB;
         else if (pass->fbo.fp_fbo)
            output.meta.rt_format = SLANG_FORMAT_R16G16B16A16_SFLOAT;

         pass_info.rt_format = gl_core::convert_glslang_format(output.meta.rt_format);
         RARCH_LOG("[slang]: Using render target format %s for pass output #%u.\n",
               glslang_format_to_string(output.meta.rt_format), i);

         switch (pass->fbo.type_x)
         {
            case RARCH_SCALE_INPUT:
               pass_info.scale_x = pass->fbo.scale_x;
               pass_info.scale_type_x = GL_CORE_FILTER_CHAIN_SCALE_SOURCE;
               break;

            case RARCH_SCALE_ABSOLUTE:
               pass_info.scale_x = float(pass->fbo.abs_x);
               pass_info.scale_type_x = GL_CORE_FILTER_CHAIN_SCALE_ABSOLUTE;
               break;

            case RARCH_SCALE_VIEWPORT:
               pass_info.scale_x = pass->fbo.scale_x;
               pass_info.scale_type_x = GL_CORE_FILTER_CHAIN_SCALE_VIEWPORT;
               break;
         }

         switch (pass->fbo.type_y)
         {
            case RARCH_SCALE_INPUT:
               pass_info.scale_y = pass->fbo.scale_y;
               pass_info.scale_type_y = GL_CORE_FILTER_CHAIN_SCALE_SOURCE;
               break;

            case RARCH_SCALE_ABSOLUTE:
               pass_info.scale_y = float(pass->fbo.abs_y);
               pass_info.scale_type_y = GL_CORE_FILTER_CHAIN_SCALE_ABSOLUTE;
               break;

            case RARCH_SCALE_VIEWPORT:
               pass_info.scale_y = pass->fbo.scale_y;
               pass_info.scale_type_y = GL_CORE_FILTER_CHAIN_SCALE_VIEWPORT;
               break;
         }
      }

      chain->set_pass_info(i, pass_info);
   }

   if (last_pass_is_fbo)
   {
      struct gl_core_filter_chain_pass_info pass_info;

      pass_info.scale_type_x  = GL_CORE_FILTER_CHAIN_SCALE_VIEWPORT;
      pass_info.scale_type_y  = GL_CORE_FILTER_CHAIN_SCALE_VIEWPORT;
      pass_info.scale_x       = 1.0f;
      pass_info.scale_y       = 1.0f;

      pass_info.rt_format     = 0;

      pass_info.source_filter = filter;
      pass_info.mip_filter    = GL_CORE_FILTER_CHAIN_NEAREST;
      pass_info.address       = GL_CORE_FILTER_CHAIN_ADDRESS_CLAMP_TO_EDGE;

      pass_info.max_levels    = 0;

      chain->set_pass_info(shader->passes, pass_info);

      chain->set_shader(shader->passes,
            GL_VERTEX_SHADER,
            gl_core::opaque_vert,
            sizeof(gl_core::opaque_vert) / sizeof(uint32_t));

      chain->set_shader(shader->passes,
            GL_FRAGMENT_SHADER,
            gl_core::opaque_frag,
            sizeof(gl_core::opaque_frag) / sizeof(uint32_t));
   }

   if (!video_shader_resolve_current_parameters(conf.get(), shader.get()))
      return nullptr;

   chain->set_shader_preset(move(shader));

   if (!chain->init())
      return nullptr;

   return chain.release();
}

struct video_shader *gl_core_filter_chain_get_preset(
      gl_core_filter_chain_t *chain)
{
   return chain->get_shader_preset();
}

void gl_core_filter_chain_free(
      gl_core_filter_chain_t *chain)
{
   delete chain;
}

void gl_core_filter_chain_set_shader(
      gl_core_filter_chain_t *chain,
      unsigned pass,
      GLenum shader_stage,
      const uint32_t *spirv,
      size_t spirv_words)
{
   chain->set_shader(pass, shader_stage, spirv, spirv_words);
}

void gl_core_filter_chain_set_pass_info(
      gl_core_filter_chain_t *chain,
      unsigned pass,
      const struct gl_core_filter_chain_pass_info *info)
{
   chain->set_pass_info(pass, *info);
}

bool gl_core_filter_chain_init(gl_core_filter_chain_t *chain)
{
   return chain->init();
}

void gl_core_filter_chain_set_input_texture(
      gl_core_filter_chain_t *chain,
      const struct gl_core_filter_chain_texture *texture)
{
   chain->set_input_texture(*texture);
}

void gl_core_filter_chain_set_frame_count(
      gl_core_filter_chain_t *chain,
      uint64_t count)
{
   chain->set_frame_count(count);
}

void gl_core_filter_chain_set_frame_count_period(
      gl_core_filter_chain_t *chain,
      unsigned pass,
      unsigned period)
{
   chain->set_frame_count_period(pass, period);
}

void gl_core_filter_chain_set_pass_name(
      gl_core_filter_chain_t *chain,
      unsigned pass,
      const char *name)
{
   chain->set_pass_name(pass, name);
}

void gl_core_filter_chain_build_offscreen_passes(
      gl_core_filter_chain_t *chain,
      const gl_core_viewport *vp)
{
   chain->build_offscreen_passes(*vp);
}

void gl_core_filter_chain_build_viewport_pass(
      gl_core_filter_chain_t *chain,
      const gl_core_viewport *vp, const float *mvp)
{
   chain->build_viewport_pass(*vp, mvp);
}

void gl_core_filter_chain_end_frame(gl_core_filter_chain_t *chain)
{
   chain->end_frame();
}