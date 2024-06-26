/*******************************************************************************
 *     ___                  _   ____  ____
 *    / _ \ _   _  ___  ___| |_|  _ \| __ )
 *   | | | | | | |/ _ \/ __| __| | | |  _ \
 *   | |_| | |_| |  __/\__ \ |_| |_| | |_) |
 *    \__\_\\__,_|\___||___/\__|____/|____/
 *
 *  Copyright (c) 2014-2019 Appsicle
 *  Copyright (c) 2019-2024 QuestDB
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

#ifndef QUESTDB_JIT_X86_H
#define QUESTDB_JIT_X86_H

#include "common.h"
#include "impl/x86.h"

namespace questdb::x86 {
    using namespace asmjit::x86;

    jit_value_t
    read_vars_mem(Compiler &c, data_type_t type, int32_t idx, const Gp &vars_ptr) {
        auto shift = type_shift(type);
        auto type_size = 1 << shift;
        return {Mem(vars_ptr, 8 * idx, type_size), type, data_kind_t::kMemory};
    }

    // Reads length of variable size column with header stored in data vector (string, binary).
    jit_value_t read_mem_varsize(Compiler &c,
                                 uint32_t header_size,
                                 int32_t column_idx,
                                 const Gp &data_ptr,
                                 const Gp &varsize_aux_ptr,
                                 const Gp &input_index) {
        // Column has variable-size data with header stored in data vector.
        // First, we load this and the next data vector offsets from the aux vector.
        // When the offset difference is zero, it can indicate an empty  value (length 0)
        // or a NULL (length -1). In the zero difference case, we have to load the header
        // from the data vector. In the positive difference case, the difference is equal
        // to the length, so there is no need to do an extra load.
        Label l_nonzero = c.newLabel();
        Gp offset = c.newInt64("offset");
        Gp length = c.newInt64("length");
        Gp varsize_aux_address = c.newInt64("varsize_aux_address");
        Gp next_input_index = c.newInt64("next_input_index");
        c.mov(next_input_index, input_index);
        c.inc(next_input_index);
        c.mov(varsize_aux_address, ptr(varsize_aux_ptr, 8 * column_idx, 8));
        auto offset_shift = type_shift(data_type_t::i64);
        auto offset_size = 1 << offset_shift;
        c.mov(offset, ptr(varsize_aux_address, input_index, offset_shift, 0, offset_size));
        c.mov(length, ptr(varsize_aux_address, next_input_index, offset_shift, 0, offset_size));
        c.sub(length, offset);
        c.sub(length, header_size);
        // length now contains the length of the value. It can be zero for two reasons:
        // empty value or NULL value.
        c.jnz(l_nonzero);
        // If it's zero, we have to load the actual header value, which can be 0 or -1.
        Gp column_address = c.newInt64("column_address");
        c.mov(column_address, ptr(data_ptr, 8 * column_idx, 8));
        c.mov(length, ptr(column_address, offset, 0, 0, header_size));
        c.bind(l_nonzero);
        if (header_size == 4) {
            return {length.r32(), data_type_t::i32, data_kind_t::kMemory};
        }
        return {length, data_type_t::i64, data_kind_t::kMemory};
    }

    // Reads length part of the varchar header for aux vector.
    // This part is stored in the lowest bytes of the header
    // (see VarcharTypeDriver to understand the format).
    //
    // Note: unlike read_mem_varsize this method doesn't return the length,
    //       so it can only be used in NULL checks.
    jit_value_t read_mem_varchar_header(Compiler &c,
                                        int32_t column_idx,
                                        const Gp &varsize_aux_ptr,
                                        const Gp &input_index) {
        Gp varsize_aux_address = c.newInt64("varsize_aux_address");
        c.mov(varsize_aux_address, ptr(varsize_aux_ptr, 8 * column_idx, 8));

        Gp header_offset = c.newInt64("header_offset");
        c.mov(header_offset, input_index);
        auto header_shift = type_shift(data_type_t::i128);
        c.sal(header_offset, header_shift);

        Gp header = c.newInt64("header");
        c.mov(header, ptr(varsize_aux_address, header_offset, 0));

        return {header, data_type_t::i64, data_kind_t::kMemory};
    }

    jit_value_t read_mem(
            Compiler &c, data_type_t type, int32_t column_idx, const Gp &data_ptr,
            const Gp &varsize_aux_ptr, const Gp &input_index
    ) {
        if (type == data_type_t::varchar_header) {
            return read_mem_varchar_header(c, column_idx, varsize_aux_ptr, input_index);
        }

        uint32_t header_size;
        switch (type) {
            case data_type_t::string_header:
                header_size = 4;
                break;
            case data_type_t::binary_header:
                header_size = 8;
                break;
            default:
                header_size = 0;
        }
        if (header_size != 0) {
            return read_mem_varsize(c, header_size, column_idx, data_ptr, varsize_aux_ptr, input_index);
        }

        // Simple case: column has fixed-length data.

        Gp column_address = c.newInt64("column_address");
        c.mov(column_address, ptr(data_ptr, 8 * column_idx, 8));

        auto shift = type_shift(type);
        auto type_size = 1 << shift;
        if (type_size <= 8) {
            return {Mem(column_address, input_index, shift, 0, type_size), type, data_kind_t::kMemory};
        } else {
            Gp offset = c.newInt64("row_offset");
            c.mov(offset, input_index);
            c.sal(offset, shift);
            return {Mem(column_address, offset, 0, 0, type_size), type, data_kind_t::kMemory};
        }
    }

    jit_value_t mem2reg(Compiler &c, const jit_value_t &v) {
        auto type = v.dtype();
        auto mem = v.op().as<Mem>();
        switch (type) {
            case data_type_t::i8: {
                Gp row_data = c.newGpd("i8_mem");
                c.movsx(row_data, mem);
                return {row_data, type, data_kind_t::kMemory};
            }
            case data_type_t::i16: {
                Gp row_data = c.newGpd("i16_mem");
                c.movsx(row_data, mem);
                return {row_data, type, data_kind_t::kMemory};
            }
            case data_type_t::i32: {
                Gp row_data = c.newGpd("i32_mem");
                c.mov(row_data, mem);
                return {row_data, type, data_kind_t::kMemory};
            }
            case data_type_t::i64: {
                Gp row_data = c.newGpq("i64_mem");
                c.mov(row_data, mem);
                return {row_data, type, data_kind_t::kMemory};
            }
            case data_type_t::i128: {
                Xmm row_data = c.newXmm("i128_mem");
                c.movdqu(row_data, mem);
                return {row_data, type, data_kind_t::kMemory};
            }
            case data_type_t::f32: {
                Xmm row_data = c.newXmmSs("f32_mem");
                c.movss(row_data, mem);
                return {row_data, type, data_kind_t::kMemory};
            }
            case data_type_t::f64: {
                Xmm row_data = c.newXmmSd("f64_mem");
                c.movsd(row_data, mem);
                return {row_data, type, data_kind_t::kMemory};
            }
            default:
                __builtin_unreachable();
        }
    }

    jit_value_t read_imm(Compiler &c, const instruction_t &instr) {
        auto type = static_cast<data_type_t>(instr.options);
        switch (type) {
            case data_type_t::i8:
            case data_type_t::i16:
            case data_type_t::i32:
            case data_type_t::i64: {
                return {imm(instr.ipayload.lo), type, data_kind_t::kConst};
            }
            case data_type_t::i128: {
                return {
                    c.newConst(ConstPool::kScopeLocal, &instr.ipayload, 16),
                    type,
                    data_kind_t::kMemory
                };
            }
            case data_type_t::f32:
            case data_type_t::f64: {
                return {imm(instr.dpayload), type, data_kind_t::kConst};
            }
            default:
                __builtin_unreachable();
        }
    }

    bool is_int32(int64_t x) {
        return x >= std::numeric_limits<int32_t>::min() && x <= std::numeric_limits<int32_t>::max();
    }

    bool is_float(double x) {
        return x >= std::numeric_limits<float>::min() && x <= std::numeric_limits<float>::max();
    }

    jit_value_t imm2reg(Compiler &c, data_type_t dst_type, const jit_value_t &v) {
        Imm k = v.op().as<Imm>();
        if (k.isInteger()) {
            auto value = k.valueAs<int64_t>();
            switch (dst_type) {
                case data_type_t::f32: {
                    Xmm reg = c.newXmmSs("f32_imm %f", value);
                    Mem mem = c.newFloatConst(ConstPool::kScopeLocal, static_cast<float>(value));
                    c.movss(reg, mem);
                    return {reg, data_type_t::f32, data_kind_t::kConst};
                }
                case data_type_t::f64: {
                    Xmm reg = c.newXmmSd("f64_imm %f", (double) value);
                    Mem mem = c.newDoubleConst(ConstPool::kScopeLocal, static_cast<double>(value));
                    c.movsd(reg, mem);
                    return {reg, data_type_t::f64, data_kind_t::kConst};
                }
                default: {
                    if (dst_type == data_type_t::i64 || !is_int32(value)) {
                        Gp reg = c.newGpq("i64_imm %d", value);
                        c.movabs(reg, value);
                        return {reg, data_type_t::i64, data_kind_t::kConst};
                    } else {
                        Gp reg = c.newGpd("i32_imm %d", value);
                        c.mov(reg, value);
                        return {reg, dst_type, data_kind_t::kConst};
                    }
                }
            }
        } else {
            auto value = k.valueAs<double>();
            if (dst_type == data_type_t::i64 || dst_type == data_type_t::f64 || !is_float(value)) {
                Xmm reg = c.newXmmSd("f64_imm %f", value);
                Mem mem = c.newDoubleConst(ConstPool::kScopeLocal, static_cast<double>(value));
                c.movsd(reg, mem);
                return {reg, data_type_t::f64, data_kind_t::kConst};
            } else {
                Xmm reg = c.newXmmSs("f32_imm %f", value);
                Mem mem = c.newFloatConst(ConstPool::kScopeLocal, static_cast<float>(value));
                c.movss(reg, mem);
                return {reg, data_type_t::f32, data_kind_t::kConst};
            }
        }
    }

    jit_value_t load_register(Compiler &c, data_type_t dst_type, const jit_value_t &v) {
        if (v.op().isImm()) {
            return imm2reg(c, dst_type, v);
        } else if (v.op().isMem()) {
            return mem2reg(c, v);
        } else {
            return v;
        }
    }

    jit_value_t load_register(Compiler &c, const jit_value_t &v) {
        return load_register(c, v.dtype(), v);
    }

    std::pair<jit_value_t, jit_value_t> load_registers(Compiler &c, const jit_value_t &lhs, const jit_value_t &rhs) {
        data_type_t lt;
        data_type_t rt;
        if (lhs.op().isImm() && !rhs.op().isImm()) {
            lt = rhs.dtype();
            rt = rhs.dtype();
        } else if (rhs.op().isImm() && !lhs.op().isImm()) {
            lt = lhs.dtype();
            rt = lhs.dtype();
        } else {
            lt = lhs.dtype();
            rt = rhs.dtype();
        }
        jit_value_t l = load_register(c, lt, lhs);
        jit_value_t r = load_register(c, rt, rhs);
        return {l, r};
    }

    jit_value_t neg(Compiler &c, const jit_value_t &lhs, bool null_check) {
        auto dt = lhs.dtype();
        auto dk = lhs.dkind();
        switch (dt) {
            case data_type_t::i8:
            case data_type_t::i16:
            case data_type_t::i32:
                return {int32_neg(c, lhs.gp().r32(), null_check), dt, dk};
            case data_type_t::i64:
                return {int64_neg(c, lhs.gp(), null_check), dt, dk};
            case data_type_t::f32:
                return {float_neg(c, lhs.xmm()), dt, dk};
            case data_type_t::f64:
                return {double_neg(c, lhs.xmm()), dt, dk};
            default:
                __builtin_unreachable();
        }
    }

    jit_value_t bin_not(Compiler &c, const jit_value_t &lhs) {
        auto dt = lhs.dtype();
        auto dk = lhs.dkind();
        return {int32_not(c, lhs.gp().r32()), dt, dk};
    }

    jit_value_t bin_and(Compiler &c, const jit_value_t &lhs, const jit_value_t &rhs) {
        auto dt = lhs.dtype();
        auto dk = dst_kind(lhs, rhs);
        return {int32_and(c, lhs.gp().r32(), rhs.gp().r32()), dt, dk};
    }

    jit_value_t bin_or(Compiler &c, const jit_value_t &lhs, const jit_value_t &rhs) {
        auto dt = lhs.dtype();
        auto dk = dst_kind(lhs, rhs);
        return {int32_or(c, lhs.gp().r32(), rhs.gp().r32()), dt, dk};
    }

    jit_value_t cmp_eq(Compiler &c, const jit_value_t &lhs, const jit_value_t &rhs) {
        auto dt = lhs.dtype();
        auto dk = dst_kind(lhs, rhs);
        switch (dt) {
            case data_type_t::i8:
            case data_type_t::i16:
            case data_type_t::i32:
            case data_type_t::string_header:
                return {int32_eq(c, lhs.gp().r32(), rhs.gp().r32()), data_type_t::i32, dk};
            case data_type_t::i64:
            case data_type_t::binary_header:
            case data_type_t::varchar_header:
                return {int64_eq(c, lhs.gp(), rhs.gp()), data_type_t::i32, dk};
            case data_type_t::i128:
                return {int128_eq(c, lhs.xmm(), rhs.xmm()), data_type_t::i32, dk};
            case data_type_t::f32:
                return {float_eq_epsilon(c, lhs.xmm(), rhs.xmm(), FLOAT_EPSILON), data_type_t::i32, dk};
            case data_type_t::f64:
                return {double_eq_epsilon(c, lhs.xmm(), rhs.xmm(), DOUBLE_EPSILON), data_type_t::i32, dk};
            default:
                __builtin_unreachable();
        }
    }

    jit_value_t cmp_ne(Compiler &c, const jit_value_t &lhs, const jit_value_t &rhs) {
        auto dt = lhs.dtype();
        auto dk = dst_kind(lhs, rhs);
        switch (dt) {
            case data_type_t::i8:
            case data_type_t::i16:
            case data_type_t::i32:
            case data_type_t::string_header:
                return {int32_ne(c, lhs.gp().r32(), rhs.gp().r32()), data_type_t::i32, dk};
            case data_type_t::i64:
            case data_type_t::binary_header:
            case data_type_t::varchar_header:
                return {int64_ne(c, lhs.gp(), rhs.gp()), data_type_t::i32, dk};
            case data_type_t::i128:
                return {int128_ne(c, lhs.xmm(), rhs.xmm()), data_type_t::i32, dk};
            case data_type_t::f32:
                return {float_ne_epsilon(c, lhs.xmm(), rhs.xmm(), FLOAT_EPSILON), data_type_t::i32, dk};
            case data_type_t::f64:
                return {double_ne_epsilon(c, lhs.xmm(), rhs.xmm(), DOUBLE_EPSILON), data_type_t::i32, dk};
            default:
                __builtin_unreachable();
        }
    }

    jit_value_t cmp_gt(Compiler &c, const jit_value_t &lhs, const jit_value_t &rhs, bool null_check) {
        auto dt = lhs.dtype();
        auto dk = dst_kind(lhs, rhs);
        switch (dt) {
            case data_type_t::i8:
            case data_type_t::i16:
            case data_type_t::i32:
                return {int32_gt(c, lhs.gp().r32(), rhs.gp().r32(), null_check), data_type_t::i32, dk};
            case data_type_t::i64:
                return {int64_gt(c, lhs.gp(), rhs.gp(), null_check), data_type_t::i32, dk};
            case data_type_t::f32: {
                Xmm l = c.newXmmSs("lhs_copy");
                c.movss(l, lhs.xmm());
                Xmm r = c.newXmmSs("rhs_copy");
                c.movss(r, rhs.xmm());
                return { bin_and(c,
                    {float_ne_epsilon(c, lhs.xmm(), rhs.xmm(), FLOAT_EPSILON), data_type_t::i32, dk},
                    {float_gt(c, l, r), data_type_t::i32, dk})
                }; 
            }
            case data_type_t::f64: {
                Xmm l = c.newXmmSd("lhs_copy");
                c.movsd(l, lhs.xmm());
                Xmm r = c.newXmmSd("rhs_copy");
                c.movsd(r, rhs.xmm());
                return { bin_and(c,
                    {double_ne_epsilon(c, lhs.xmm(), rhs.xmm(), DOUBLE_EPSILON), data_type_t::i32, dk},
                    {double_gt(c, l, r), data_type_t::i32, dk})
                }; 
            }
            default:
                __builtin_unreachable();
        }
    }

    jit_value_t cmp_ge(Compiler &c, const jit_value_t &lhs, const jit_value_t &rhs, bool null_check) {
        auto dt = lhs.dtype();
        auto dk = dst_kind(lhs, rhs);
        switch (dt) {
            case data_type_t::i8:
            case data_type_t::i16:
            case data_type_t::i32:
                return {int32_ge(c, lhs.gp().r32(), rhs.gp().r32(), null_check), data_type_t::i32, dk};
            case data_type_t::i64:
                return {int64_ge(c, lhs.gp(), rhs.gp(), null_check), data_type_t::i32, dk};
            case data_type_t::f32: {
                Xmm l = c.newXmmSs("lhs_copy");
                c.movss(l, lhs.xmm());
                Xmm r = c.newXmmSs("rhs_copy");
                c.movss(r, rhs.xmm());
                return { bin_or(c,
                    {float_eq_epsilon(c, lhs.xmm(), rhs.xmm(), FLOAT_EPSILON), data_type_t::i32, dk},
                    {float_ge(c, l, r), data_type_t::i32, dk})
                }; 
            }
            case data_type_t::f64: {
                Xmm l = c.newXmmSd("lhs_copy");
                c.movsd(l, lhs.xmm());
                Xmm r = c.newXmmSd("rhs_copy");
                c.movsd(r, rhs.xmm());
                return { bin_or(c,
                    {double_eq_epsilon(c, lhs.xmm(), rhs.xmm(), DOUBLE_EPSILON), data_type_t::i32, dk},
                    {double_ge(c, l, r), data_type_t::i32, dk})
                }; 
            }
            default:
                __builtin_unreachable();
        }
    }

    jit_value_t cmp_lt(Compiler &c, const jit_value_t &lhs, const jit_value_t &rhs, bool null_check) {
        auto dt = lhs.dtype();
        auto dk = dst_kind(lhs, rhs);
        switch (dt) {
            case data_type_t::i8:
            case data_type_t::i16:
            case data_type_t::i32:
                return {int32_lt(c, lhs.gp().r32(), rhs.gp().r32(), null_check), data_type_t::i32, dk};
            case data_type_t::i64:
                return {int64_lt(c, lhs.gp(), rhs.gp(), null_check), data_type_t::i32, dk};
            case data_type_t::f32: {
                Xmm l = c.newXmmSs("lhs_copy");
                c.movss(l, lhs.xmm());
                Xmm r = c.newXmmSs("rhs_copy");
                c.movss(r, rhs.xmm());
                return { bin_and(c,
                    {float_ne_epsilon(c, lhs.xmm(), rhs.xmm(), FLOAT_EPSILON), data_type_t::i32, dk},
                    {float_lt(c, l.xmm(), r.xmm()), data_type_t::i32, dk})
                }; 
            }
            case data_type_t::f64: {
                Xmm l = c.newXmmSd("lhs_copy");
                c.movsd(l, lhs.xmm());
                Xmm r = c.newXmmSd("rhs_copy");
                c.movsd(r, rhs.xmm());
                return { bin_and(c,
                    {double_ne_epsilon(c, lhs.xmm(), rhs.xmm(), DOUBLE_EPSILON), data_type_t::i32, dk},
                    {double_lt(c, l, r), data_type_t::i32, dk})
                }; 
            }
            default:
                __builtin_unreachable();
        }
    }

    jit_value_t cmp_le(Compiler &c, const jit_value_t &lhs, const jit_value_t &rhs, bool null_check) {
        auto dt = lhs.dtype();
        auto dk = dst_kind(lhs, rhs);
        switch (dt) {
            case data_type_t::i8:
            case data_type_t::i16:
            case data_type_t::i32:
                return {int32_le(c, lhs.gp().r32(), rhs.gp().r32(), null_check), data_type_t::i32, dk};
            case data_type_t::i64:
                return {int64_le(c, lhs.gp(), rhs.gp(), null_check), data_type_t::i32, dk};
            case data_type_t::f32: {
                Xmm l = c.newXmmSs("lhs_copy");
                c.movss(l, lhs.xmm());
                Xmm r = c.newXmmSs("rhs_copy");
                c.movss(r, rhs.xmm());
                return { bin_or(c,
                    {float_eq_epsilon(c, lhs.xmm(), rhs.xmm(), FLOAT_EPSILON), data_type_t::i32, dk},
                    {float_le(c, l, r), data_type_t::i32, dk})
                }; 
            }
            case data_type_t::f64: {
                Xmm l = c.newXmmSd("lhs_copy");
                c.movsd(l, lhs.xmm());
                Xmm r = c.newXmmSd("rhs_copy");
                c.movsd(r, rhs.xmm());
                return { bin_or(c,
                    {double_eq_epsilon(c, lhs.xmm(), rhs.xmm(), DOUBLE_EPSILON), data_type_t::i32, dk},
                    {double_le(c, l, r), data_type_t::i32, dk})
                }; 
            }
            default:
                __builtin_unreachable();
        }
    }

    jit_value_t add(Compiler &c, const jit_value_t &lhs, const jit_value_t &rhs, bool null_check) {
        auto dt = lhs.dtype();
        auto dk = dst_kind(lhs, rhs);
        switch (dt) {
            case data_type_t::i8:
            case data_type_t::i16:
            case data_type_t::i32:
                return {int32_add(c, lhs.gp().r32(), rhs.gp().r32(), null_check), dt, dk};
            case data_type_t::i64:
                return {int64_add(c, lhs.gp(), rhs.gp(), null_check), dt, dk};
            case data_type_t::f32:
                return {float_add(c, lhs.xmm(), rhs.xmm()), dt, dk};
            case data_type_t::f64:
                return {double_add(c, lhs.xmm(), rhs.xmm()), dt, dk};
            default:
                __builtin_unreachable();
        }
    }

    jit_value_t sub(Compiler &c, const jit_value_t &lhs, const jit_value_t &rhs, bool null_check) {
        auto dt = lhs.dtype();
        auto dk = dst_kind(lhs, rhs);
        switch (dt) {
            case data_type_t::i8:
            case data_type_t::i16:
            case data_type_t::i32:
                return {int32_sub(c, lhs.gp().r32(), rhs.gp().r32(), null_check), dt, dk};
            case data_type_t::i64:
                return {int64_sub(c, lhs.gp(), rhs.gp(), null_check), dt, dk};
            case data_type_t::f32:
                return {float_sub(c, lhs.xmm(), rhs.xmm()), dt, dk};
            case data_type_t::f64:
                return {double_sub(c, lhs.xmm(), rhs.xmm()), dt, dk};
            default:
                __builtin_unreachable();
        }
    }

    jit_value_t mul(Compiler &c, const jit_value_t &lhs, const jit_value_t &rhs, bool null_check) {
        auto dt = lhs.dtype();
        auto dk = dst_kind(lhs, rhs);
        switch (dt) {
            case data_type_t::i8:
            case data_type_t::i16:
            case data_type_t::i32:
                return {int32_mul(c, lhs.gp().r32(), rhs.gp().r32(), null_check), dt, dk};
            case data_type_t::i64:
                return {int64_mul(c, lhs.gp(), rhs.gp(), null_check), dt, dk};
            case data_type_t::f32:
                return {float_mul(c, lhs.xmm(), rhs.xmm()), dt, dk};
            case data_type_t::f64:
                return {double_mul(c, lhs.xmm(), rhs.xmm()), dt, dk};
            default:
                __builtin_unreachable();
        }
    }

    jit_value_t div(Compiler &c, const jit_value_t &lhs, const jit_value_t &rhs, bool null_check) {
        auto dt = lhs.dtype();
        auto dk = dst_kind(lhs, rhs);
        switch (dt) {
            case data_type_t::i8:
            case data_type_t::i16:
            case data_type_t::i32:
                return {int32_div(c, lhs.gp().r32(), rhs.gp().r32(), null_check), dt, dk};
            case data_type_t::i64:
                return {int64_div(c, lhs.gp(), rhs.gp(), null_check), dt, dk};
            case data_type_t::f32:
                return {float_div(c, lhs.xmm(), rhs.xmm()), dt, dk};
            case data_type_t::f64:
                return {double_div(c, lhs.xmm(), rhs.xmm()), dt, dk};
            default:
                __builtin_unreachable();
        }
    }

    inline bool cvt_null_check(data_type_t type) {
        return !(type == data_type_t::i8 || type == data_type_t::i16);
    }

    inline std::pair<jit_value_t, jit_value_t>
    convert(Compiler &c, const jit_value_t &lhs, const jit_value_t &rhs, bool null_check) {
        switch (lhs.dtype()) {
            case data_type_t::i8:
            case data_type_t::i16:
            case data_type_t::i32:
                switch (rhs.dtype()) {
                    case data_type_t::i8:
                    case data_type_t::i16:
                    case data_type_t::i32:
                        return std::make_pair(lhs, rhs);
                    case data_type_t::i64:
                        return std::make_pair(
                                jit_value_t(
                                        int32_to_int64(c, lhs.gp().r32(), null_check && cvt_null_check(lhs.dtype())),
                                        data_type_t::i64,
                                        lhs.dkind()), rhs);
                    case data_type_t::f32:
                        return std::make_pair(
                                jit_value_t(
                                        int32_to_float(c, lhs.gp().r32(), null_check && cvt_null_check(lhs.dtype())),
                                        data_type_t::f32,
                                        lhs.dkind()), rhs);
                    case data_type_t::f64:
                        return std::make_pair(
                                jit_value_t(
                                        int32_to_double(c, lhs.gp().r32(), null_check && cvt_null_check(lhs.dtype())),
                                        data_type_t::f64,
                                        lhs.dkind()), rhs);
                    default:
                        __builtin_unreachable();
                }
                break;
            case data_type_t::i64:
                switch (rhs.dtype()) {
                    case data_type_t::i8:
                    case data_type_t::i16:
                    case data_type_t::i32:
                        return std::make_pair(lhs,
                                              jit_value_t(
                                                      int32_to_int64(c, rhs.gp().r32(),
                                                                     null_check && cvt_null_check(rhs.dtype())),
                                                      data_type_t::i64, rhs.dkind()));
                    case data_type_t::i64:
                        return std::make_pair(lhs, rhs);
                    case data_type_t::f32:
                        return std::make_pair(
                                jit_value_t(int64_to_double(c, lhs.gp().r64(), null_check), data_type_t::f64,
                                            lhs.dkind()),
                                jit_value_t(float_to_double(c, rhs.xmm()), data_type_t::f64, rhs.dkind()));
                    case data_type_t::f64:
                        return std::make_pair(
                                jit_value_t(int64_to_double(c, lhs.gp(), null_check), data_type_t::f64, lhs.dkind()),
                                rhs);
                    default:
                        __builtin_unreachable();
                }
                break;
            case data_type_t::f32:
                switch (rhs.dtype()) {
                    case data_type_t::i8:
                    case data_type_t::i16:
                    case data_type_t::i32:
                        return std::make_pair(lhs,
                                              jit_value_t(
                                                      int32_to_float(c, rhs.gp().r32(),
                                                                     null_check && cvt_null_check(rhs.dtype())),
                                                      data_type_t::f32, rhs.dkind()));
                    case data_type_t::i64:
                        return std::make_pair(jit_value_t(float_to_double(c, lhs.xmm()), data_type_t::f64, lhs.dkind()),
                                              jit_value_t(int64_to_double(c, rhs.gp(), null_check), data_type_t::f64,
                                                          rhs.dkind()));
                    case data_type_t::f32:
                        return std::make_pair(lhs, rhs);
                    case data_type_t::f64:
                        return std::make_pair(jit_value_t(float_to_double(c, lhs.xmm()), data_type_t::f64, lhs.dkind()),
                                              rhs);
                    default:
                        __builtin_unreachable();
                }
                break;
            case data_type_t::f64:
                switch (rhs.dtype()) {
                    case data_type_t::i8:
                    case data_type_t::i16:
                    case data_type_t::i32:
                        return std::make_pair(lhs,
                                              jit_value_t(
                                                      int32_to_double(c, rhs.gp().r32(),
                                                                      null_check && cvt_null_check(rhs.dtype())),
                                                      data_type_t::f64, rhs.dkind()));
                    case data_type_t::i64:
                        return std::make_pair(lhs,
                                              jit_value_t(int64_to_double(c, rhs.gp(), null_check), data_type_t::f64,
                                                          rhs.dkind()));
                    case data_type_t::f32:
                        return std::make_pair(lhs,
                                              jit_value_t(float_to_double(c, rhs.xmm()),
                                                          data_type_t::f64,
                                                          rhs.dkind()));
                    case data_type_t::f64:
                        return std::make_pair(lhs, rhs);
                    default:
                        __builtin_unreachable();
                }
                break;
            case data_type_t::i128:
            case data_type_t::string_header:
            case data_type_t::binary_header:
            case data_type_t::varchar_header:
                return std::make_pair(lhs, rhs);
            default:
                __builtin_unreachable();
        }
    }

    inline jit_value_t get_argument(Compiler &c, ZoneStack<jit_value_t> &values) {
        auto arg = values.pop();
        return load_register(c, arg);
    }

    inline std::pair<jit_value_t, jit_value_t>
    get_arguments(Compiler &c, ZoneStack<jit_value_t> &values, bool null_check) {
        auto lhs = values.pop();
        auto rhs = values.pop();
        auto args = load_registers(c, lhs, rhs);
        return convert(c, args.first, args.second, null_check);
    }

    void emit_bin_op(Compiler &c, const instruction_t &instr, ZoneStack<jit_value_t> &values, bool null_check) {
        auto args = get_arguments(c, values, null_check);
        auto lhs = args.first;
        auto rhs = args.second;
        switch (instr.opcode) {
            case opcodes::And:
                values.append(bin_and(c, lhs, rhs));
                break;
            case opcodes::Or:
                values.append(bin_or(c, lhs, rhs));
                break;
            case opcodes::Eq:
                values.append(cmp_eq(c, lhs, rhs));
                break;
            case opcodes::Ne:
                values.append(cmp_ne(c, lhs, rhs));
                break;
            case opcodes::Gt:
                values.append(cmp_gt(c, lhs, rhs, null_check));
                break;
            case opcodes::Ge:
                values.append(cmp_ge(c, lhs, rhs, null_check));
                break;
            case opcodes::Lt:
                values.append(cmp_lt(c, lhs, rhs, null_check));
                break;
            case opcodes::Le:
                values.append(cmp_le(c, lhs, rhs, null_check));
                break;
            case opcodes::Add:
                values.append(add(c, lhs, rhs, null_check));
                break;
            case opcodes::Sub:
                values.append(sub(c, lhs, rhs, null_check));
                break;
            case opcodes::Mul:
                values.append(mul(c, lhs, rhs, null_check));
                break;
            case opcodes::Div:
                values.append(div(c, lhs, rhs, null_check));
                break;
            default:
                __builtin_unreachable();
        }
    }

    void
    emit_code(Compiler &c, const instruction_t *istream, size_t size, ZoneStack<jit_value_t> &values,
              bool null_check,
              const Gp &data_ptr,
              const Gp &varsize_aux_ptr,
              const Gp &vars_ptr,
              const Gp &input_index) {

        for (size_t i = 0; i < size; ++i) {
            auto &instr = istream[i];
            switch (instr.opcode) {
                case opcodes::Inv:
                    return; // todo: throw exception
                case opcodes::Ret:
                    return;
                case opcodes::Var: {
                    auto type = static_cast<data_type_t>(instr.options);
                    auto idx  = static_cast<int32_t>(instr.ipayload.lo);
                    values.append(read_vars_mem(c, type, idx, vars_ptr));
                }
                    break;
                case opcodes::Mem: {
                    auto type = static_cast<data_type_t>(instr.options);
                    auto idx  = static_cast<int32_t>(instr.ipayload.lo);
                    values.append(read_mem(c, type, idx, data_ptr, varsize_aux_ptr, input_index));
                }
                    break;
                case opcodes::Imm:
                    values.append(read_imm(c, instr));
                    break;
                case opcodes::Neg:
                    values.append(neg(c, get_argument(c, values), null_check));
                    break;
                case opcodes::Not:
                    values.append(bin_not(c, get_argument(c, values)));
                    break;
                default:
                    emit_bin_op(c, instr, values, null_check);
                    break;
            }
        }
    }
}

#endif //QUESTDB_JIT_X86_H
