/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "stack.h"
#include <limits>

#include "android-base/stringprintf.h"

#include "arch/context.h"
#include "art_method-inl.h"
#include "base/callee_save_type.h"
#include "base/hex_dump.h"
#include "base/indenter.h"
#include "base/pointer_size.h"
#include "base/utils.h"
#include "dex/dex_file_types.h"
#include "entrypoints/entrypoint_utils-inl.h"
#include "entrypoints/quick/callee_save_frame.h"
#include "entrypoints/runtime_asm_entrypoints.h"
#include "gc/space/image_space.h"
#include "gc/space/space-inl.h"
#include "interpreter/mterp/nterp.h"
#include "interpreter/shadow_frame-inl.h"
#include "jit/jit.h"
#include "jit/jit_code_cache.h"
#include "linear_alloc.h"
#include "managed_stack.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "nterp_helpers.h"
#include "oat/oat_quick_method_header.h"
#include "obj_ptr-inl.h"
#include "quick/quick_method_frame_info.h"
#include "runtime.h"
#include "thread.h"
#include "thread_list.h"

namespace art HIDDEN {

using android::base::StringPrintf;

static constexpr bool kDebugStackWalk = false;

StackVisitor::StackVisitor(Thread* thread,
                           Context* context,
                           StackWalkKind walk_kind,
                           bool check_suspended)
    : StackVisitor(thread, context, walk_kind, 0, check_suspended) {}

StackVisitor::StackVisitor(Thread* thread,
                           Context* context,
                           StackWalkKind walk_kind,
                           size_t num_frames,
                           bool check_suspended)
    : thread_(thread),
      walk_kind_(walk_kind),
      cur_shadow_frame_(nullptr),
      cur_quick_frame_(nullptr),
      cur_quick_frame_pc_(0),
      cur_oat_quick_method_header_(nullptr),
      num_frames_(num_frames),
      cur_depth_(0),
      cur_inline_info_(nullptr, CodeInfo()),
      cur_stack_map_(0, StackMap()),
      context_(context),
      check_suspended_(check_suspended) {
  if (check_suspended_) {
    DCHECK(thread == Thread::Current() || thread->GetState() != ThreadState::kRunnable) << *thread;
  }
}

CodeInfo* StackVisitor::GetCurrentInlineInfo() const {
  DCHECK(!(*cur_quick_frame_)->IsNative());
  const OatQuickMethodHeader* header = GetCurrentOatQuickMethodHeader();
  if (cur_inline_info_.first != header) {
    cur_inline_info_ = std::make_pair(header, CodeInfo::DecodeInlineInfoOnly(header));
  }
  return &cur_inline_info_.second;
}

StackMap* StackVisitor::GetCurrentStackMap() const {
  DCHECK(!(*cur_quick_frame_)->IsNative());
  const OatQuickMethodHeader* header = GetCurrentOatQuickMethodHeader();
  if (cur_stack_map_.first != cur_quick_frame_pc_) {
    uint32_t pc = header->NativeQuickPcOffset(cur_quick_frame_pc_);
    cur_stack_map_ = std::make_pair(cur_quick_frame_pc_,
                                    GetCurrentInlineInfo()->GetStackMapForNativePcOffset(pc));
  }
  return &cur_stack_map_.second;
}

ArtMethod* StackVisitor::GetMethod() const {
  if (cur_shadow_frame_ != nullptr) {
    return cur_shadow_frame_->GetMethod();
  } else if (cur_quick_frame_ != nullptr) {
    if (IsInInlinedFrame()) {
      CodeInfo* code_info = GetCurrentInlineInfo();
      DCHECK(walk_kind_ != StackWalkKind::kSkipInlinedFrames);
      return GetResolvedMethod(*GetCurrentQuickFrame(), *code_info, current_inline_frames_);
    } else {
      return *cur_quick_frame_;
    }
  }
  return nullptr;
}

uint32_t StackVisitor::GetDexPc(bool abort_on_failure) const {
  if (cur_shadow_frame_ != nullptr) {
    return cur_shadow_frame_->GetDexPC();
  } else if (cur_quick_frame_ != nullptr) {
    if (IsInInlinedFrame()) {
      return current_inline_frames_.back().GetDexPc();
    } else if (cur_oat_quick_method_header_ == nullptr) {
      return dex::kDexNoIndex;
    } else if ((*GetCurrentQuickFrame())->IsNative()) {
      return cur_oat_quick_method_header_->ToDexPc(
          GetCurrentQuickFrame(), cur_quick_frame_pc_, abort_on_failure);
    } else if (cur_oat_quick_method_header_->IsOptimized()) {
      StackMap* stack_map = GetCurrentStackMap();
      if (!stack_map->IsValid()) {
        // Debugging code for b/361916648.
        CodeInfo code_info(cur_oat_quick_method_header_);
        std::stringstream os;
        VariableIndentationOutputStream vios(&os);
        code_info.Dump(&vios, /* code_offset= */ 0u, /* verbose= */ true, kRuntimeISA);
        LOG(FATAL) << os.str() << '\n'
                   << "StackMap not found for "
                   << std::hex << cur_quick_frame_pc_ << " in "
                   << GetMethod()->PrettyMethod()
                   << " @" << std::hex
                   << reinterpret_cast<uintptr_t>(cur_oat_quick_method_header_->GetCode());
      }
      return stack_map->GetDexPc();
    } else {
      DCHECK(cur_oat_quick_method_header_->IsNterpMethodHeader());
      return NterpGetDexPC(cur_quick_frame_);
    }
  } else {
    return 0;
  }
}

std::vector<uint32_t> StackVisitor::ComputeDexPcList(uint32_t handler_dex_pc) const {
  std::vector<uint32_t> result;
  if (cur_shadow_frame_ == nullptr && cur_quick_frame_ != nullptr && IsInInlinedFrame()) {
    const BitTableRange<InlineInfo>& infos = current_inline_frames_;
    DCHECK_NE(infos.size(), 0u);

    // Outermost dex_pc.
    result.push_back(GetCurrentStackMap()->GetDexPc());

    // The mid dex_pcs. Note that we skip the last one since we want to change that for
    // `handler_dex_pc`.
    for (size_t index = 0; index < infos.size() - 1; ++index) {
      result.push_back(infos[index].GetDexPc());
    }
  }

  // The innermost dex_pc has to be the handler dex_pc. In the case of no inline frames, it will be
  // just the one dex_pc. In the case of inlining we will be replacing the innermost InlineInfo's
  // dex_pc with this one.
  result.push_back(handler_dex_pc);
  return result;
}

extern "C" mirror::Object* artQuickGetProxyThisObject(ArtMethod** sp)
    REQUIRES_SHARED(Locks::mutator_lock_);

ObjPtr<mirror::Object> StackVisitor::GetThisObject() const {
  DCHECK_EQ(Runtime::Current()->GetClassLinker()->GetImagePointerSize(), kRuntimePointerSize);
  ArtMethod* m = GetMethod();
  if (m->IsStatic()) {
    return nullptr;
  } else if (m->IsNative()) {
    if (cur_quick_frame_ != nullptr) {
      // The `this` reference is stored in the first out vreg in the caller's frame.
      const size_t frame_size = GetCurrentQuickFrameInfo().FrameSizeInBytes();
      auto* stack_ref = reinterpret_cast<StackReference<mirror::Object>*>(
          reinterpret_cast<uint8_t*>(cur_quick_frame_) + frame_size + sizeof(ArtMethod*));
      return stack_ref->AsMirrorPtr();
    } else {
      return cur_shadow_frame_->GetVRegReference(0);
    }
  } else if (m->IsProxyMethod()) {
    if (cur_quick_frame_ != nullptr) {
      return artQuickGetProxyThisObject(cur_quick_frame_);
    } else {
      return cur_shadow_frame_->GetVRegReference(0);
    }
  } else {
    CodeItemDataAccessor accessor(m->DexInstructionData());
    if (!accessor.HasCodeItem()) {
      UNIMPLEMENTED(ERROR) << "Failed to determine this object of abstract or proxy method: "
          << ArtMethod::PrettyMethod(m);
      return nullptr;
    } else {
      uint16_t reg = accessor.RegistersSize() - accessor.InsSize();
      uint32_t value = 0;
      if (!GetVReg(m, reg, kReferenceVReg, &value)) {
        return nullptr;
      }
      return reinterpret_cast<mirror::Object*>(value);
    }
  }
}

size_t StackVisitor::GetNativePcOffset() const {
  DCHECK(!IsShadowFrame());
  return GetCurrentOatQuickMethodHeader()->NativeQuickPcOffset(cur_quick_frame_pc_);
}

bool StackVisitor::GetVRegFromDebuggerShadowFrame(uint16_t vreg,
                                                  VRegKind kind,
                                                  uint32_t* val) const {
  size_t frame_id = const_cast<StackVisitor*>(this)->GetFrameId();
  ShadowFrame* shadow_frame = thread_->FindDebuggerShadowFrame(frame_id);
  if (shadow_frame != nullptr) {
    bool* updated_vreg_flags = thread_->GetUpdatedVRegFlags(frame_id);
    DCHECK(updated_vreg_flags != nullptr);
    if (updated_vreg_flags[vreg]) {
      // Value is set by the debugger.
      if (kind == kReferenceVReg) {
        *val = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
            shadow_frame->GetVRegReference(vreg)));
      } else {
        *val = shadow_frame->GetVReg(vreg);
      }
      return true;
    }
  }
  // No value is set by the debugger.
  return false;
}

