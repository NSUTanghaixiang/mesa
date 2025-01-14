/*
 * Copyright © 2022 Valve Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include "ac_nir.h"
#include "nir_builder.h"
#include "amdgfxregs.h"
#include "u_math.h"

/*
 * These NIR passes are used to lower NIR cross-stage I/O intrinsics
 * between task and mesh shader stages into the memory accesses
 * that actually happen on the HW.
 *
 */

typedef struct {
   unsigned payload_entry_bytes;
   unsigned draw_entry_bytes;
   unsigned num_entries;
} lower_tsms_io_state;

typedef struct {
   nir_ssa_def *hw_workgroup_id;
   nir_ssa_def *api_workgroup_id;
} add_first_task_to_workgroup_id_state;

static bool filter_workgroup_id(const nir_instr *instr,
                                UNUSED const void *state)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
   return intrin->intrinsic == nir_intrinsic_load_workgroup_id;
}

static nir_ssa_def *
replace_workgroup_id_use_first_task(nir_builder *b,
                                    nir_instr *instr,
                                    void *state)
{
   add_first_task_to_workgroup_id_state *s = (add_first_task_to_workgroup_id_state *) state;
   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);

   assert(s->hw_workgroup_id);

   if (s->hw_workgroup_id == &intrin->dest.ssa)
      return NULL;

   return s->api_workgroup_id;
}

void
ac_nir_apply_first_task_to_task_shader(nir_shader *shader)
{
   /* The draw packets on RDNA2 GPUs don't support adding an offset to the task shader
    * workgroups, so we have to emulate the firstTask feature for NV_mesh_shader.
    *
    * 1. Pass the address of the IB (indirect buffer) from the NV_mesh_shader draw call
    *    to the shader in an SGPR argument (2 SGPRs for address, 1 SGPR for stride).
    * 2. Create a descriptor for the IB in the shader.
    * 3. Load the firstTask value from the IB
    * 4. Add the firstTask value the workgroup ID and use the result instead of the
    *    workgroup ID generated by the HW.
    *
    * NOTE: This pass must run _before_ lowering the task shader outputs to memory
    *       accesses. The lowering uses the workgroup ID and that must be unchanged
    *       because it has to be the real HW workgroup ID.
    */

   /* If the shader doesn't use workgroup ID, nothing to do here. */
   if (!BITSET_TEST(shader->info.system_values_read, SYSTEM_VALUE_WORKGROUP_ID))
      return;

   nir_function_impl *impl = nir_shader_get_entrypoint(shader);
   assert(impl);

   nir_builder builder;
   nir_builder *b = &builder; /* This is to avoid the & */
   nir_builder_init(b, impl);
   b->cursor = nir_before_cf_list(&impl->body);

   /* This is the stride passed to vkCmdDrawMeshTasksIndirectNV */
   nir_ssa_def *ib_stride = nir_load_task_ib_stride(b);
   nir_ssa_def *zero = nir_imm_int(b, 0);
   nir_ssa_def *first_task = NULL;

   /* If the stride is zero, we assume that firstTask is also 0. */
   nir_if *if_stride = nir_push_if(b, nir_ine(b, ib_stride, zero));
   {
      /* Address of the IB (indirect buffer) used by the current draw call. */
      nir_ssa_def *ib_addr = nir_load_task_ib_addr(b);

      /* Compose a 64-bit address from the IB address. */
      nir_ssa_def *addr = nir_pack_64_2x32_split(b, nir_channel(b, ib_addr, 0), nir_channel(b, ib_addr, 1));
      /* The IB needs to be addressed by draw ID * stride. */
      addr = nir_iadd(b, addr, nir_u2u64(b, nir_imul(b, nir_load_draw_id(b), ib_stride)));
      /* Byte offset of the firstTask field in VkDrawMeshTasksIndirectCommandNV. */
      addr = nir_iadd_imm(b, addr, 4);

      first_task = nir_build_load_global(b, 1, 32, addr, .access = ACCESS_NON_WRITEABLE | ACCESS_COHERENT);
   }
   nir_pop_if(b, if_stride);
   first_task = nir_if_phi(b, first_task, zero);

   /* NV_mesh_shader workgroups are 1 dimensional so we only care about X here. */
   nir_ssa_def *hw_workgroup_id = nir_load_workgroup_id(b, 32);
   nir_ssa_def *api_workgroup_id_x = nir_iadd(b, nir_channel(b, hw_workgroup_id, 0), first_task);
   nir_ssa_def *api_workgroup_id = nir_vec3(b, api_workgroup_id_x, zero, zero);

   add_first_task_to_workgroup_id_state state = {
      .hw_workgroup_id = hw_workgroup_id,
      .api_workgroup_id = api_workgroup_id,
   };
   nir_shader_lower_instructions(shader,
                                 filter_workgroup_id,
                                 replace_workgroup_id_use_first_task,
                                 &state);

   nir_validate_shader(shader, "after including firstTask in the task shader workgroup ID");
}

