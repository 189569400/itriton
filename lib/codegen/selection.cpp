﻿#include "triton/codegen/selection.h"
#include "triton/codegen/tune.h"
#include "triton/codegen/shmem_allocation.h"
#include "triton/codegen/target.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include "triton/ir/context.h"
#include "triton/ir/module.h"
#include "triton/ir/function.h"
#include "triton/ir/type.h"
#include "llvm/Transforms/Scalar/EarlyCSE.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/InlineAsm.h"

namespace triton{
namespace codegen{

using namespace llvm;

/* Distributed Tile */
void distributed_tile::init_indices() {
  std::vector<size_t> id(axes_.size(), 0);
  size_t k = 0;
  while(true) {
    indices_t current;
    for(size_t d = 0; d < id.size(); d++)
      current.push_back(axes_[d].values[id[d]]);
    size_t sz = indices_.size();
    indices_[current] = sz;
    values_[current] = UndefValue::get(ty_);
    ordered_indices_.push_back(current);
    id[0]++;
    while(id[k] == axes_[k].values.size()){
      if(k == id.size() - 1)
        return;
      id[k++] = 0;
      id[k]++;
    }
    k = 0;
  }
}

llvm::Type *distributed_tile::make_vector_ty(llvm::Type *ty, size_t vector_size) {
  if(vector_size == 1)
    return ty;
  return VectorType::get(ty, vector_size);
}

distributed_tile::distributed_tile(Type *ty, const shapes_t &shapes, const axes_t &axes, llvm::IRBuilder<> &builder, bool vectorize)
    : tile(make_vector_ty(ty, vectorize?axes[0].contiguous:1), shapes), axes_(axes), builder_(builder) {
  vector_size_ = vectorize?ty_->getVectorNumElements():1;
  init_indices();
}

void distributed_tile::set_value(indices_t idx, Value *v) {
  values_[idx] = v;
}

Value* distributed_tile::get_value(indices_t idx) {
  return values_[idx];
}

unsigned distributed_tile::get_linear_index(indices_t idx) {
  return indices_[idx];
}

void distributed_tile::for_each(std::function<void (indices_t)> fn) {
  for(unsigned i = 0; i < ordered_indices_.size(); i++)
    if(i % vector_size_ == 0)
      fn(ordered_indices_[i]);
}

/* Shared Tile */
void shared_tile::extract_constant(Value *arg, Value *&non_cst, Value *&cst) {
  BinaryOperator *bin_op = dyn_cast<BinaryOperator>(arg);
  Constant *_0 = ConstantInt::get(Type::getInt32Ty(arg->getContext()), 0);
  if(dyn_cast<Constant>(arg)){
    cst = arg;
    non_cst = _0;
    return;
  }
  if(!bin_op || bin_op->getOpcode() != llvm::BinaryOperator::Add){
    non_cst = arg;
    cst = _0;
    return;
  }
  Constant *cst_lhs = dyn_cast<Constant>(bin_op->getOperand(0));
  Constant *cst_rhs = dyn_cast<Constant>(bin_op->getOperand(1));
  if(cst_lhs && cst_rhs){
    cst = arg;
    non_cst = _0;
  }
  else if(cst_lhs){
    cst = cst_lhs;
    non_cst = bin_op->getOperand(1);
  }
  else if(cst_rhs){
    cst = cst_rhs;
    non_cst = bin_op->getOperand(0);
  }
  else{
    non_cst = arg;
    cst = _0;
  }
}

void shared_tile::extract_constant(const indices_t &arg_idx, indices_t &non_cst_idx, indices_t &cst_idx) {
  non_cst_idx.clear();
  cst_idx.clear();
  for(Value *idx: arg_idx){
    Value *non_cst, *cst;
    extract_constant(idx, non_cst, cst);
    non_cst_idx.push_back(non_cst);
    cst_idx.push_back(cst);
  }
}


Value* shared_tile::shared_offset(indices_t idx) {
  Value *result = builder_.getInt32(0);
  result = builder_.CreateAdd(result, idx[0]);
  for(size_t i = 1; i < idx.size(); i++)
    result = builder_.CreateAdd(result, builder_.CreateMul(idx[i], builder_.getInt32(shapes_[i-1])));
  return result;
}

shared_tile::shared_tile(Type *ty, const shapes_t &shapes, Value *ptr, llvm::IRBuilder<> &builder, Value *offset):
  tile(ty, shapes), ptr_(ptr), builder_(builder), offset_(offset), vector_size_(1){
  return_vector_ = false;
}

void shared_tile::set_value(indices_t idx, Value *value) {
  Value *ptr = builder_.CreateGEP(ptr_, shared_offset(idx));
  unsigned addr_space = ptr->getType()->getPointerAddressSpace();
  ptr = builder_.CreateBitCast(ptr, value->getType()->getPointerTo(addr_space));
  builder_.CreateStore(value, ptr);
}

void shared_tile::set_vector_size(unsigned vector_size) {
  vector_size_ = vector_size;
}

void shared_tile::set_return_mode(bool return_vector){
  return_vector_ = return_vector;
}


Value* shared_tile::get_value(indices_t idx) {
  indices_t non_cst_idx, cst_idx;
  extract_constant(idx, non_cst_idx, cst_idx);
  Value *&base_ptr = ptr_cache_[non_cst_idx];
  unsigned vector_size = vector_size_;
  Type *ty = ty_;
  if(ty->isHalfTy() && (vector_size % 2 == 0)){
    ty = IntegerType::get(ty->getContext(), 32);
    vector_size = vector_size / 2;
  }
  if(base_ptr == nullptr){
//    BasicBlock* store = builder_.GetInsertBlock();
//    if(!non_cst_idx.empty())
//    if(isa<Instruction>(non_cst_idx.front())){
//      builder_.SetInsertPoint((Instruction*)non_cst_idx.front());
//    }
    base_ptr = builder_.CreateGEP(ptr_, shared_offset(non_cst_idx));
    if(vector_size_ > 1){
      Type *vec_ty = VectorType::get(ty, vector_size);
      Type *vec_ptr_ty = PointerType::get(vec_ty, base_ptr->getType()->getPointerAddressSpace());
      base_ptr = builder_.CreateBitCast(base_ptr, vec_ptr_ty);
    }
//    builder_.SetInsertPoint(store);
  }
  Value *offset = shared_offset(cst_idx);
  Value *div = offset;
  if(vector_size_ > 1)
    div = builder_.CreateUDiv(offset, builder_.getInt32(vector_size_));
  Value *ptr = builder_.CreateGEP(base_ptr, div);
  Value *result = builder_.CreateLoad(ptr);
  if(return_vector_ == false && vector_size_ > 1) {
    Value *rem = builder_.CreateURem(offset, builder_.getInt32(vector_size_));
    result = builder_.CreateExtractElement(result, rem);
  }
  return result;
}

/* convert ir::type to Type */
Type *selection::llvm_type(ir::type *ty, LLVMContext &ctx) {
  // function
  if(auto* tt = dynamic_cast<ir::function_type*>(ty)){
    Type *return_ty = llvm_type(tt->get_return_ty(), ctx);
    std::vector<Type*> param_tys;
    std::transform(tt->params_begin(), tt->params_end(), std::back_inserter(param_tys),
                   [this,&ctx](ir::type* t){ return llvm_type(t, ctx);});
    return FunctionType::get(return_ty, param_tys, false);
  }
  // pointer
  if(ty->is_pointer_ty()){
    Type *elt_ty = llvm_type(ty->get_pointer_element_ty(), ctx);
    unsigned addr_space = ty->get_pointer_address_space();
    return PointerType::get(elt_ty, addr_space);
  }
  // integer
  if(ty->is_integer_ty()){
    unsigned bitwidth = ty->get_integer_bitwidth();
    return IntegerType::get(ctx, bitwidth);
  }
  // primitive types
  switch(ty->get_type_id()){
    case ir::type::VoidTyID:      return Type::getVoidTy(ctx);
    case ir::type::HalfTyID:      return Type::getHalfTy(ctx);
    case ir::type::FloatTyID:     return Type::getFloatTy(ctx);
    case ir::type::DoubleTyID:    return Type::getDoubleTy(ctx);
    case ir::type::X86_FP80TyID:  return Type::getX86_FP80Ty(ctx);
    case ir::type::PPC_FP128TyID: return Type::getPPC_FP128Ty(ctx);
    case ir::type::LabelTyID:     return Type::getLabelTy(ctx);
    case ir::type::MetadataTyID:  return Type::getMetadataTy(ctx);
    case ir::type::TokenTyID:     return Type::getTokenTy(ctx);
    default: break;
  }
  // unknown type
  throw std::runtime_error("unknown conversion from ir::type to Type");
}

/* convert ir::constant to Constant */
Constant *selection::llvm_constant(ir::constant *cst, LLVMContext &ctx) {
  Type *dst_ty = llvm_type(cst->get_type(), ctx);
  if(auto* cc = dynamic_cast<ir::constant_int*>(cst))
    return ConstantInt::get(dst_ty, cc->get_value());
  if(auto* cc = dynamic_cast<ir::constant_fp*>(cst))
    return ConstantFP::get(dst_ty, cc->get_value());
  // unknown constant
  throw std::runtime_error("unknown conversion from ir::constant to Constant");
}

inline Value *Reassociate(Value *V, IRBuilder<> &Builder){
  BinaryOperator *BinOp = dyn_cast<BinaryOperator>(V);
  if(BinOp)
  if(BinOp->getOpcode()==BinaryOperator::BinaryOps::Add){
    Value *LHS = Reassociate(BinOp->getOperand(0), Builder);
    Value *RHS = Reassociate(BinOp->getOperand(1), Builder);
    if(BinaryOperator *BinLHS = dyn_cast<BinaryOperator>(LHS))
    if(BinLHS->getOpcode()==BinaryOperator::BinaryOps::Add){
      Value *LLHS = BinLHS->getOperand(0);
      Value *RLHS = BinLHS->getOperand(1);
      // (cst + x) + y -> cst + (x + y)
      if(isa<Constant>(LLHS))
        return Builder.CreateAdd(LLHS, Builder.CreateAdd(RLHS, RHS));
      // (x + cst) + y -> cst + (x + y)
      if(isa<Constant>(RLHS))
        return Builder.CreateAdd(RLHS, Builder.CreateAdd(LLHS, RHS));
    }
    if(BinaryOperator *BinRHS = dyn_cast<BinaryOperator>(RHS))
    if(BinRHS->getOpcode()==BinaryOperator::BinaryOps::Add){
      Value *LRHS = BinRHS->getOperand(0);
      Value *RRHS = BinRHS->getOperand(1);
      // x + (cst + y) -> cst + (x + y)
      if(isa<Constant>(LRHS))
        return Builder.CreateAdd(LRHS, Builder.CreateAdd(RRHS, LHS));
      // x + (cst + y) -> cst + (x + y)
      if(isa<Constant>(LRHS))
        return Builder.CreateAdd(RRHS, Builder.CreateAdd(LRHS, LHS));
    }
    return BinOp;
  }
  return V;
}

/* convert ir::instruction to llvm::Instruction */
Instruction *selection::llvm_inst(ir::instruction *inst, std::function<Value*(ir::value*)> value, IRBuilder<> &builder) {
  LLVMContext & ctx = builder.getContext();
  auto block = [&](ir::basic_block *x) { return (BasicBlock*)vmap_.at(x); };
  auto type = [&](ir::type *x) { return llvm_type(x, ctx); };
  if(auto* ii = dynamic_cast<ir::cond_branch_inst*>(inst)){
    BasicBlock *true_dest  = block(ii->get_true_dest());
    BasicBlock *false_dest = block(ii->get_false_dest());
    Value *cond = value(ii->get_cond());
    return builder.Insert(BranchInst::Create(true_dest, false_dest, cond));
  }
  if(auto* ii = dynamic_cast<ir::uncond_branch_inst*>(inst)){
    BasicBlock *dest = block(ii->get_dest());
    return builder.Insert(BranchInst::Create(dest));
  }
  if(dynamic_cast<ir::barrier_inst*>(inst)){
    Module *module = builder.GetInsertBlock()->getModule();
    return tgt_->add_barrier(module, builder);
  }
  if(auto* ii = dynamic_cast<ir::phi_node*>(inst)){
    Type *ty = type(ii->get_type()->get_scalar_ty());
    unsigned num_ops = ii->get_num_operands();
    return builder.Insert(PHINode::Create(ty, num_ops));
  }
  if(auto* ii = dynamic_cast<ir::return_inst*>(inst)){
    ir::value *ret_val = ii->get_return_value();
    return builder.Insert(ReturnInst::Create(ctx, ret_val?value(ret_val):nullptr));
  }
  if(auto* ii = dynamic_cast<ir::binary_operator*>(inst)){
    Value *lhs = value(ii->get_operand(0));
    Value *rhs = value(ii->get_operand(1));
    return builder.Insert(BinaryOperator::Create(ii->get_op(), lhs, rhs));
  }
  if(auto* ii = dynamic_cast<ir::icmp_inst*>(inst)){
    CmpInst::Predicate pred = ii->get_pred();
    Value *lhs = value(ii->get_operand(0));
    Value *rhs = value(ii->get_operand(1));
    return builder.Insert(CmpInst::Create(Instruction::ICmp, pred, lhs, rhs));
  }
  if(auto* ii = dynamic_cast<ir::fcmp_inst*>(inst)){
    CmpInst::Predicate pred = ii->get_pred();
    Value *lhs = value(ii->get_operand(0));
    Value *rhs = value(ii->get_operand(1));
    return builder.Insert(FCmpInst::Create(Instruction::FCmp, pred, lhs, rhs));
  }
  if(auto* ii = dynamic_cast<ir::cast_inst*>(inst)){
    Value *arg = value(ii->get_operand(0));
    Type *dst_ty = type(ii->get_type()->get_scalar_ty());
    return builder.Insert(CastInst::Create(ii->get_op(), arg, dst_ty));
  }
  if(auto* ii = dynamic_cast<ir::getelementptr_inst*>(inst)){
    std::vector<Value*> idx_vals;
    std::transform(ii->idx_begin(), ii->idx_end(), std::back_inserter(idx_vals),
                   [&value](ir::value* x){ return value(x);});
    Type *source_ty = type(ii->get_source_elt_ty()->get_scalar_ty());
    idx_vals[0] = Reassociate(idx_vals[0], builder);
    Value *arg = value(ii->get_operand(0));
    return builder.Insert(GetElementPtrInst::CreateInBounds(source_ty, arg, idx_vals));
  }
  if(ir::load_inst* ii = dynamic_cast<ir::load_inst*>(inst)){
    Value *ptr = value(ii->get_pointer_operand());
    LoadInst *result = new LoadInst(ptr);
    return builder.Insert(result);
  }
  if(ir::store_inst* ii = dynamic_cast<ir::store_inst*>(inst)){
    Value *val = value(ii->get_value_operand());
    Value *ptr = value(ii->get_pointer_operand());
    builder.CreateStore(val, ptr);
    return nullptr;
  }
  if(ir::select_inst* ii = dynamic_cast<ir::select_inst*>(inst)){
    Value *pred = value(ii->get_operand(0));
    Value *if_value = value(ii->get_operand(1));
    Value *else_value = value(ii->get_operand(2));
    return builder.Insert(SelectInst::Create(pred, if_value, else_value));
  }
  if(ir::get_range_id_inst* ii = dynamic_cast<ir::get_range_id_inst*>(inst)){
    Value *offset = tgt_->get_block_id(builder.GetInsertBlock()->getModule(), builder, ii->get_axis());
    return (Instruction*)builder.CreateAdd(offset, builder.getInt32(0));
  }
  if(ir::atomic_cas_inst* ii = dynamic_cast<ir::atomic_cas_inst*>(inst)){
    BasicBlock *current = builder.GetInsertBlock();
    Module *module = current->getModule();
    Value *tid = tgt_->get_local_id(module, builder, 0);
    Value *pred = builder.CreateICmpEQ(tid, builder.getInt32(0));
    BasicBlock *tid_0_bb = BasicBlock::Create(ctx, "tid_0", current->getParent());
    BasicBlock *tid_0_done_bb = BasicBlock::Create(ctx, "tid_0_done", current->getParent());
    Value *ptr = builder.CreateGEP(sh_mem_ptr_, builder.getInt32(alloc_->get_offset(ii)));
    ptr = builder.CreateBitCast(ptr, PointerType::get(builder.getInt32Ty(), ptr->getType()->getPointerAddressSpace()));
    tgt_->add_barrier(module, builder);
    builder.CreateCondBr(pred, tid_0_bb, tid_0_done_bb);
    builder.SetInsertPoint(tid_0_bb);
    Value *cas_ptr = value(ii->get_operand(0));
    Value *cas_cmp = value(ii->get_operand(1));
    Value *cas_val = value(ii->get_operand(2));
    Value *old = builder.CreateAtomicCmpXchg(cas_ptr, cas_cmp, cas_val, AtomicOrdering::Monotonic, AtomicOrdering::Monotonic);
    old = builder.CreateExtractValue(old, {0});
    builder.CreateStore(old, ptr);
    builder.CreateBr(tid_0_done_bb);
    builder.SetInsertPoint(tid_0_done_bb);
    tgt_->add_barrier(module, builder);
    Value *res = builder.CreateLoad(ptr);
    return (Instruction*)res;
  }
  // unknown instruction
  throw std::runtime_error("unknown conversion from ir::instruction to Instruction");
}

/* convert ir::alloc_const to llvm::GlobalVariable */
Value* selection::llvm_alloc_const(ir::alloc_const *v, Module *module, IRBuilder<> &builder) {
  unsigned size = ((ir::constant_int*)v->get_operand(0))->get_value();
  Type *element_ty = llvm_type(v->get_type()->get_pointer_element_ty(), module->getContext());
  Type *array_ty = llvm::ArrayType::get(element_ty, size);
  Value *array = new llvm::GlobalVariable(*module, array_ty, false, llvm::GlobalVariable::ExternalLinkage,
                                            nullptr, v->get_name(), nullptr, llvm::GlobalVariable::NotThreadLocal, 4);
  return builder.CreateBitCast(array, element_ty->getPointerTo(4));
}

/* convert ir::value to llvm::Value */
Value* selection::llvm_value(ir::value *v, IRBuilder<> &builder) {
  assert(!v->get_type()->is_tile_ty());
  LLVMContext &ctx = builder.getContext();
  if(vmap_.find(v) != vmap_.end())
    return vmap_.at(v);
  // create operands
  if(auto *cc = dynamic_cast<ir::constant*>(v))
    return llvm_constant(cc, ctx);
  // alloc const
  if(auto *cc = dynamic_cast<ir::alloc_const*>(v)){
    BasicBlock *block = builder.GetInsertBlock();
    Module *module = block->getModule();
    unsigned size = ((ir::constant_int*)cc->get_operand(0))->get_value();
    Type *element_ty = llvm_type(cc->get_type()->get_pointer_element_ty(), ctx);
    Type *array_ty = llvm::ArrayType::get(element_ty, size);
    if(vmap_.find(v) == vmap_.end()){
      Value *array = new llvm::GlobalVariable(*module, array_ty, false, llvm::GlobalVariable::ExternalLinkage,
                                              nullptr, cc->get_name(), nullptr, llvm::GlobalVariable::NotThreadLocal, 4);
      vmap_[v] = builder.CreateBitCast(array, array->getType()->getArrayElementType()->getPointerTo(4));
    }
    return vmap_.at(v);
  }
  // instruction
  if(auto *ii = dynamic_cast<ir::instruction*>(v)){
    auto value = [&](ir::value *x) { return llvm_value(x, builder); };
    return llvm_inst(ii, value, builder);
  }
  // unknown value
  throw std::runtime_error("unknown conversion from ir::value to Value");
}

// Grid construction
std::vector<Value*> delinearize(Value *trailing, std::vector<unsigned> &shapes, IRBuilder<> &builder){
  size_t dim = shapes.size();
  std::vector<Value*> result(dim);
  for(unsigned k = 0; k < dim - 1; k++){
    Constant *dim_k = builder.getInt32(shapes[k]);
    Value *rem = builder.CreateURem(trailing, dim_k);
    trailing = builder.CreateUDiv(trailing, dim_k);
    result[k] = rem;
  }
  result[dim - 1] = trailing;
  return result;
}

inline int32_t ceil(int32_t num, int32_t div){
  return (num + div - 1)/div;
}

inline void to_warps(const std::vector<unsigned> &bs, std::vector<unsigned> &nw, std::vector<unsigned> &ws){
  static const size_t warp_size = 32;
  size_t nthreads = 1, nwarps = 1;
  nw.resize(bs.size());
  ws.resize(bs.size());
  for(size_t i = 0; i < bs.size(); ++i){
    nthreads *= bs[i];
    nw[i] = ceil(nthreads, nwarps*warp_size);
    nwarps *= nw[i];
  }
  for(size_t i = 0; i < bs.size(); ++i)
    ws[i] = bs[i] / nw[i];
}

void selection::init_axes(ir::value *v, IRBuilder<> &builder, Value *u_thread_id, Value *u_warp_id) {
  const auto& shapes = v->get_type()->get_tile_shapes();
  size_t dim = shapes.size();
  if(params_->get_fragment(v, 0) == tune::STRIDED_SCAN){
    std::vector<unsigned> contiguous(dim);
    std::vector<unsigned> block_size(dim);
    std::vector<unsigned> warp_size(dim);
    std::vector<unsigned> n_warps(dim);
    for(unsigned i = 0; i < shapes.size(); i++){
      std::string str_i = std::to_string(i);
      contiguous[i] = params_->get_param(v, "nts.d" + str_i)->get_value();
      block_size[i] = params_->get_param(v, "mts.d" + str_i)->get_value();
    }
    to_warps(block_size, n_warps, warp_size);
    std::vector<Value*> thread_id_in_warp = delinearize(u_thread_id, warp_size, builder);
    std::vector<Value*> warp_id = delinearize(u_warp_id, n_warps, builder);
    // Create axes
    for(unsigned k = 0; k < dim; k++) {
      std::string str_k = std::to_string(k);
      Value *warp_size_k = builder.getInt32(warp_size[k]);
      Value *contiguous_k = builder.getInt32(contiguous[k]);
      Value *thread_id   = builder.CreateAdd(thread_id_in_warp[k], builder.CreateMul(warp_id[k], warp_size_k));
      thread_id = builder.CreateMul(thread_id, contiguous_k);
      unsigned per_block = contiguous[k] * warp_size[k] * n_warps[k];
      unsigned per_thread = contiguous[k] * shapes[k]->get_value() / per_block;
      std::vector<Value*> idx_list(per_thread);
      for(unsigned n = 0 ; n < per_thread; n++){
        unsigned offset = n / contiguous[k] * per_block + n % contiguous[k];
        idx_list[n] = builder.CreateAdd(thread_id, builder.getInt32(offset), "idx_" + str_k + "_" + std::to_string(n));
      }
      axes_[params_->get_param_group(v, k)] = distributed_axis{contiguous[k], idx_list};
    }
  }
  else {
    Value *_1 = builder.getInt32(1);
    Value *_2 = builder.getInt32(2);
    Value *_3 = builder.getInt32(3);
    Value *_4 = builder.getInt32(4);
    Value *_8 = builder.getInt32(8);
    Value *_16 = builder.getInt32(16);

    // fragments per warp
    unsigned fpw_0 = params_->get_param(v, "fpw.d0")->get_value();
    unsigned fpw_1 = params_->get_param(v, "fpw.d1")->get_value();
    // warps per tile
    unsigned wpt_0 = params_->get_param(v, "wpt.d0")->get_value();
    unsigned wpt_1 = params_->get_param(v, "wpt.d1")->get_value();
    // hmma warp tile size
    unsigned hmma_wts_0 = fpw_0 * 8;
    unsigned hmma_wts_1 = fpw_1 * 8;
    // hmma block tile size
    unsigned hmma_bts_0 = hmma_wts_0 * wpt_0;
    unsigned hmma_bts_1 = hmma_wts_1 * wpt_1;
    // number of repetition
    unsigned num_rep_0 = shapes[0]->get_value() / hmma_bts_0;
    unsigned num_rep_1 = shapes[1]->get_value() / hmma_bts_1;
    // size of each pack (interleaving)
    pack_size_0_ = std::min<unsigned>(num_rep_0, 1);
    pack_size_1_ = std::min<unsigned>(num_rep_1, 1);
    // number of packs (interleaving)
    num_packs_0_ = num_rep_0 / pack_size_0_;
    num_packs_1_ = num_rep_1 / pack_size_1_;


    /* intra warp offset */
    // offset of quad in pair
    Value *in_pair_off_a = builder.CreateMul(builder.CreateUDiv(builder.CreateAnd(u_thread_id, _16), builder.getInt32(4)), builder.getInt32(fpw_0 * pack_size_0_));
    Value *in_pair_off_b = builder.CreateMul(builder.CreateUDiv(builder.CreateAnd(u_thread_id, _16), builder.getInt32(4)), builder.getInt32(fpw_1 * pack_size_1_));
    // Quad pair id
    Value *pair_a_id = builder.CreateUDiv(builder.CreateURem(u_thread_id, _16), _4);
    Value *pair_b_id = builder.CreateUDiv(builder.CreateURem(u_thread_id, _16), _4);
    pair_a_id = builder.CreateURem(pair_a_id, builder.getInt32(fpw_0));
    pair_b_id = builder.CreateUDiv(pair_b_id, builder.getInt32(fpw_0));
    // Quad pair offset
    Value *pair_a_off = builder.CreateMul(pair_a_id, builder.getInt32(4 * pack_size_0_));
    Value *pair_b_off = builder.CreateMul(pair_b_id, builder.getInt32(4 * pack_size_1_));

    /* inter warp offset */
    Value *warp_id_0 = builder.CreateURem(u_warp_id, builder.getInt32(wpt_0));
    Value *warp_id_1 = builder.CreateUDiv(u_warp_id, builder.getInt32(wpt_0));
    Value *warp_offset_i = builder.CreateMul(warp_id_0, builder.getInt32(hmma_wts_0 * pack_size_0_));
    Value *warp_offset_j = builder.CreateMul(warp_id_1, builder.getInt32(hmma_wts_1 * pack_size_1_));

    /* offsets */
    // a offset
    offset_a_i_ = builder.CreateAdd(warp_offset_i, builder.CreateAdd(pair_a_off, in_pair_off_a));
    offset_a_k_ = builder.CreateAnd(u_thread_id, _3);
//    // b offsets
    offset_b_j_ = builder.CreateAdd(warp_offset_j, builder.CreateAdd(pair_b_off, in_pair_off_b));
    offset_b_k_ = builder.CreateAnd(u_thread_id, _3);

    // c offsets
    Value *offset_c_i = builder.CreateAdd(builder.CreateAnd(u_thread_id, _1), offset_a_i_);
    Value *offset_c_j = builder.CreateAdd(builder.CreateAnd(u_thread_id, _2),
                                          builder.CreateAdd(warp_offset_j, pair_b_off));

    /* indices */
    // i indices
    std::vector<Value*> idx_i;
    for(unsigned pack = 0; pack < num_packs_0_; pack++)
    for(unsigned ii = 0; ii < pack_size_0_; ii++)
    for(unsigned i = 0; i < 2; i++){
      idx_i.push_back(builder.CreateAdd(offset_c_i, builder.getInt32(pack*hmma_bts_0*pack_size_0_ + ii*4 + i*2)));
    }
    // j indices
    std::vector<Value*> idx_j;
    for(unsigned pack = 0; pack < num_packs_1_; pack++)
    for(unsigned jj = 0; jj < pack_size_1_; jj++)
    for(unsigned j = 0; j < 2; j++){
      idx_j.push_back(builder.CreateAdd(offset_c_j, builder.getInt32(pack*hmma_bts_1*pack_size_1_ + jj*4 + j*4*fpw_1*pack_size_1_)));
      idx_j.push_back(builder.CreateAdd(offset_c_j, builder.getInt32(pack*hmma_bts_1*pack_size_1_ + jj*4 + j*4*fpw_1*pack_size_1_ + 1)));
    }

    /* axes */
    axes_[params_->get_param_group(v, 0)] = distributed_axis{1, idx_i};
    axes_[params_->get_param_group(v, 1)] = distributed_axis{1, idx_j};
  }
}

void selection::create_grids(std::vector<ir::value*> &grids,
                             std::map<unsigned, ir::value*> &references,
                             ir::function *fn) {
  // get number of dimensions greater than 1
  auto get_tile_gt1_dim = [&](ir::value *v){
    unsigned result = 0;
    for(ir::constant_int* shape: v->get_type()->get_tile_shapes()) {
      result += (shape->get_value() > 1)?shape->get_value():0;
    }
    return result;
  };
  // bind references
  std::set<ir::value*> seen;
  std::function<void(ir::value*)> bind_references = [&](ir::value *v)
  {
    // skip
    if(!v->get_type()->is_tile_ty() || !seen.insert(v).second || dynamic_cast<ir::mask_inst*>(v))
      return;
    // recurse
    if(auto *user = dynamic_cast<ir::user*>(v))
      for(ir::value *op: user->ops())
        bind_references(op);
    // bind
    const auto& shapes = v->get_type()->get_tile_shapes();
    if(buffer_info_->is_shared(v))
      return;
    for(size_t d = 0; d < shapes.size(); d++){
      if(shapes[d]->get_value() == 1)
        continue;
      unsigned x = params_->get_param_group(v, d);
      ir::value *&r = references[x];
      if(!r || get_tile_gt1_dim(v) > get_tile_gt1_dim(r))
        r = v;
    }
  };

  for(ir::basic_block *block: fn->blocks())
  for(ir::instruction *i: block->get_inst_list())
    bind_references(i);

  // create grid
  for(auto &ref: references)
    if(std::find(grids.begin(), grids.end(), ref.second) == grids.end())
      grids.push_back(ref.second);
}

bool static inline has_phi_user(ir::value *v) {
  for(ir::user *usr: v->get_users()){
    if(dynamic_cast<ir::phi_node*>(usr))
      return true;
  }
  return false;
}
void selection::create_tile(ir::value *v, IRBuilder<> &builder,
                            const std::map<unsigned, ir::value*>& references,
                            std::set<ir::value*> &seen, Value *sh_mem_ptr) {
  if(!v->get_type()->is_tile_ty() || !seen.insert(v).second)
    return;
  if(auto *user = dynamic_cast<ir::user*>(v))
    for(ir::value *op: user->ops())
      create_tile(op, builder, references, seen, sh_mem_ptr);
  LLVMContext &ctx = builder.getContext();
  const auto& cshapes = v->get_type()->get_tile_shapes();
  std::vector<unsigned> shapes;
  for(ir::constant_int* shape: cshapes)
    shapes.push_back(shape->get_value());
  unsigned pad = alloc_->is_ld_padded(v);
  if(pad > 0)
    shapes[0] += pad;
  Type* ty = llvm_type(v->get_type()->get_scalar_ty(), ctx);
  // create shared tile
  if(buffer_info_->is_shared(v)){
    // shared copy
    PointerType *ptr_ty = ty->getPointerTo(sh_mem_ptr->getType()->getPointerAddressSpace());
    // phi-node (double-buffering)
    if(auto *phi = dynamic_cast<ir::phi_node*>(v)) {
      BasicBlock *parent = (BasicBlock*)vmap_[phi->get_parent()];
      unsigned id_pre = 0, id_loop = 1;
      if(phi->get_incoming_block(0) == phi->get_parent())
        std::swap(id_pre, id_loop);
      if(parent->empty())
        builder.SetInsertPoint(parent);
      else
        builder.SetInsertPoint(&*parent->getFirstInsertionPt());
      PHINode *ptr = builder.CreatePHI(ptr_ty, 2);
      PHINode *offset = builder.CreatePHI(builder.getInt32Ty(), 2);
      // next pointer
      Value *pre_ptr = builder.CreateGEP(sh_mem_ptr, builder.getInt32(alloc_->get_offset(phi)));
      pre_ptr = builder.CreateBitCast(pre_ptr, ptr->getType());
      Value *next_ptr = builder.CreateGEP(ptr, offset, "next_ptr");
      tmap_.insert({phi, new shared_tile(ty, shapes, ptr, builder, offset)});
      for(unsigned i = 0; i < phi->get_num_incoming(); i++) {
        ir::basic_block* inc_block = phi->get_incoming_block(i);
        ir::value* inc_value = phi->get_incoming_value(i);
        ir::instruction* terminator = inc_block->get_inst_list().back();
        bool is_loop_latch = buffer_info_->is_loop_latch(phi, terminator);
        tmap_.insert({inc_value, new shared_tile(ty, shapes, is_loop_latch?next_ptr:pre_ptr, builder)});
      }
    }
    else {
      if(!has_phi_user(v)){
        size_t offset = alloc_->get_offset(v);
        Value *ptr = builder.CreateGEP(sh_mem_ptr, builder.getInt32(offset));
        ptr = builder.CreateBitCast(ptr, ptr_ty);
        tmap_.insert({v, new shared_tile(ty, shapes, ptr, builder)});
      }
    }
  }
  // create distributed tile
  else {
    const auto &cshapes = v->get_type()->get_tile_shapes();
    std::vector<distributed_axis> axes(cshapes.size());
    for(size_t d = 0; d < cshapes.size(); d++){
      if(cshapes[d]->get_value() > 1){
        unsigned x = params_->get_param_group(v, d);
        axes[d] = axes_.at(x);
      }
      else{
        axes[d].contiguous = 1;
        axes[d].values = {builder.getInt32(0)};
      }
    }
    bool vectorize = dynamic_cast<ir::vectorize_inst*>(v);
    distributed_tile *T = new distributed_tile(ty, shapes, axes, builder, vectorize);
    tmap_.insert({v, T});
    // constant range
    if(dynamic_cast<ir::constant*>(v) && !dynamic_cast<ir::undef_value*>(v)){
      T->for_each([&](indices_t idx){
        assert(idx.size() == 1);
        T->set_value(idx, idx[0]);
      });
    }

  }
}

void selection::init_grids(ir::function *fn, IRBuilder<> &builder, Value *sh_mem_ptr){
  // fetch linear ID
  Module *mod = builder.GetInsertBlock()->getParent()->getParent();
  Value *warp_size = builder.getInt32(32);
  Value* u_thread_id = tgt_->get_local_id(mod, builder, 0);
  Value *u_thread_warp_id = builder.CreateURem(u_thread_id, warp_size);
  Value *u_warp_id = builder.CreateUDiv(u_thread_id, warp_size);
  // create grid
  std::vector<ir::value*> grids;
  std::map<unsigned, ir::value*> references;
  create_grids(grids, references, fn);
  for(ir::value* i: grids){
    if(auto *instr = dynamic_cast<ir::instruction*>(i))
      for(unsigned r = 0; r < instr->get_num_results(); r++)
        init_axes(instr->get_result(r), builder, u_thread_warp_id, u_warp_id);
    else
      init_axes(i, builder, u_thread_warp_id, u_warp_id);
  }
  // create tile
  std::set<ir::value*> seen;
  for(ir::basic_block *block: fn->blocks())
  for(ir::instruction *i: block->get_inst_list()){
    if(!i->get_type()->is_tile_ty())
      continue;
    for(unsigned r = 0; r < i->get_num_results(); r++)
      create_tile(i->get_result(r), builder, references, seen, sh_mem_ptr);
  }
}


void selection::lower_tile_instruction(ir::instruction *ins, llvm::IRBuilder<> &builder) {
  BasicBlock *block = builder.GetInsertBlock();
  Module *module = block->getModule();
  LLVMContext &ctx = builder.getContext();
  Function *fn = block->getParent();
  ir::value *mask = ins->get_mask_pred();
  BasicBlock *last_block = nullptr;
  auto set_mask_insert_pt = [&](indices_t idx){
    if(mask){
      distributed_tile *mask_tile = (distributed_tile*)tmap_.at(ins->get_mask_pred());
      BasicBlock *block = pmap_.at({mask_tile, idx});
      builder.SetInsertPoint(block->getTerminator());
      last_block = last_block_.at({mask_tile, idx});
    }
  };
  // store
  if(auto *x = dynamic_cast<ir::store_inst*>(ins)) {
    distributed_tile* ptr = (distributed_tile*)tmap_.at(x->get_pointer_operand());
    tile *value = tmap_.at(x->get_value_operand());
    ptr->for_each([&](indices_t idx){
      set_mask_insert_pt(idx);
      StoreInst *store = new StoreInst(value->get_value(idx), ptr->get_value(idx));
      builder.Insert(store);
    });
  }
  else {
    if(auto *x = dynamic_cast<ir::downcast_inst*>(ins)){
      vmap_[x] = tmap_[x->get_operand(0)]->get_value({builder.getInt32(0)});
      return;
    }
    tile *ti = tmap_[ins];
    distributed_tile* result = (distributed_tile*)ti;
    if(!ins->get_type()->is_tile_ty())
      return;
    const auto& shapes = ins->get_type()->get_tile_shapes();
    // global_range
    if(auto *x = dynamic_cast<ir::get_global_range_inst*>(ins)) {
      Value *offset = tgt_->get_global_offset(module, builder, shapes[0]->get_value(), x->get_axis());
      result->for_each([&](indices_t idx){
        BinaryOperator *bin = static_cast<BinaryOperator*>(idx[0]);
        result->set_value(idx, builder.CreateAdd(bin, offset));
      });
    }
    // mask
    else if(dynamic_cast<ir::mask_inst*>(ins)) {
      distributed_tile* pred = (distributed_tile*)tmap_.at(ins->get_operand(0));
      distributed_tile* mask_tile_true = (distributed_tile*)tmap_.at(ins->get_result(0));
      distributed_tile* mask_tile_false = (distributed_tile*)tmap_.at(ins->get_result(1));
      pred->for_each([&](indices_t idx){
        BasicBlock *mask_then_bb   = BasicBlock::Create(ctx, "mask_then", fn);
        BasicBlock* mask_else_bb = BasicBlock::Create(ctx, "mask_else", fn);
        BasicBlock *mask_done_bb = BasicBlock::Create(ctx, "mask_done", fn);
        builder.CreateCondBr(pred->get_value(idx), mask_then_bb, mask_else_bb);
        builder.SetInsertPoint(mask_then_bb);
        builder.CreateBr(mask_done_bb);
        builder.SetInsertPoint(mask_else_bb);
        builder.CreateBr(mask_done_bb);
        builder.SetInsertPoint(mask_done_bb);
        pmap_.insert({{mask_tile_true, idx}, mask_then_bb});
        pmap_.insert({{mask_tile_false, idx}, mask_else_bb});
        last_block_.insert({{mask_tile_true, idx}, mask_done_bb});
        last_block_.insert({{mask_tile_false, idx}, mask_done_bb});
      });
    }
    // merge
    else if(auto *merge = dynamic_cast<ir::psi_inst*>(ins)) {
      distributed_tile* mask_tile_true = (distributed_tile*)tmap_.at(merge->get_mask_true());
      distributed_tile *value_tile_true = (distributed_tile*)tmap_.at(merge->get_value_true());
      distributed_tile* mask_tile_false = (distributed_tile*)tmap_.at(merge->get_mask_false());
      distributed_tile *value_tile_false = (distributed_tile*)tmap_.at(merge->get_value_false());
      result->for_each([&](indices_t idx){
        BasicBlock *block_true = pmap_.at({mask_tile_true, idx});
        Value *value_true = value_tile_true->get_value(idx);
        BasicBlock *block_false =  pmap_.at({mask_tile_false, idx});
        Value *value_false = value_tile_false->get_value(idx);
        BasicBlock *block_done = last_block_.at({mask_tile_true, idx});
        if(block_done->getTerminator())
          builder.SetInsertPoint(block_done->getTerminator());
        else
          builder.SetInsertPoint(block_done);
        PHINode *phi = builder.CreatePHI(value_true->getType(), 2);
        phi->addIncoming(value_true, block_true);
        phi->addIncoming(value_false,block_false);
        result->set_value(idx, phi);
      });
    }
    // reshape
    else if(dynamic_cast<ir::reshape_inst*>(ins)) {
      ir::value* in = ins->get_operand(0);
      distributed_tile *in_tile = (distributed_tile*)tmap_.at(in);
      result->for_each([&](indices_t out_idx){
        indices_t in_idx;
        for(size_t k = 0; k < shapes.size(); k++){
          if(shapes[k]->get_value() > 1)
            in_idx.push_back(out_idx[k]);
        }
        result->set_value(out_idx, in_tile->get_value(in_idx));
      });
    }
    // splat
    else if(dynamic_cast<ir::splat_inst*>(ins)) {
      result->for_each([&](indices_t idx) {
        set_mask_insert_pt(idx);
        result->set_value(idx, llvm_value(ins->get_operand(0), builder));
      });
    }
    // broadcast
    else if(dynamic_cast<ir::broadcast_inst*>(ins)) {
      ir::value* in = ins->get_operand(0);
      const auto& in_shapes = in->get_type()->get_tile_shapes();
      distributed_tile *in_tile = (distributed_tile*)tmap_.at(in);
      result->for_each([&](indices_t out_idx){
        indices_t in_idx = out_idx;
        for(size_t k = 0; k < in_idx.size(); k++){
          if(in_shapes[k]->get_value() == 1)
            in_idx[k] = builder.getInt32(0);
        }
        result->set_value(out_idx, in_tile->get_value(in_idx));
      });
    }
    // vectorize
    else if(dynamic_cast<ir::vectorize_inst*>(ins)) {
      distributed_tile* in = (distributed_tile*)tmap_.at(ins->get_operand(0));
      unsigned vector_size = result->axis(0).contiguous;
      std::map<unsigned, Value*> packets;
      in->for_each([&](indices_t idx){
        unsigned linear = in->get_linear_index(idx);
        unsigned id = linear / vector_size;
        if(linear % vector_size == 0)
          packets[id] = result->get_value(idx);
        packets[id] = builder.CreateInsertElement(packets.at(id), in->get_value(idx), linear % vector_size);
      });
      result->for_each([&](indices_t idx){
        unsigned linear = in->get_linear_index(idx);
        unsigned id = linear / vector_size;
        if(linear % vector_size == 0)
          result->set_value(idx, packets[id]);
      });
    }
    // copy to shared
    else if(dynamic_cast<ir::copy_to_shared_inst*>(ins)) {
      distributed_tile* in = (distributed_tile*)tmap_.at(ins->get_operand(0));
      in->for_each([&](indices_t idx){
        ti->set_value(idx, in->get_value(idx));
      });
    }
    // trans
    else if(dynamic_cast<ir::trans_inst*>(ins)) {
      distributed_tile* in = (distributed_tile*)tmap_.at(ins->get_operand(0));
      in->for_each([&](indices_t idx){
        indices_t out_idx = idx;
        std::rotate(out_idx.begin(), out_idx.begin() + 1, out_idx.end());
        ti->set_value(out_idx, in->get_value(idx));
      });
    }
    else if(buffer_info_->is_shared(ins))
      return;
    // dot
    else if(auto dot = dynamic_cast<ir::dot_inst*>(ins)) {
      ir::value *A = ins->get_operand(0);
      ir::value *B = ins->get_operand(1);
      ir::value *C = ins->get_operand(2);
      bool AT = dot->is_a_trans();
      bool BT = dot->is_b_trans();
      distributed_tile *TC = (distributed_tile*)tmap_.at(C);
      Function *f_mul_add = Intrinsic::getDeclaration(module, Intrinsic::fmuladd, {llvm_type(C->get_type()->get_scalar_ty(), ctx)});
      unsigned NK = A->get_type()->get_tile_shapes()[1]->get_value();
      if(NK != 1)
      {
        shared_tile *TA = (shared_tile*)tmap_.at(A);
        shared_tile *TB = (shared_tile*)tmap_.at(B);
        if(params_->get_fragment(ins, 0) == tune::STRIDED_SCAN)
        {
          TA->set_vector_size(TC->axis(0).contiguous);
          TB->set_vector_size(TC->axis(1).contiguous);
          result->for_each([&](indices_t idx){
            Value *res = TC->get_value(idx);
            for(unsigned K = 0; K < NK; ++K){
              indices_t a_idx = {idx[0], builder.getInt32(K)};
              indices_t b_idx = {builder.getInt32(K), idx[1]};
              if(AT)
                std::swap(a_idx[0], a_idx[1]);
              if(BT)
                std::swap(b_idx[0], b_idx[1]);
              Value *a = TA->get_value(a_idx);
              Value *b = TB->get_value(b_idx);
              res = builder.CreateCall(f_mul_add, {a, b, res});

            }
            result->set_value(idx, res);
          });
        }
        else
        {
          TA->set_vector_size(4*pack_size_0_);
          TB->set_vector_size(4*pack_size_1_);
          TA->set_return_mode(true);
          TB->set_return_mode(true);

          std::vector<Value *> fc;
          result->for_each([&](indices_t idx){
            fc.push_back(result->get_value(idx));
          });

          Type *fp32_ty = builder.getFloatTy();
          Type *fp16x2_ty = VectorType::get(builder.getHalfTy(), 2);
          Type *fp32_pack8_ty = StructType::get(ctx, {fp32_ty, fp32_ty, fp32_ty, fp32_ty, fp32_ty, fp32_ty, fp32_ty, fp32_ty});
          FunctionType *mma_ty = FunctionType::get(fp32_pack8_ty, {fp16x2_ty, fp16x2_ty, fp16x2_ty, fp16x2_ty, fp32_ty, fp32_ty, fp32_ty, fp32_ty, fp32_ty, fp32_ty, fp32_ty, fp32_ty}, false);

          InlineAsm *mma_fn = InlineAsm::get(mma_ty, " mma.sync.aligned.m8n8k4.col.row.f32.f16.f16.f32 "
                                                     "{$0, $1, $2, $3, $4, $5, $6, $7}, "
                                                     "{$8, $9}, "
                                                     "{$10, $11}, "
                                                     "{$0, $1, $2, $3, $4, $5, $6, $7};", "=f,=f,=f,=f,=f,=f,=f,=f,r,r,r,r,0,1,2,3,4,5,6,7", false);

          unsigned fpw_0 = params_->get_param(dot, "fpw.d0")->get_value();
          unsigned fpw_1 = params_->get_param(dot, "fpw.d1")->get_value();
          unsigned wts_0 = fpw_0 * 8;
          unsigned wts_1 = fpw_1 * 8;
          unsigned wpt_0 = params_->get_param(dot, "wpt.d0")->get_value();
          unsigned wpt_1 = params_->get_param(dot, "wpt.d1")->get_value();
          unsigned stride_rep_i = wpt_0 * wts_0;
          unsigned stride_rep_j = wpt_1 * wts_1;
          unsigned num_rep_i = shapes[0]->get_value() / stride_rep_i;
          unsigned ld_fc = num_rep_i * 2;
          for(unsigned pack_i = 0; pack_i < num_packs_0_; pack_i++)
          for(unsigned pack_j = 0; pack_j < num_packs_1_; pack_j++){
          for(unsigned K = 0; K < NK; K += 4){
            Value *_K = builder.getInt32(K);
            Value *current_offset_a_i = builder.CreateAdd(offset_a_i_, builder.getInt32(pack_i*stride_rep_i*pack_size_0_));
            Value *current_offset_b_i = builder.CreateAdd(offset_b_j_, builder.getInt32(pack_j*stride_rep_j*pack_size_1_));
            Value *ha = TA->get_value({current_offset_a_i, builder.CreateAdd(offset_a_k_, _K)});
            Value *hb = TB->get_value({current_offset_b_i, builder.CreateAdd(offset_b_k_, _K)});
            for(unsigned ii = 0; ii < pack_size_0_; ii++)
            for(unsigned jj = 0; jj < pack_size_1_; jj++){
              Value *ha0 = builder.CreateExtractElement(ha, builder.getInt32(ii*pack_size_0_ + 0));
              Value *ha1 = builder.CreateExtractElement(ha, builder.getInt32(ii*pack_size_0_ + 1));
              Value *hb0 = builder.CreateExtractElement(hb, builder.getInt32(jj*pack_size_0_ + 0));
              Value *hb1 = builder.CreateExtractElement(hb, builder.getInt32(jj*pack_size_0_ + 1));
              std::vector<size_t> idx = {
                (pack_i*2*pack_size_0_ + ii*2 + 0) + (pack_j*4*pack_size_1_ + jj*4 + 0)*ld_fc,
                (pack_i*2*pack_size_0_ + ii*2 + 0) + (pack_j*4*pack_size_1_ + jj*4 + 1)*ld_fc,
                (pack_i*2*pack_size_0_ + ii*2 + 1) + (pack_j*4*pack_size_1_ + jj*4 + 0)*ld_fc,
                (pack_i*2*pack_size_0_ + ii*2 + 1) + (pack_j*4*pack_size_1_ + jj*4 + 1)*ld_fc,
                (pack_i*2*pack_size_0_ + ii*2 + 0) + (pack_j*4*pack_size_1_ + jj*4 + 2)*ld_fc,
                (pack_i*2*pack_size_0_ + ii*2 + 0) + (pack_j*4*pack_size_1_ + jj*4 + 3)*ld_fc,
                (pack_i*2*pack_size_0_ + ii*2 + 1) + (pack_j*4*pack_size_1_ + jj*4 + 2)*ld_fc,
                (pack_i*2*pack_size_0_ + ii*2 + 1) + (pack_j*4*pack_size_1_ + jj*4 + 3)*ld_fc
              };
              Value *nc = builder.CreateCall(mma_fn, {ha0, ha1, hb0, hb1, fc[idx[0]], fc[idx[1]], fc[idx[2]], fc[idx[3]], fc[idx[4]], fc[idx[5]], fc[idx[6]], fc[idx[7]]});
              fc[idx[0]] = builder.CreateExtractValue(nc, {0});
              fc[idx[1]] = builder.CreateExtractValue(nc, {1});
              fc[idx[2]] = builder.CreateExtractValue(nc, {2});
              fc[idx[3]] = builder.CreateExtractValue(nc, {3});
              fc[idx[4]] = builder.CreateExtractValue(nc, {4});
              fc[idx[5]] = builder.CreateExtractValue(nc, {5});
              fc[idx[6]] = builder.CreateExtractValue(nc, {6});
              fc[idx[7]] = builder.CreateExtractValue(nc, {7});
            }
          }
          }

          // write back
          unsigned i = 0;
          result->for_each([&](indices_t idx){
            result->set_value(idx, fc[i++]);
          });

          TA->set_return_mode(false);
          TB->set_return_mode(false);
        }
      }
      else
      {
        distributed_tile *TA = (distributed_tile*)tmap_.at(A);
        distributed_tile *TB = (distributed_tile*)tmap_.at(B);
        result->for_each([&](indices_t idx){
          Value *res = TC->get_value(idx);
          indices_t a_idx = {idx[0], builder.getInt32(0)};
          indices_t b_idx = {builder.getInt32(0), idx[1]};
          if(AT)
            std::swap(a_idx[0], a_idx[1]);
          if(BT)
            std::swap(b_idx[0], b_idx[1]);
          Value *a = TA->get_value(a_idx);
          Value *b = TB->get_value(b_idx);
          res = builder.CreateCall(f_mul_add, {a, b, res});
          result->set_value(idx, res);
        });
      }
    }
    else if(auto *ld = dynamic_cast<ir::load_inst*>(ins)){
      unsigned vector_size = result->axis(0).contiguous;
      std::map<unsigned, Value*> packets;
      distributed_tile *TP = (distributed_tile*)tmap_.at(ld->get_pointer_operand());
      result->for_each([&](indices_t idx){
        set_mask_insert_pt(idx);
        unsigned linear = result->get_linear_index(idx);
        unsigned id = linear / vector_size;
        if(linear % vector_size == 0){
          Value *ptr = TP->get_value(idx);
          ptr= builder.CreateBitCast(ptr, PointerType::get(VectorType::get(result->get_ty(), vector_size),
                                                           ptr->getType()->getPointerAddressSpace()));
          packets[id] = builder.CreateLoad(ptr);
        }
        result->set_value(idx, builder.CreateExtractElement(packets.at(id), linear % vector_size));
      });
    }
    // element-wise
    else {
      result->for_each([&](indices_t idx){
        auto value = [&](ir::value *x) {
          if(x->get_type()->is_tile_ty())
            return tmap_.at(x)->get_value(idx);
          else
            return llvm_value(x, builder);
        };
        set_mask_insert_pt(idx);
        result->set_value(idx, llvm_inst(ins, value, builder));
      });
    }
  }
  if(mask){
    builder.SetInsertPoint(block);
    if(last_block)
      builder.SetInsertPoint(last_block);
  }
}

void selection::lower_instruction(ir::instruction *src, IRBuilder<> &builder) {
  if(src->has_tile_result_or_op() || (src->get_mask_pred() && src->get_mask_pred()->get_type()->is_tile_ty())) {
    lower_tile_instruction(src, builder);
  }
  else {
    Instruction *i = (Instruction*)llvm_value(src, builder);
    vmap_[src] = i;
  }
}

inline llvm::Attribute llvm_attr(llvm::LLVMContext& ctx, ir::attribute attr) {
  switch(attr.get_kind()){
    case ir::noalias: return llvm::Attribute::get(ctx, llvm::Attribute::NoAlias);
    case ir::readonly: return llvm::Attribute::get(ctx, llvm::Attribute::ReadOnly);
    case ir::writeonly: return llvm::Attribute::get(ctx, llvm::Attribute::WriteOnly);
    case ir::aligned: return llvm::Attribute::get(ctx, llvm::Attribute::Alignment, attr.get_value());
    default: throw std::runtime_error("cannot convert ir::attribute_t to llvm::Attribute");
  }
}

ArrayType* selection::llvm_linearized_tile_type(ir::type *ty, LLVMContext &ctx) {
  unsigned size = 1;
  for(ir::constant_int* shape: ty->get_tile_shapes())
    size *= shape->get_value();
  return ArrayType::get(llvm_type(ty->get_scalar_ty(), ctx), size);
}

void selection::run(ir::module &src, Module &dst) {
  vmap_.clear();
  LLVMContext &dst_ctx = dst.getContext();
  IRBuilder<> dst_builder(dst_ctx);

  for(ir::alloc_const *x: src.allocs()) {
    vmap_[x] = llvm_alloc_const(x, &dst, dst_builder);
  }

  // iterate over functions
  for(ir::function *fn: src.get_function_list()) {

    // create LLVM function
    FunctionType *fn_ty = (FunctionType*)llvm_type(fn->get_fn_type(), dst_ctx);
    FunctionType *dst_fn_ty = fn_ty;
    if(!tgt_->is_gpu()){
      Type *dst_fn_ret_ty = fn_ty->getReturnType();
      std::vector<Type*> dst_fn_args_ty;
      for(unsigned i = 0; i < fn_ty->getNumParams(); i++)
        dst_fn_args_ty.push_back(fn_ty->getParamType(i));
      dst_fn_args_ty.push_back(dst_builder.getInt32Ty());
      dst_fn_args_ty.push_back(dst_builder.getInt32Ty());
      dst_fn_args_ty.push_back(dst_builder.getInt32Ty());
      dst_fn_ty = FunctionType::get(dst_fn_ret_ty, dst_fn_args_ty, false);
    }

    // grid indices
    fn->get_fn_type()->get_return_ty();
    Function *dst_fn = Function::Create(dst_fn_ty, Function::ExternalLinkage, fn->get_name(), &dst);

    // set attributes
    for(auto attr_pair: fn->attrs()){
      unsigned id = attr_pair.first;
      for(ir::attribute attr: attr_pair.second)
        dst_fn->addAttribute(id, llvm_attr(dst_ctx, attr));
    }
    tgt_->set_kernel(dst_builder, dst_ctx, &dst, dst_fn);

    // map parameters
    for(unsigned i = 0; i < fn->args().size(); i++)
      vmap_[fn->args()[i]] = &*(dst_fn->arg_begin() + i);
    // create blocks
    for(ir::basic_block *block: fn->blocks()) {
      BasicBlock *dst_block = BasicBlock::Create(dst_ctx, block->get_name(), dst_fn);
      vmap_[block] = dst_block;
    }
    dst_builder.SetInsertPoint((BasicBlock*)vmap_[fn->blocks()[0]]);

    // allocate shared memory
    Value *sh_mem_ptr = nullptr;
    if(tgt_->is_gpu())
    if(unsigned alloc_size = alloc_->get_allocated_size()){
      Type *int_8_ty = Type::getInt8Ty(dst_ctx);
      ArrayType *array_ty = ArrayType::get(int_8_ty, alloc_size);
      Type *ptr_ty = PointerType::get(int_8_ty, 3);
      GlobalVariable *sh_mem_array =
        new GlobalVariable(dst, array_ty, false, GlobalVariable::ExternalLinkage,
                           nullptr, "__shared_ptr", nullptr, GlobalVariable::NotThreadLocal, 3);
      sh_mem_ptr = dst_builder.CreateBitCast(sh_mem_array, ptr_ty);
    }
    sh_mem_ptr_ = sh_mem_ptr;

    // create grids
    init_grids(fn, dst_builder, sh_mem_ptr);


    // iterate through block
    std::map<ir::basic_block*, BasicBlock*> last_block;
    for(ir::basic_block *block: fn->blocks()) {
      BasicBlock *parent = (BasicBlock*)vmap_[block];
      dst_builder.SetInsertPoint(parent);
      for(ir::instruction *i: block->get_inst_list()){
        BasicBlock *current = dst_builder.GetInsertBlock();
        bool phi_inserted = (dynamic_cast<ir::phi_node*>(i) || dynamic_cast<ir::psi_inst*>(i)) && !current->empty();
        if(phi_inserted && current->getFirstNonPHI())
          dst_builder.SetInsertPoint(&*current->getFirstNonPHI());
        lower_instruction(i, dst_builder);
        if(phi_inserted && current->getFirstNonPHI())
          dst_builder.SetInsertPoint(current);
        last_block[block] = dst_builder.GetInsertBlock();
      }
    }

    // add phi operands
    for(ir::basic_block *block: fn->blocks())
    for(ir::instruction *inst: block->get_inst_list())
    if(auto *phi = dynamic_cast<ir::phi_node*>(inst)){
      if(buffer_info_->is_double(phi)) {
        PHINode *ptr = (PHINode*)((shared_tile*)tmap_.at(phi))->get_pointer();
        PHINode *offset = (PHINode*)((shared_tile*)tmap_.at(phi))->get_offset();
        for(unsigned n = 0; n < phi->get_num_incoming(); n++){
          ir::basic_block* inc_block = phi->get_incoming_block(n);
          ir::value* inc_val = phi->get_incoming_value(n);
          ir::instruction* terminator = inc_block->get_inst_list().back();
          BasicBlock *llvm_inc_block = last_block.at(inc_block);
          shared_tile *inc_shared = (shared_tile*)tmap_.at(inc_val);
          bool is_loop_latch = buffer_info_->is_loop_latch(phi, terminator);
          if(is_loop_latch){
            dst_builder.SetInsertPoint(llvm_inc_block->getTerminator());
            Value *next_offset = dst_builder.CreateNeg(offset);
            offset->addIncoming(next_offset, llvm_inc_block);
          }
          else {
            offset->addIncoming(dst_builder.getInt32(alloc_->get_num_bytes(phi)/(2*4)), llvm_inc_block);
          }
          ptr->addIncoming(inc_shared->get_pointer(), llvm_inc_block);
        }
      }
      else {
        for(unsigned n = 0; n < phi->get_num_incoming(); n++){
          ir::value *inc_val = phi->get_incoming_value(n);
          ir::basic_block *inc_block = phi->get_incoming_block(n);
          BasicBlock *llvm_inc_block = last_block.at(inc_block);
          if(phi->get_type()->is_tile_ty()) {
            distributed_tile *phi_tile = (distributed_tile*)tmap_.at(phi);
            distributed_tile *inc_tile = (distributed_tile*)tmap_.at(inc_val);
            phi_tile->for_each([&](indices_t idx){
              PHINode *llvm_phi = (PHINode*)phi_tile->get_value(idx);
              Value *llvm_inc_val = inc_tile->get_value(idx);
              llvm_phi->addIncoming(llvm_inc_val, llvm_inc_block);
            });
          }
          else {
            PHINode *llvm_phi = (PHINode*)llvm_value(phi, dst_builder);
            Value *llvm_inc_val = llvm_value(inc_val, dst_builder);
            llvm_phi->addIncoming(llvm_inc_val, llvm_inc_block);
          }
        }
      }
    }
  }
}


}
}