bool StackVisitor::GetVReg(ArtMethod* m,
                           uint16_t vreg,
                           VRegKind kind,
                           uint32_t* val,
                           std::optional<DexRegisterLocation> location,
                           bool need_full_register_list) const {
  if (cur_quick_frame_ != nullptr) {
    DCHECK(context_ != nullptr);  // You can't reliably read registers without a context.
    DCHECK(m == GetMethod());
    // Check if there is value set by the debugger.
    if (GetVRegFromDebuggerShadowFrame(vreg, kind, val)) {
      return true;
    }
    bool result = false;
    if (cur_oat_quick_method_header_->IsNterpMethodHeader()) {
      result = true;
      *val = (kind == kReferenceVReg)
          ? NterpGetVRegReference(cur_quick_frame_, vreg)
          : NterpGetVReg(cur_quick_frame_, vreg);
    } else {
      DCHECK(cur_oat_quick_method_header_->IsOptimized());
      if (location.has_value() && kind != kReferenceVReg) {
        uint32_t val2 = *val;
        // The caller already known the register location, so we can use the faster overload
        // which does not decode the stack maps.
        result = GetVRegFromOptimizedCode(location.value(), val);
        // Compare to the slower overload.
        DCHECK_EQ(result, GetVRegFromOptimizedCode(m, vreg, kind, &val2, need_full_register_list));
        DCHECK_EQ(*val, val2);
      } else {
        result = GetVRegFromOptimizedCode(m, vreg, kind, val, need_full_register_list);
      }
    }
    if (kind == kReferenceVReg) {
      // Perform a read barrier in case we are in a different thread and GC is ongoing.
      mirror::Object* out = reinterpret_cast<mirror::Object*>(static_cast<uintptr_t>(*val));
      uintptr_t ptr_out = reinterpret_cast<uintptr_t>(GcRoot<mirror::Object>(out).Read());
      DCHECK_LT(ptr_out, std::numeric_limits<uint32_t>::max());
      *val = static_cast<uint32_t>(ptr_out);
    }
    return result;
  } else {
    DCHECK(cur_shadow_frame_ != nullptr);
    if (kind == kReferenceVReg) {
      *val = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
          cur_shadow_frame_->GetVRegReference(vreg)));
    } else {
      *val = cur_shadow_frame_->GetVReg(vreg);
    }
    return true;
  }
}