static nir_ssa_def *
task_workgroup_index(nir_builder *b,
                     lower_tsms_io_state *s)
{
   nir_ssa_def *id = nir_load_workgroup_id(b, 32);

   /* NV_mesh_shader: workgroups are always 1D, so index is the same as ID.x */
   return nir_channel(b, id, 0);
}

static nir_ssa_def *
task_ring_entry_index(nir_builder *b,
                      lower_tsms_io_state *s)
{
   /* Task shader ring_entry shader argument:
    *
    * - It's a copy of write_ptr[31:0] from the task control buffer.
    * - The same value (which is the initial value at dispatch)
    *   seems to be copied to all workgroups in the same dispatch,
    *   therefore a workgroup index needs to be added.
    * - write_ptr must be initialized to num_entries so ring_entry needs
    *   AND with num_entries - 1 to get the correct meaning.
    *   Note that num_entries must be a power of two.
    */
   nir_ssa_def *ring_entry = nir_load_task_ring_entry_amd(b);
   nir_ssa_def *idx = nir_iadd_nuw(b, ring_entry, task_workgroup_index(b, s));
   return nir_iand_imm(b, idx, s->num_entries - 1);
}

static nir_ssa_def *
task_draw_ready_bit(nir_builder *b,
                    lower_tsms_io_state *s)
{
   /* Value of the ready bit is 1 for odd and 0 for even passes through the draw ring.
    *
    * The ring_entry is a copy of the write_ptr. We use that to determine whether
    * the current pass through the draw ring is odd or even, so we can write the
    * correct value to the draw ready bit.
    *
    * This tells the firmware that it can now start launching mesh shader workgroups.
    * The encoding of the last dword of the draw ring entry is:
    * - bit 0: Draw ready bit.
    *          Its meaning flips on every pass through the entry.
    * - bit 1: Packet end bit.
    *          The firmware uses this to mark the entry after the last one
    *          used by the current task dispatch.
    * - bits [2:31] unused.
    *
    * Task shaders MUST write the draw ready bit to the draw ring
    * before they finish. The firmware waits for the shader to write
    * this bit before it reads the mesh dispatch size to launch the
    * mesh shader workgroups.
    *
    * If the task shader doesn't write this bit, the HW hangs.
    */

   nir_ssa_def *ring_entry = nir_load_task_ring_entry_amd(b);
   nir_ssa_def *workgroup_index = task_workgroup_index(b, s);

   nir_ssa_def *idx = nir_iadd_nuw(b, ring_entry, workgroup_index);
   return nir_ubfe(b, idx, nir_imm_int(b, util_bitcount(s->num_entries - 1)), nir_imm_int(b, 1));
}

static nir_ssa_def *
mesh_ring_entry_index(nir_builder *b,
                      lower_tsms_io_state *s)
{
   /* Mesh shader ring_entry shader argument:
    *
    * - It's a copy of the read_ptr[31:0] from the task control buffer.
    * - All workgroups in the same task->mesh dispatch get the same value,
    *   which is fine because they need to read the same entry.
    * - read_ptr must be initialized to num_entries so ring_entry needs
    *   AND with num_entries - 1 to get the correct meaning.
    *   Note that num_entries must be a power of two.
    */
   return nir_iand_imm(b, nir_load_task_ring_entry_amd(b), s->num_entries - 1);
}

