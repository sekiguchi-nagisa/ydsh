/*
 * Copyright (C) 2016-2018 Nagisa Sekiguchi
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

#ifndef YDSH_OPCODE_H
#define YDSH_OPCODE_H

namespace ydsh {

/**
 * see (doc/opcode.md)
 */
#define OPCODE_LIST(OP) \
    OP(HALT         , 0,  0) \
    OP(ASSERT       , 0, -2) \
    OP(PRINT        , 4, -1) \
    OP(INSTANCE_OF  , 4,  0) \
    OP(CHECK_CAST   , 4,  0) \
    OP(PUSH_NULL    , 0,  1) \
    OP(PUSH_TRUE    , 0,  1) \
    OP(PUSH_FALSE   , 0,  1) \
    OP(PUSH_ESTRING , 0,  1) \
    OP(LOAD_CONST   , 1,  1) \
    OP(LOAD_CONST_W , 2,  1) \
    OP(LOAD_CONST_T , 3,  1) \
    OP(LOAD_GLOBAL  , 2,  1) \
    OP(STORE_GLOBAL , 2, -1) \
    OP(LOAD_LOCAL   , 1,  1) \
    OP(STORE_LOCAL  , 1, -1) \
    OP(LOAD_FIELD   , 2,  0) \
    OP(STORE_FIELD  , 2, -2) \
    OP(IMPORT_ENV   , 1, -1) \
    OP(LOAD_ENV     , 0,  0) \
    OP(STORE_ENV    , 0, -2) \
    OP(POP          , 0, -1) \
    OP(DUP          , 0,  1) \
    OP(DUP2         , 0,  2) \
    OP(SWAP         , 0,  0) \
    OP(NEW_STRING   , 0,  1) \
    OP(APPEND_STRING, 0, -1) \
    OP(NEW_ARRAY    , 4,  1) \
    OP(APPEND_ARRAY , 0, -1) \
    OP(NEW_MAP      , 4,  1) \
    OP(APPEND_MAP   , 0, -1) \
    OP(NEW_TUPLE    , 4,  1) \
    OP(NEW          , 4,  1) \
    OP(CALL_INIT    , 1,  0) \
    OP(CALL_METHOD  , 3,  1) \
    OP(CALL_FUNC    , 1,  1) \
    OP(CALL_NATIVE  , 8,  1) \
    OP(INIT_MODULE  , 0,  0) \
    OP(RETURN       , 0,  0) \
    OP(RETURN_V     , 0,  0) \
    OP(RETURN_UDC   , 0,  0) \
    OP(RETURN_SIG   , 0,  0) \
    OP(BRANCH       , 2, -1) \
    OP(GOTO         , 4,  0) \
    OP(THROW        , 0,  0) \
    OP(ENTER_FINALLY, 2,  1) \
    OP(EXIT_FINALLY , 0, -1) \
    OP(LOOKUP_HASH  , 0, -2) \
    OP(I32_TO_I64   , 0,  0) \
    OP(I64_TO_I32   , 0,  0) \
    OP(I32_TO_D     , 0,  0) \
    OP(I64_TO_D     , 0,  0) \
    OP(D_TO_I32     , 0,  0) \
    OP(D_TO_I64     , 0,  0) \
    OP(REF_EQ       , 0, -1) \
    OP(REF_NE       , 0, -1) \
    OP(FORK         , 3,  1) \
    OP(PIPELINE    , -1,  1) \
    OP(PIPELINE_LP , -1,  1) \
    OP(EXPAND_TILDE , 0,  0) \
    OP(NEW_CMD      , 0,  0) \
    OP(ADD_CMD_ARG  , 1, -1) \
    OP(CALL_CMD     , 0, -1) \
    OP(CALL_CMD_P   , 0, -1) \
    OP(BUILTIN_CMD  , 0,  1) \
    OP(BUILTIN_EVAL , 0,  1) \
    OP(BUILTIN_EXEC , 0,  1) \
    OP(NEW_REDIR    , 0,  1) \
    OP(ADD_REDIR_OP , 1, -1) \
    OP(DO_REDIR     , 0,  0) \
    OP(RAND         , 0,  1) \
    OP(GET_SECOND   , 0,  1) \
    OP(SET_SECOND   , 0, -1) \
    OP(UNWRAP       , 0,  0) \
    OP(CHECK_UNWRAP , 0,  0) \
    OP(TRY_UNWRAP   , 2,  0) \
    OP(NEW_INVALID  , 0,  1) \
    OP(RECLAIM_LOCAL, 2,  0)

enum class OpCode : unsigned char {
#define GEN_OPCODE(CODE, N, S) CODE,
    OPCODE_LIST(GEN_OPCODE)
#undef GEN_OPCODE
};

int getByteSize(OpCode code);

bool isTypeOp(OpCode code);

} // namespace ydsh

#endif //YDSH_OPCODE_H