size_t StackVisitor::GetNumberOfRegisters(CodeInfo* code_info, int depth) const {
  return depth == 0
    ? code_info->GetNumberOfDexRegisters()
    : current_inline_frames_[depth - 1].GetNumberOfDexRegisters();
}

bool StackVisitor::GetVRegFromOptimizedCode(ArtMethod* m,
                                            uint16_t vreg,
                                            VRegKind kind,
                                            uint32_t* val,
                                            bool need_full_register_list) const {
  DCHECK_EQ(m, GetMethod());
  // Can't be null or how would we compile its instructions?
  DCHECK(m->GetCodeItem() != nullptr) << m->PrettyMethod();
  const OatQuickMethodHeader* method_header = GetCurrentOatQuickMethodHeader();
  CodeInfo code_info(method_header);

  uint32_t native_pc_offset = method_header->NativeQuickPcOffset(cur_quick_frame_pc_);
  StackMap stack_map = code_info.GetStackMapForNativePcOffset(native_pc_offset);
  DCHECK(stack_map.IsValid());

  DexRegisterMap dex_register_map = (IsInInlinedFrame() && !need_full_register_list)
    ? code_info.GetInlineDexRegisterMapOf(stack_map, current_inline_frames_.back())
    : code_info.GetDexRegisterMapOf(stack_map,
                                    /* first= */ 0,
                                    GetNumberOfRegisters(&code_info, InlineDepth()));

  if (dex_register_map.empty()) {
    return false;
  }

  const size_t number_of_dex_registers = dex_register_map.size();
  DCHECK_LT(vreg, number_of_dex_registers);
  DexRegisterLocation::Kind location_kind = dex_register_map[vreg].GetKind();
  switch (location_kind) {
    case DexRegisterLocation::Kind::kInStack: {
      const int32_t offset = dex_register_map[vreg].GetStackOffsetInBytes();
      BitMemoryRegion stack_mask = code_info.GetStackMaskOf(stack_map);
      if (kind == kReferenceVReg && !stack_mask.LoadBit(offset / kFrameSlotSize)) {
        return false;
      }
      const uint8_t* addr = reinterpret_cast<const uint8_t*>(cur_quick_frame_) + offset;
      *val = *reinterpret_cast<const uint32_t*>(addr);
      return true;
    }
    case DexRegisterLocation::Kind::kInRegister: {
      uint32_t register_mask = code_info.GetRegisterMaskOf(stack_map);
      uint32_t reg = dex_register_map[vreg].GetMachineRegister();
      if (kind == kReferenceVReg && !(register_mask & (1 << reg))) {
        return false;
      }
      return GetRegisterIfAccessible(reg, location_kind, val);
    }
    case DexRegisterLocation::Kind::kInRegisterHigh:
    case DexRegisterLocation::Kind::kInFpuRegister:
    case DexRegisterLocation::Kind::kInFpuRegisterHigh: {
      if (kind == kReferenceVReg) {
        return false;
      }
      uint32_t reg = dex_register_map[vreg].GetMachineRegister();
      return GetRegisterIfAccessible(reg, location_kind, val);
    }
    case DexRegisterLocation::Kind::kConstant: {
      uint32_t result = dex_register_map[vreg].GetConstant();
      if (kind == kReferenceVReg && result != 0) {
        return false;
      }
      *val = result;
      return true;
    }
    case DexRegisterLocation::Kind::kNone:
      return false;
    default:
      LOG(FATAL) << "Unexpected location kind " << dex_register_map[vreg].GetKind();
      UNREACHABLE();
  }
}

bool StackVisitor::GetVRegFromOptimizedCode(DexRegisterLocation location, uint32_t* val) const {
  switch (location.GetKind()) {
    case DexRegisterLocation::Kind::kInvalid:
      break;
    case DexRegisterLocation::Kind::kInStack: {
      const uint8_t* sp = reinterpret_cast<const uint8_t*>(cur_quick_frame_);
      *val = *reinterpret_cast<const uint32_t*>(sp + location.GetStackOffsetInBytes());
      return true;
    }
    case DexRegisterLocation::Kind::kInRegister:
    case DexRegisterLocation::Kind::kInRegisterHigh:
    case DexRegisterLocation::Kind::kInFpuRegister:
    case DexRegisterLocation::Kind::kInFpuRegisterHigh:
      return GetRegisterIfAccessible(location.GetMachineRegister(), location.GetKind(), val);
    case DexRegisterLocation::Kind::kConstant:
      *val = location.GetConstant();
      return true;
    case DexRegisterLocation::Kind::kNone:
      return false;
  }
  LOG(FATAL) << "Unexpected location kind " << location.GetKind();
  UNREACHABLE();
}