static void
task_write_draw_ring(nir_builder *b,
                     nir_ssa_def *store_val,
                     unsigned const_off,
                     lower_tsms_io_state *s)
{
   nir_ssa_def *ptr = task_ring_entry_index(b, s);
   nir_ssa_def *ring = nir_load_ring_task_draw_amd(b);
   nir_ssa_def *scalar_off = nir_imul_imm(b, ptr, s->draw_entry_bytes);
   nir_ssa_def *vector_off = nir_imm_int(b, 0);

   nir_store_buffer_amd(b, store_val, ring, vector_off, scalar_off,
                        .base = const_off, .memory_modes = nir_var_shader_out);
}

static bool
filter_task_output_or_payload(const nir_instr *instr,
                              UNUSED const void *state)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
   return intrin->intrinsic == nir_intrinsic_store_output ||
          intrin->intrinsic == nir_intrinsic_store_task_payload ||
          intrin->intrinsic == nir_intrinsic_load_task_payload;
}

static nir_ssa_def *
lower_task_output_store(nir_builder *b,
                        nir_intrinsic_instr *intrin,
                        lower_tsms_io_state *s)
{
   /* NV_mesh_shader:
    * Task shaders should only have 1 output: TASK_COUNT
    * which is the number of launched mesh shader workgroups in 1D.
    *
    * Task count is one dimensional, but the HW needs X, Y, Z.
    * Use the shader's value for X, and write Y=1, Z=1.
    */

   nir_ssa_def *store_val = nir_vec3(b, intrin->src[0].ssa,
                                        nir_imm_int(b, 1),
                                        nir_imm_int(b, 1));

   task_write_draw_ring(b, store_val, 0, s);
   return NIR_LOWER_INSTR_PROGRESS_REPLACE;
}

static nir_ssa_def *
lower_task_payload_store(nir_builder *b,
                         nir_intrinsic_instr *intrin,
                         lower_tsms_io_state *s)
{
   unsigned write_mask = nir_intrinsic_write_mask(intrin);
   unsigned base = nir_intrinsic_base(intrin);

   nir_ssa_def *store_val = intrin->src[0].ssa;
   nir_ssa_def *addr = intrin->src[1].ssa;
   nir_ssa_def *ring = nir_load_ring_task_payload_amd(b);
   nir_ssa_def *ptr = task_ring_entry_index(b, s);
   nir_ssa_def *ring_off = nir_imul_imm(b, ptr, s->payload_entry_bytes);

   nir_store_buffer_amd(b, store_val, ring, addr, ring_off, .base = base,
                        .write_mask = write_mask,
                        .memory_modes = nir_var_mem_task_payload);

   return NIR_LOWER_INSTR_PROGRESS_REPLACE;
}

static nir_ssa_def *
lower_taskmesh_payload_load(nir_builder *b,
                            nir_intrinsic_instr *intrin,
                            lower_tsms_io_state *s)
{
   unsigned base = nir_intrinsic_base(intrin);
   unsigned num_components = intrin->dest.ssa.num_components;
   unsigned bit_size = intrin->dest.ssa.bit_size;

   nir_ssa_def *ptr =
      b->shader->info.stage == MESA_SHADER_TASK ?
      task_ring_entry_index(b, s) :
      mesh_ring_entry_index(b, s);

   nir_ssa_def *addr = intrin->src[0].ssa;
   nir_ssa_def *ring = nir_load_ring_task_payload_amd(b);
   nir_ssa_def *ring_off = nir_imul_imm(b, ptr, s->payload_entry_bytes);

   return nir_load_buffer_amd(b, num_components, bit_size, ring, addr, ring_off, .base = base,
                              .memory_modes = nir_var_mem_task_payload);
}