bool StackVisitor::GetRegisterIfAccessible(uint32_t reg,
                                           DexRegisterLocation::Kind location_kind,
                                           uint32_t* val) const {
  const bool is_float = (location_kind == DexRegisterLocation::Kind::kInFpuRegister) ||
                        (location_kind == DexRegisterLocation::Kind::kInFpuRegisterHigh);

  if (kRuntimeISA == InstructionSet::kX86 && is_float) {
    // X86 float registers are 64-bit and each XMM register is provided as two separate
    // 32-bit registers by the context.
    reg = (location_kind == DexRegisterLocation::Kind::kInFpuRegisterHigh)
        ? (2 * reg + 1)
        : (2 * reg);
  }

  if (!IsAccessibleRegister(reg, is_float)) {
    return false;
  }
  uintptr_t ptr_val = GetRegister(reg, is_float);
  const bool target64 = Is64BitInstructionSet(kRuntimeISA);
  if (target64) {
    const bool is_high = (location_kind == DexRegisterLocation::Kind::kInRegisterHigh) ||
                         (location_kind == DexRegisterLocation::Kind::kInFpuRegisterHigh);
    int64_t value_long = static_cast<int64_t>(ptr_val);
    ptr_val = static_cast<uintptr_t>(is_high ? High32Bits(value_long) : Low32Bits(value_long));
  }
  *val = ptr_val;
  return true;
}

bool StackVisitor::GetVRegPairFromDebuggerShadowFrame(uint16_t vreg,
                                                      VRegKind kind_lo,
                                                      VRegKind kind_hi,
                                                      uint64_t* val) const {
  uint32_t low_32bits;
  uint32_t high_32bits;
  bool success = GetVRegFromDebuggerShadowFrame(vreg, kind_lo, &low_32bits);
  success &= GetVRegFromDebuggerShadowFrame(vreg + 1, kind_hi, &high_32bits);
  if (success) {
    *val = (static_cast<uint64_t>(high_32bits) << 32) | static_cast<uint64_t>(low_32bits);
  }
  return success;
}

bool StackVisitor::GetVRegPair(ArtMethod* m, uint16_t vreg, VRegKind kind_lo,
                               VRegKind kind_hi, uint64_t* val) const {
  if (kind_lo == kLongLoVReg) {
    DCHECK_EQ(kind_hi, kLongHiVReg);
  } else if (kind_lo == kDoubleLoVReg) {
    DCHECK_EQ(kind_hi, kDoubleHiVReg);
  } else {
    LOG(FATAL) << "Expected long or double: kind_lo=" << kind_lo << ", kind_hi=" << kind_hi;
    UNREACHABLE();
  }
  // Check if there is value set by the debugger.
  if (GetVRegPairFromDebuggerShadowFrame(vreg, kind_lo, kind_hi, val)) {
    return true;
  }
  if (cur_quick_frame_ == nullptr) {
    DCHECK(cur_shadow_frame_ != nullptr);
    *val = cur_shadow_frame_->GetVRegLong(vreg);
    return true;
  }
  if (cur_oat_quick_method_header_->IsNterpMethodHeader()) {
    uint64_t val_lo = NterpGetVReg(cur_quick_frame_, vreg);
    uint64_t val_hi = NterpGetVReg(cur_quick_frame_, vreg + 1);
    *val = (val_hi << 32) + val_lo;
    return true;
  }

  DCHECK(context_ != nullptr);  // You can't reliably read registers without a context.
  DCHECK(m == GetMethod());
  DCHECK(cur_oat_quick_method_header_->IsOptimized());
  return GetVRegPairFromOptimizedCode(m, vreg, kind_lo, kind_hi, val);
}

bool StackVisitor::GetVRegPairFromOptimizedCode(ArtMethod* m, uint16_t vreg,
                                                VRegKind kind_lo, VRegKind kind_hi,
                                                uint64_t* val) const {
  uint32_t low_32bits;
  uint32_t high_32bits;
  bool success = GetVRegFromOptimizedCode(m, vreg, kind_lo, &low_32bits);
  success &= GetVRegFromOptimizedCode(m, vreg + 1, kind_hi, &high_32bits);
  if (success) {
    *val = (static_cast<uint64_t>(high_32bits) << 32) | static_cast<uint64_t>(low_32bits);
  }
  return success;
}

ShadowFrame* StackVisitor::PrepareSetVReg(ArtMethod* m, uint16_t vreg, bool wide) {
  CodeItemDataAccessor accessor(m->DexInstructionData());
  if (!accessor.HasCodeItem()) {
    return nullptr;
  }
  ShadowFrame* shadow_frame = GetCurrentShadowFrame();
  if (shadow_frame == nullptr) {
    // This is a compiled frame: we must prepare and update a shadow frame that will
    // be executed by the interpreter after deoptimization of the stack.
    const size_t frame_id = GetFrameId();
    const uint16_t num_regs = accessor.RegistersSize();
    shadow_frame = thread_->FindOrCreateDebuggerShadowFrame(frame_id, num_regs, m, GetDexPc());
    CHECK(shadow_frame != nullptr);
    // Remember the vreg(s) has been set for debugging and must not be overwritten by the
    // original value during deoptimization of the stack.
    thread_->GetUpdatedVRegFlags(frame_id)[vreg] = true;
    if (wide) {
      thread_->GetUpdatedVRegFlags(frame_id)[vreg + 1] = true;
    }
  }
  return shadow_frame;
}

bool StackVisitor::SetVReg(ArtMethod* m, uint16_t vreg, uint32_t new_value, VRegKind kind) {
  DCHECK(kind == kIntVReg || kind == kFloatVReg);
  ShadowFrame* shadow_frame = PrepareSetVReg(m, vreg, /* wide= */ false);
  if (shadow_frame == nullptr) {
    return false;
  }
  shadow_frame->SetVReg(vreg, new_value);
  return true;
}

bool StackVisitor::SetVRegReference(ArtMethod* m, uint16_t vreg, ObjPtr<mirror::Object> new_value) {
  ShadowFrame* shadow_frame = PrepareSetVReg(m, vreg, /* wide= */ false);
  if (shadow_frame == nullptr) {
    return false;
  }
  shadow_frame->SetVRegReference(vreg, new_value);
  return true;
}

bool StackVisitor::SetVRegPair(ArtMethod* m,
                               uint16_t vreg,
                               uint64_t new_value,
                               VRegKind kind_lo,
                               VRegKind kind_hi) {
  if (kind_lo == kLongLoVReg) {
    DCHECK_EQ(kind_hi, kLongHiVReg);
  } else if (kind_lo == kDoubleLoVReg) {
    DCHECK_EQ(kind_hi, kDoubleHiVReg);
  } else {
    LOG(FATAL) << "Expected long or double: kind_lo=" << kind_lo << ", kind_hi=" << kind_hi;
    UNREACHABLE();
  }
  ShadowFrame* shadow_frame = PrepareSetVReg(m, vreg, /* wide= */ true);
  if (shadow_frame == nullptr) {
    return false;
  }
  shadow_frame->SetVRegLong(vreg, new_value);
  return true;
}

bool StackVisitor::IsAccessibleGPR(uint32_t reg) const {
  DCHECK(context_ != nullptr);
  return context_->IsAccessibleGPR(reg);
}

uintptr_t* StackVisitor::GetGPRAddress(uint32_t reg) const {
  DCHECK(cur_quick_frame_ != nullptr) << "This is a quick frame routine";
  DCHECK(context_ != nullptr);
  return context_->GetGPRAddress(reg);
}

uintptr_t StackVisitor::GetGPR(uint32_t reg) const {
  DCHECK(cur_quick_frame_ != nullptr) << "This is a quick frame routine";
  DCHECK(context_ != nullptr);
  return context_->GetGPR(reg);
}

bool StackVisitor::IsAccessibleFPR(uint32_t reg) const {
  DCHECK(context_ != nullptr);
  return context_->IsAccessibleFPR(reg);
}

uintptr_t StackVisitor::GetFPR(uint32_t reg) const {
  DCHECK(cur_quick_frame_ != nullptr) << "This is a quick frame routine";
  DCHECK(context_ != nullptr);
  return context_->GetFPR(reg);
}

uintptr_t StackVisitor::GetReturnPcAddr() const {
  uintptr_t sp = reinterpret_cast<uintptr_t>(GetCurrentQuickFrame());
  DCHECK_NE(sp, 0u);
  return sp + GetCurrentQuickFrameInfo().GetReturnPcOffset();
}

uintptr_t StackVisitor::GetReturnPc() const {
  return *reinterpret_cast<uintptr_t*>(GetReturnPcAddr());
}

void StackVisitor::SetReturnPc(uintptr_t new_ret_pc) {
  *reinterpret_cast<uintptr_t*>(GetReturnPcAddr()) = new_ret_pc;
}

size_t StackVisitor::ComputeNumFrames(Thread* thread, StackWalkKind walk_kind) {
  struct NumFramesVisitor : public StackVisitor {
    NumFramesVisitor(Thread* thread_in, StackWalkKind walk_kind_in)
        : StackVisitor(thread_in, nullptr, walk_kind_in), frames(0) {}

    bool VisitFrame() override {
      frames++;
      return true;
    }

    size_t frames;
  };
  NumFramesVisitor visitor(thread, walk_kind);
  visitor.WalkStack(true);
  return visitor.frames;
}

bool StackVisitor::GetNextMethodAndDexPc(ArtMethod** next_method, uint32_t* next_dex_pc) {
  struct HasMoreFramesVisitor : public StackVisitor {
    HasMoreFramesVisitor(Thread* thread,
                         StackWalkKind walk_kind,
                         size_t num_frames,
                         size_t frame_height)
        : StackVisitor(thread, nullptr, walk_kind, num_frames),
          frame_height_(frame_height),
          found_frame_(false),
          has_more_frames_(false),
          next_method_(nullptr),
          next_dex_pc_(0) {
    }

    bool VisitFrame() override REQUIRES_SHARED(Locks::mutator_lock_) {
      if (found_frame_) {
        ArtMethod* method = GetMethod();
        if (method != nullptr && !method->IsRuntimeMethod()) {
          has_more_frames_ = true;
          next_method_ = method;
          next_dex_pc_ = GetDexPc();
          return false;  // End stack walk once next method is found.
        }
      } else if (GetFrameHeight() == frame_height_) {
        found_frame_ = true;
      }
      return true;
    }

    size_t frame_height_;
    bool found_frame_;
    bool has_more_frames_;
    ArtMethod* next_method_;
    uint32_t next_dex_pc_;
  };
  HasMoreFramesVisitor visitor(thread_, walk_kind_, GetNumFrames(), GetFrameHeight());
  visitor.WalkStack(true);
  *next_method = visitor.next_method_;
  *next_dex_pc = visitor.next_dex_pc_;
  return visitor.has_more_frames_;
}