static nir_ssa_def *
lower_task_intrinsics(nir_builder *b,
                      nir_instr *instr,
                      void *state)
{
   assert(instr->type == nir_instr_type_intrinsic);
   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
   lower_tsms_io_state *s = (lower_tsms_io_state *)state;

   if (intrin->intrinsic == nir_intrinsic_store_output)
      return lower_task_output_store(b, intrin, s);
   else if (intrin->intrinsic == nir_intrinsic_store_task_payload)
      return lower_task_payload_store(b, intrin, s);
   else if (intrin->intrinsic == nir_intrinsic_load_task_payload)
      return lower_taskmesh_payload_load(b, intrin, s);
   else
      unreachable("unsupported task shader intrinsic");
}

static void
emit_task_finale(nir_builder *b, lower_tsms_io_state *s)
{
   /* We assume there is always a single end block in the shader. */
   b->cursor = nir_after_block(nir_impl_last_block(b->impl));

   /* Wait for all task_payload, output, SSBO and global stores to finish. */
   nir_scoped_barrier(b, .execution_scope = NIR_SCOPE_WORKGROUP,
                         .memory_scope = NIR_SCOPE_WORKGROUP,
                         .memory_semantics = NIR_MEMORY_ACQ_REL,
                         .memory_modes = nir_var_mem_task_payload | nir_var_shader_out |
                                         nir_var_mem_ssbo | nir_var_mem_global);

   nir_ssa_def *invocation_index = nir_load_local_invocation_index(b);
   nir_if *if_invocation_index_zero = nir_push_if(b, nir_ieq_imm(b, invocation_index, 0));
   {
      /* Write ready bit. */
      nir_ssa_def *ready_bit = task_draw_ready_bit(b, s);
      task_write_draw_ring(b, ready_bit, 12, s);
   }
   nir_pop_if(b, if_invocation_index_zero);
}

void
ac_nir_lower_task_outputs_to_mem(nir_shader *shader,
                                 unsigned task_payload_entry_bytes,
                                 unsigned task_num_entries)
{
   assert(util_is_power_of_two_nonzero(task_num_entries));

   lower_tsms_io_state state = {
      .draw_entry_bytes = 16,
      .payload_entry_bytes = task_payload_entry_bytes,
      .num_entries = task_num_entries,
   };

   nir_function_impl *impl = nir_shader_get_entrypoint(shader);
   nir_builder builder;
   nir_builder *b = &builder; /* This is to avoid the & */
   nir_builder_init(b, impl);

   nir_shader_lower_instructions(shader,
                                 filter_task_output_or_payload,
                                 lower_task_intrinsics,
                                 &state);

   emit_task_finale(b, &state);
   nir_metadata_preserve(impl, nir_metadata_none);

   nir_validate_shader(shader, "after lowering task shader outputs to memory stores");
}

static bool
filter_mesh_input_load(const nir_instr *instr,
                       UNUSED const void *state)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
   return intrin->intrinsic == nir_intrinsic_load_task_payload;
}

static nir_ssa_def *
lower_mesh_intrinsics(nir_builder *b,
                      nir_instr *instr,
                      void *state)
{
   assert(instr->type == nir_instr_type_intrinsic);
   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
   lower_tsms_io_state *s = (lower_tsms_io_state *)state;

   if (intrin->intrinsic == nir_intrinsic_load_task_payload)
      return lower_taskmesh_payload_load(b, intrin, s);
   else
      unreachable("unsupported mesh shader intrinsic");
}

void
ac_nir_lower_mesh_inputs_to_mem(nir_shader *shader,
                                unsigned task_payload_entry_bytes,
                                unsigned task_num_entries)
{
   assert(util_is_power_of_two_nonzero(task_num_entries));

   lower_tsms_io_state state = {
      .draw_entry_bytes = 16,
      .payload_entry_bytes = task_payload_entry_bytes,
      .num_entries = task_num_entries,
   };

   nir_shader_lower_instructions(shader,
                                 filter_mesh_input_load,
                                 lower_mesh_intrinsics,
                                 &state);
}