void StackVisitor::DescribeStack(Thread* thread) {
  struct DescribeStackVisitor : public StackVisitor {
    explicit DescribeStackVisitor(Thread* thread_in)
        : StackVisitor(thread_in, nullptr, StackVisitor::StackWalkKind::kIncludeInlinedFrames) {}

    bool VisitFrame() override REQUIRES_SHARED(Locks::mutator_lock_) {
      LOG(INFO) << "Frame Id=" << GetFrameId() << " " << DescribeLocation();
      return true;
    }
  };
  DescribeStackVisitor visitor(thread);
  visitor.WalkStack(true);
}

std::string StackVisitor::DescribeLocation() const {
  std::string result("Visiting method '");
  ArtMethod* m = GetMethod();
  if (m == nullptr) {
    return "upcall";
  }
  result += m->PrettyMethod();
  result += StringPrintf("' at dex PC 0x%04x", GetDexPc());
  if (!IsShadowFrame()) {
    result += StringPrintf(" (native PC %p)", reinterpret_cast<void*>(GetCurrentQuickFramePc()));
  }
  return result;
}

void StackVisitor::SetMethod(ArtMethod* method) {
  DCHECK(GetMethod() != nullptr);
  if (cur_shadow_frame_ != nullptr) {
    cur_shadow_frame_->SetMethod(method);
  } else {
    DCHECK(cur_quick_frame_ != nullptr);
    CHECK(!IsInInlinedFrame()) << "We do not support setting inlined method's ArtMethod: "
                               << GetMethod()->PrettyMethod() << " is inlined into "
                               << GetOuterMethod()->PrettyMethod();
    *cur_quick_frame_ = method;
  }
}

void StackVisitor::ValidateFrame() const {
  if (!kIsDebugBuild) {
    return;
  }
  ArtMethod* method = GetMethod();
  ObjPtr<mirror::Class> declaring_class = method->GetDeclaringClass();
  // Runtime methods have null declaring class.
  if (!method->IsRuntimeMethod()) {
    CHECK(declaring_class != nullptr);
    CHECK_EQ(declaring_class->GetClass(), declaring_class->GetClass()->GetClass())
        << declaring_class;
  } else {
    CHECK(declaring_class == nullptr);
  }
  Runtime* const runtime = Runtime::Current();
  LinearAlloc* const linear_alloc = runtime->GetLinearAlloc();
  if (!linear_alloc->Contains(method)) {
    // Check class linker linear allocs.
    // We get the canonical method as copied methods may have been allocated
    // by a different class loader.
    const PointerSize ptrSize = runtime->GetClassLinker()->GetImagePointerSize();
    ArtMethod* canonical = method->GetCanonicalMethod(ptrSize);
    ObjPtr<mirror::Class> klass = canonical->GetDeclaringClass();
    LinearAlloc* const class_linear_alloc = (klass != nullptr)
        ? runtime->GetClassLinker()->GetAllocatorForClassLoader(klass->GetClassLoader())
        : linear_alloc;
    if (!class_linear_alloc->Contains(canonical)) {
      // Check image space.
      bool in_image = false;
      for (auto& space : runtime->GetHeap()->GetContinuousSpaces()) {
        if (space->IsImageSpace()) {
          auto* image_space = space->AsImageSpace();
          const auto& header = image_space->GetImageHeader();
          const ImageSection& methods = header.GetMethodsSection();
          const ImageSection& runtime_methods = header.GetRuntimeMethodsSection();
          const size_t offset =  reinterpret_cast<const uint8_t*>(canonical) - image_space->Begin();
          if (methods.Contains(offset) || runtime_methods.Contains(offset)) {
            in_image = true;
            break;
          }
        }
      }
      CHECK(in_image) << canonical->PrettyMethod() << " not in linear alloc or image";
    }
  }
  if (cur_quick_frame_ != nullptr) {
    // Frame consistency checks.
    size_t frame_size = GetCurrentQuickFrameInfo().FrameSizeInBytes();
    CHECK_NE(frame_size, 0u);
    // For compiled code, we could try to have a rough guess at an upper size we expect
    // to see for a frame:
    // 256 registers
    // 2 words HandleScope overhead
    // 3+3 register spills
    // const size_t kMaxExpectedFrameSize = (256 + 2 + 3 + 3) * sizeof(word);
    const size_t kMaxExpectedFrameSize = interpreter::kNterpMaxFrame;
    CHECK_LE(frame_size, kMaxExpectedFrameSize) << method->PrettyMethod();
    size_t return_pc_offset = GetCurrentQuickFrameInfo().GetReturnPcOffset();
    CHECK_LT(return_pc_offset, frame_size);
  }
}

QuickMethodFrameInfo StackVisitor::GetCurrentQuickFrameInfo() const {
  if (cur_oat_quick_method_header_ != nullptr) {
    if (cur_oat_quick_method_header_->IsOptimized()) {
      return cur_oat_quick_method_header_->GetFrameInfo();
    } else {
      DCHECK(cur_oat_quick_method_header_->IsNterpMethodHeader());
      return NterpFrameInfo(cur_quick_frame_);
    }
  }

  ArtMethod* method = GetMethod();
  Runtime* runtime = Runtime::Current();

  if (method->IsAbstract()) {
    return RuntimeCalleeSaveFrame::GetMethodFrameInfo(CalleeSaveType::kSaveRefsAndArgs);
  }

  // This goes before IsProxyMethod since runtime methods have a null declaring class.
  if (method->IsRuntimeMethod()) {
    return runtime->GetRuntimeMethodFrameInfo(method);
  }

  if (method->IsProxyMethod()) {
    // There is only one direct method of a proxy class: the constructor. A direct method is
    // cloned from the original java.lang.reflect.Proxy and is executed as usual quick
    // compiled method without any stubs. Therefore the method must have a OatQuickMethodHeader.
    DCHECK(!method->IsDirect() && !method->IsConstructor())
        << "Constructors of proxy classes must have a OatQuickMethodHeader";
    return RuntimeCalleeSaveFrame::GetMethodFrameInfo(CalleeSaveType::kSaveRefsAndArgs);
  }

  // The only remaining cases are for native methods that either
  //   - use the Generic JNI stub, called either directly or through some
  //     (resolution, instrumentation) trampoline; or
  //   - fake a Generic JNI frame in art_jni_dlsym_lookup_critical_stub.
  DCHECK(method->IsNative());
  // Generic JNI frame is just like the SaveRefsAndArgs frame.
  // Note that HandleScope, if any, is below the frame.
  return RuntimeCalleeSaveFrame::GetMethodFrameInfo(CalleeSaveType::kSaveRefsAndArgs);
}

uint8_t* StackVisitor::GetShouldDeoptimizeFlagAddr() const REQUIRES_SHARED(Locks::mutator_lock_) {
  DCHECK(GetCurrentOatQuickMethodHeader()->HasShouldDeoptimizeFlag());
  QuickMethodFrameInfo frame_info = GetCurrentQuickFrameInfo();
  size_t frame_size = frame_info.FrameSizeInBytes();
  uint8_t* sp = reinterpret_cast<uint8_t*>(GetCurrentQuickFrame());
  size_t core_spill_size =
      POPCOUNT(frame_info.CoreSpillMask()) * GetBytesPerGprSpillLocation(kRuntimeISA);
  size_t fpu_spill_size =
      POPCOUNT(frame_info.FpSpillMask()) * GetBytesPerFprSpillLocation(kRuntimeISA);
  size_t offset = frame_size - core_spill_size - fpu_spill_size - kShouldDeoptimizeFlagSize;
  uint8_t* should_deoptimize_addr = sp + offset;
  DCHECK_EQ(*should_deoptimize_addr & ~static_cast<uint8_t>(DeoptimizeFlagValue::kAll), 0);
  return should_deoptimize_addr;
}

template <StackVisitor::CountTransitions kCount>
void StackVisitor::WalkStack(bool include_transitions) {
  if (check_suspended_) {
    DCHECK(thread_ == Thread::Current() || thread_->GetState() != ThreadState::kRunnable);
  }
  CHECK_EQ(cur_depth_, 0U);

  for (const ManagedStack* current_fragment = thread_->GetManagedStack();
       current_fragment != nullptr; current_fragment = current_fragment->GetLink()) {
    cur_shadow_frame_ = current_fragment->GetTopShadowFrame();
    cur_quick_frame_ = current_fragment->GetTopQuickFrame();
    cur_quick_frame_pc_ = 0;
    DCHECK(cur_oat_quick_method_header_ == nullptr);

    if (kDebugStackWalk) {
      LOG(INFO) << "Tid=" << thread_-> GetThreadId()
          << ", ManagedStack fragement: " << current_fragment;
    }

    if (cur_quick_frame_ != nullptr) {  // Handle quick stack frames.
      // Can't be both a shadow and a quick fragment.
      DCHECK(current_fragment->GetTopShadowFrame() == nullptr);
      ArtMethod* method = *cur_quick_frame_;
      DCHECK(method != nullptr);
      bool header_retrieved = false;
      if (method->IsNative()) {
        // We do not have a PC for the first frame, so we cannot simply use
        // ArtMethod::GetOatQuickMethodHeader() as we're unable to distinguish there
        // between GenericJNI frame and JIT-compiled JNI stub; the entrypoint may have
        // changed since the frame was entered. The top quick frame tag indicates
        // GenericJNI here, otherwise it's either AOT-compiled or JNI-compiled JNI stub.
        if (UNLIKELY(current_fragment->GetTopQuickFrameGenericJniTag())) {
          // The generic JNI does not have any method header.
          cur_oat_quick_method_header_ = nullptr;
        } else if (UNLIKELY(current_fragment->GetTopQuickFrameJitJniTag())) {
          // Should be JITed code.
          Runtime* runtime = Runtime::Current();
          const void* code = runtime->GetJit()->GetCodeCache()->GetJniStubCode(method);
          CHECK(code != nullptr) << method->PrettyMethod();
          cur_oat_quick_method_header_ = OatQuickMethodHeader::FromCodePointer(code);
        } else {
          // We are sure we are not running GenericJni here. Though the entry point could still be
          // GenericJnistub. The entry point is usually JITed or AOT code. It could be also a
          // resolution stub if the class isn't visibly initialized yet.
          const void* existing_entry_point = method->GetEntryPointFromQuickCompiledCode();
          CHECK(existing_entry_point != nullptr);
          Runtime* runtime = Runtime::Current();
          ClassLinker* class_linker = runtime->GetClassLinker();
          // Check whether we can quickly get the header from the current entrypoint.
          if (!class_linker->IsQuickGenericJniStub(existing_entry_point) &&
              !class_linker->IsQuickResolutionStub(existing_entry_point)) {
            cur_oat_quick_method_header_ =
                OatQuickMethodHeader::FromEntryPoint(existing_entry_point);
          } else {
            const void* code = method->GetOatMethodQuickCode(class_linker->GetImagePointerSize());
            if (code != nullptr) {
              cur_oat_quick_method_header_ = OatQuickMethodHeader::FromEntryPoint(code);
            } else {
              // For non-debuggable runtimes, the JNI stub can be JIT-compiled or AOT-compiled, and
              // can also reuse the stub in boot images. Since we checked for AOT code earlier, we
              // must be running JITed code or boot JNI stub.
              // For debuggable runtimes, we won't be here as we never use AOT code in debuggable.
              // And the JIT situation is handled earlier as its SP will be tagged. But there is a
              // special case where we change runtime state from non-debuggable to debuggable in
              // the JNI implementation and do deopt inside, which could be treated as
              // a case of non-debuggable as well.
              if (runtime->GetJit() != nullptr) {
                code = runtime->GetJit()->GetCodeCache()->GetJniStubCode(method);
              }
              if (code == nullptr) {
                // Check if current method uses the boot JNI stub.
                const void* boot_jni_stub = class_linker->FindBootJniStub(method);
                if (boot_jni_stub != nullptr) {
                  code = EntryPointToCodePointer(boot_jni_stub);
                }
              }
              CHECK(code != nullptr) << method->PrettyMethod();
              cur_oat_quick_method_header_ = OatQuickMethodHeader::FromCodePointer(code);
            }
          }
        }
        header_retrieved = true;
      }
      while (method != nullptr) {
        if (!header_retrieved) {
          cur_oat_quick_method_header_ = method->GetOatQuickMethodHeader(cur_quick_frame_pc_);
        }
        header_retrieved = false;  // Force header retrieval in next iteration.

        if (kDebugStackWalk) {
          LOG(INFO) << "Early print: Tid=" << thread_-> GetThreadId() << ", method: "
              << ArtMethod::PrettyMethod(method) << "@" << method;
        }
        ValidateFrame();
        if ((walk_kind_ == StackWalkKind::kIncludeInlinedFrames)
            && (cur_oat_quick_method_header_ != nullptr)
            && cur_oat_quick_method_header_->IsOptimized()
            && !method->IsNative()  // JNI methods cannot have any inlined frames.
            && CodeInfo::HasInlineInfo(cur_oat_quick_method_header_->GetOptimizedCodeInfoPtr())) {
          DCHECK_NE(cur_quick_frame_pc_, 0u);
          CodeInfo* code_info = GetCurrentInlineInfo();
          StackMap* stack_map = GetCurrentStackMap();
          if (stack_map->IsValid() && stack_map->HasInlineInfo()) {
            DCHECK_EQ(current_inline_frames_.size(), 0u);
            for (current_inline_frames_ = code_info->GetInlineInfosOf(*stack_map);
                 !current_inline_frames_.empty();
                 current_inline_frames_.pop_back()) {
              bool should_continue = VisitFrame();
              if (UNLIKELY(!should_continue)) {
                return;
              }
              cur_depth_++;
            }
          }
        }

        bool should_continue = VisitFrame();
        if (UNLIKELY(!should_continue)) {
          return;
        }

        QuickMethodFrameInfo frame_info = GetCurrentQuickFrameInfo();
        if (context_ != nullptr) {
          context_->FillCalleeSaves(reinterpret_cast<uint8_t*>(cur_quick_frame_), frame_info);
        }
        // Compute PC for next stack frame from return PC.
        size_t frame_size = frame_info.FrameSizeInBytes();
        uintptr_t return_pc_addr = GetReturnPcAddr();

        cur_quick_frame_pc_ = *reinterpret_cast<uintptr_t*>(return_pc_addr);
        uint8_t* next_frame = reinterpret_cast<uint8_t*>(cur_quick_frame_) + frame_size;
        cur_quick_frame_ = reinterpret_cast<ArtMethod**>(next_frame);

        if (kDebugStackWalk) {
          LOG(INFO) << "Tid=" << thread_-> GetThreadId() << ", method: "
              << ArtMethod::PrettyMethod(method) << "@" << method << " size=" << frame_size
              << std::boolalpha
              << " optimized=" << (cur_oat_quick_method_header_ != nullptr &&
                                   cur_oat_quick_method_header_->IsOptimized())
              << " native=" << method->IsNative()
              << std::noboolalpha
              << " entrypoints=" << method->GetEntryPointFromQuickCompiledCode()
              << "," << (method->IsNative() ? method->GetEntryPointFromJni() : nullptr)
              << " next=" << *cur_quick_frame_;
        }

        if (kCount == CountTransitions::kYes || !method->IsRuntimeMethod()) {
          cur_depth_++;
        }
        method = *cur_quick_frame_;
      }
      // We reached a transition frame, it doesn't have a method header.
      cur_oat_quick_method_header_ = nullptr;
    } else if (cur_shadow_frame_ != nullptr) {
      do {
        if (kDebugStackWalk) {
          ArtMethod* method = cur_shadow_frame_->GetMethod();
          LOG(INFO) << "Tid=" << thread_-> GetThreadId() << ", method: "
              << ArtMethod::PrettyMethod(method) << "@" << method
              << ", ShadowFrame";
        }
        ValidateFrame();
        bool should_continue = VisitFrame();
        if (UNLIKELY(!should_continue)) {
          return;
        }
        cur_depth_++;
        cur_shadow_frame_ = cur_shadow_frame_->GetLink();
      } while (cur_shadow_frame_ != nullptr);
    }
    if (include_transitions) {
      bool should_continue = VisitFrame();
      if (!should_continue) {
        return;
      }
    }
    if (kCount == CountTransitions::kYes) {
      cur_depth_++;
    }
  }
  if (num_frames_ != 0) {
    CHECK_EQ(cur_depth_, num_frames_);
  }
}

template void StackVisitor::WalkStack<StackVisitor::CountTransitions::kYes>(bool);
template void StackVisitor::WalkStack<StackVisitor::CountTransitions::kNo>(bool);

}  // namespace art
