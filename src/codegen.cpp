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

#include "codegen.h"
#include "symbol_table.h"
#include "constant.h"
#include "core.h"
#include "cmd.h"

namespace ydsh {

int getByteSize(OpCode code) {
    char table[] = {
#define GEN_BYTE_SIZE(CODE, N, S) N,
            OPCODE_LIST(GEN_BYTE_SIZE)
#undef GEN_BYTE_SIZE
    };
    return table[static_cast<unsigned char>(code)];
}

bool isTypeOp(OpCode code) {
    switch(code) {
    case OpCode::PRINT:
    case OpCode::INSTANCE_OF:
    case OpCode::CHECK_CAST:
    case OpCode::NEW_ARRAY:
    case OpCode::NEW_MAP:
    case OpCode::NEW_TUPLE:
    case OpCode::NEW:
        ASSERT_BYTE_SIZE(code, 8);
        return true;
    default:
        return false;
    }
}


// ###############################
// ##     ByteCodeGenerator     ##
// ###############################

void ByteCodeGenerator::emitIns(OpCode op) {
    this->curBuilder().append8(static_cast<unsigned char>(op));

    // max stack depth size
    int table[] = {
#define GEN_SIZE_TABLE(C, N, S) S,
            OPCODE_LIST(GEN_SIZE_TABLE)
#undef GEN_SIZE_TABLE
    };
    int size = table[static_cast<unsigned int>(op)];
    this->curBuilder().stackDepthCount += size;
    int count = this->curBuilder().stackDepthCount;
    if(count > 0 && static_cast<unsigned short>(count) > this->curBuilder().maxStackDepth) {
        this->curBuilder().maxStackDepth = count;
    }
}

void ByteCodeGenerator::emitTypeIns(OpCode op, const DSType &type) {
    assert(isTypeOp(op));
    this->emit8byteIns(op, reinterpret_cast<unsigned long>(&type));
}

unsigned int ByteCodeGenerator::emitConstant(DSValue &&value) {
    this->curBuilder().constBuffer.push_back(std::move(value));
    unsigned int index = this->curBuilder().constBuffer.size() - 1;
    if(index > 0xFFFFFF) {
        fatal("const pool index is up to 24bit\n");
    }
    return index;
}

void ByteCodeGenerator::emitLdcIns(const DSValue &value) {
    this->emitLdcIns(DSValue(value));
}

void ByteCodeGenerator::emitLdcIns(DSValue &&value) {
    unsigned int index = this->emitConstant(std::move(value));
    if(index <= UINT8_MAX) {
        this->emit1byteIns(OpCode::LOAD_CONST, index);
    } else if(index <= UINT16_MAX) {
        this->emit2byteIns(OpCode::LOAD_CONST_W, index);
    } else {
        this->emit3byteIns(OpCode::LOAD_CONST_T, index);
    }
}

void ByteCodeGenerator::emitDescriptorIns(OpCode op, std::string &&desc) {
    unsigned short index = this->emitConstant(
            DSValue::create<String_Object>(this->symbolTable.get(TYPE::String), std::move(desc)));
    this->emit2byteIns(op, index);
}

void ByteCodeGenerator::generateToString() {
    if(this->handle_STR == nullptr) {
        this->handle_STR = this->symbolTable.get(TYPE::Any).lookupMethodHandle(this->symbolTable, std::string(OP_STR));
    }

    this->emitCallIns(OpCode::CALL_METHOD, 0, this->handle_STR->getMethodIndex());
}

static constexpr unsigned short toShort(OpCode op) {
    return static_cast<unsigned char>(op);
}

static constexpr unsigned short toShort(OpCode op1, OpCode op2) {
    return toShort(op1) | toShort(op2) << 8;
}

void ByteCodeGenerator::emitNumCastIns(const DSType &beforeType, const DSType &afterType) {
    const int beforeIndex = this->symbolTable.getNumTypeIndex(beforeType);
    const int afterIndex = this->symbolTable.getNumTypeIndex(afterType);

    assert(beforeIndex > -1 && beforeIndex < 8);
    assert(afterIndex > -1 && afterIndex < 8);

#define _1(L) toShort(OpCode::L)
#define _2(L, R) toShort(OpCode::L, OpCode::R)

    const unsigned short table[8][8] = {
            {_1(HALT),             _1(COPY_INT),        _1(COPY_INT),        _1(COPY_INT), _1(COPY_INT), _1(NEW_LONG),   _1(NEW_LONG),   _1(U32_TO_D)},
            {_1(TO_BYTE),          _1(HALT),            _1(TO_U16),          _1(COPY_INT), _1(COPY_INT), _1(I_NEW_LONG), _1(I_NEW_LONG), _1(I32_TO_D)},
            {_1(TO_BYTE),          _1(TO_I16),          _1(HALT),            _1(COPY_INT), _1(COPY_INT), _1(NEW_LONG),   _1(NEW_LONG),   _1(U32_TO_D)},
            {_1(TO_BYTE),          _1(TO_I16),          _1(TO_U16),          _1(HALT),     _1(COPY_INT), _1(I_NEW_LONG), _1(I_NEW_LONG), _1(I32_TO_D)},
            {_1(TO_BYTE),          _1(TO_I16),          _1(TO_U16),          _1(COPY_INT), _1(HALT),     _1(NEW_LONG),   _1(NEW_LONG),   _1(U32_TO_D)},
            {_2(NEW_INT,TO_BYTE),  _2(NEW_INT,TO_I16),  _2(NEW_INT,TO_U16),  _1(NEW_INT),  _1(NEW_INT),  _1(HALT),       _1(COPY_LONG),  _1(I64_TO_D)},
            {_2(NEW_INT,TO_BYTE),  _2(NEW_INT,TO_I16),  _2(NEW_INT,TO_U16),  _1(NEW_INT),  _1(NEW_INT),  _1(COPY_LONG),  _1(HALT),       _1(U64_TO_D)},
            {_2(D_TO_U32,TO_BYTE), _2(D_TO_I32,TO_I16), _2(D_TO_U32,TO_U16), _1(D_TO_I32), _1(D_TO_U32), _1(D_TO_I64),   _1(D_TO_U64),   _1(HALT)},
    };

#undef _1
#undef _2

    const unsigned short v = table[beforeIndex][afterIndex];
    for(unsigned int i = 0; i < 2; i++) {
        const unsigned short mask = 0xFF << (i * 8);
        auto op = static_cast<OpCode>((mask & v) >> (i * 8));
        if(op != OpCode::HALT) {
            int size = getByteSize(op);
            assert(size == 0 || size == 1);
            if(size != 0) {
                this->emit1byteIns(op, afterIndex);
            } else {
                this->emit0byteIns(op);
            }
        }
    }
}

void ByteCodeGenerator::emitBranchIns(OpCode op, const Label &label) {
    const unsigned int index = this->curBuilder().codeBuffer.size();    //FIXME: check index range
    this->emit2byteIns(op, 0);
    this->curBuilder().writeLabel(index + 1, label, index, CodeEmitter<true>::LabelTarget::_16);
}

void ByteCodeGenerator::emitForkIns(ForkKind kind, const Label &label) {
    const unsigned int offset = this->curBuilder().codeBuffer.size();
    this->emitIns(OpCode::FORK);
    this->curBuilder().append8(static_cast<unsigned char>(kind));
    this->curBuilder().append16(0);
    this->curBuilder().writeLabel(offset + 2, label, offset + 1, CodeEmitter<true>::LabelTarget::_16);
}

void ByteCodeGenerator::emitJumpIns(const Label &label) {
    const unsigned int index = this->curBuilder().codeBuffer.size();
    this->emit4byteIns(OpCode::GOTO, 0);
    this->curBuilder().writeLabel(index + 1, label, 0, CodeEmitter<true>::LabelTarget::_32);
}

void ByteCodeGenerator::markLabel(Label &label) {
    const unsigned int index = this->curBuilder().codeBuffer.size();
    this->curBuilder().markLabel(index, label);
}

void ByteCodeGenerator::pushLoopLabels(Label breakLabel, Label continueLabel, Label breakWithValueLabel) {
    LoopState s = {
            .breakLabel = std::move(breakLabel),
            .continueLabel = std::move(continueLabel),
            .breakWithValueLabel = std::move(breakWithValueLabel),
            .blockIndex = static_cast<unsigned int>(this->curBuilder().localVars.size()),
    };
    this->curBuilder().loopLabels.push_back(std::move(s));
}

void ByteCodeGenerator::popLoopLabels() {
    this->curBuilder().loopLabels.pop_back();
}

const LoopState &ByteCodeGenerator::peekLoopLabels() {
    return this->curBuilder().loopLabels.back();
}

void ByteCodeGenerator::emitSourcePos(unsigned int pos) {
    const unsigned int index = this->curBuilder().codeBuffer.size();
    if(this->curBuilder().sourcePosEntries.empty() || this->curBuilder().sourcePosEntries.back().pos != pos) {
        this->curBuilder().sourcePosEntries.push_back({index, pos});
    }
}

void ByteCodeGenerator::catchException(const Label &begin, const Label &end,
                                       const DSType &type, unsigned short localOffset, unsigned short localSize) {
    const unsigned int index = this->curBuilder().codeBuffer.size();
    this->curBuilder().catchBuilders.emplace_back(begin, end, type, index, localOffset, localSize);
}

void ByteCodeGenerator::enterFinally() {
    for(auto iter = this->curBuilder().finallyLabels.rbegin();
        iter != this->curBuilder().finallyLabels.rend(); ++iter) {
        const unsigned int index = this->curBuilder().codeBuffer.size();
        this->emit2byteIns(OpCode::ENTER_FINALLY, 0);
        this->curBuilder().writeLabel(index + 1, *iter, index, CodeEmitter<true>::LabelTarget::_16);
    }
}

static bool isTildeExpansion(const Node *node) {
    return node->is(NodeKind::String) && static_cast<const StringNode *>(node)->isTilde();
}

void ByteCodeGenerator::generateCmdArg(CmdArgNode &node) {
    const unsigned int size = node.getSegmentNodes().size();

    if(size == 1) {
        this->visit(*node.getSegmentNodes()[0]);
    } else {
        this->emit0byteIns(OpCode::NEW_STRING);

        unsigned int index = 0;
        const bool tildeExpansion = isTildeExpansion(node.getSegmentNodes()[0]);
        if(tildeExpansion) {
            this->emitLdcIns(DSValue::create<String_Object>(
                    this->symbolTable.get(TYPE::String), static_cast<StringNode *>(node.getSegmentNodes()[0])->getValue()));
            this->emit0byteIns(OpCode::APPEND_STRING);
            index++;
        }

        for(; index < size; index++) {
            auto *e = node.getSegmentNodes()[index];
            if(e->is(NodeKind::StringExpr) && static_cast<StringExprNode *>(e)->getExprNodes().size() > 1) {
                this->generateStringExpr(*static_cast<StringExprNode *>(e), true);
            } else {
                this->visit(*e);
                this->emit0byteIns(OpCode::APPEND_STRING);
            }
        }

        if(tildeExpansion) {
            this->emit0byteIns(OpCode::EXPAND_TILDE);
        }
    }
}

void ByteCodeGenerator::emitPipelineIns(const std::vector<Label> &labels) {
    const unsigned int size = labels.size();
    if(size > UINT8_MAX) {
        fatal("reach limit\n");
    }

    const unsigned int offset = this->curBuilder().codeBuffer.size();
    this->emitIns(OpCode::PIPELINE);
    this->curBuilder().append8(size);
    for(unsigned int i = 0; i < size; i++) {
        this->curBuilder().append16(0);
        this->curBuilder().writeLabel(offset + 2 + i * 2, labels[i], offset, CodeEmitter<true>::LabelTarget::_16);
    }
}

void ByteCodeGenerator::generateStringExpr(StringExprNode &node, bool fragment) {
    const unsigned int size = node.getExprNodes().size();
    if(size == 0) {
        if(!fragment) {
            this->emit0byteIns(OpCode::PUSH_ESTRING);
        }
    } else if(size == 1) {
        this->visit(*node.getExprNodes()[0]);
    } else {
        if(!fragment) {
            this->emit0byteIns(OpCode::NEW_STRING);
        }
        unsigned int count = 0;
        for(Node *e : node.getExprNodes()) {
            if(e->is(NodeKind::BinaryOp)) {
                auto *binary = static_cast<BinaryOpNode *>(e);
                if(binary->getOptNode()->is(NodeKind::StringExpr)) {
                    for(Node *e2 : static_cast<StringExprNode *>(binary->getOptNode())->getExprNodes()) {
                        this->visit(*e2);
                        this->emit0byteIns(OpCode::APPEND_STRING);
                    }
                    continue;
                }
            }
            this->visit(*e);
            if(count++ == 0 && e->is(NodeKind::Empty)) {
                /**
                 * When calling `APPEND_STRING' ins, the operand stack layout is the following
                 *
                 * +-----------------------------+-------+
                 * | buf (created by NEW_STRING) | value |
                 * +-----------------------------+-------+
                 *
                 * However, when string self assignment, first expr is empty expression
                 * (due to self assignment implementation, see. AssignNode).
                 * As a result, the stack layout will be the following
                 *
                 * +-------+-----+
                 * | value | buf |
                 * +-------+-----+
                 *
                 * In this situation `APPEND_STRING' ins breaks stack top string object (due to appending buf to value).
                 * To prevent it, swap stack top two values.
                 */
                this->emit0byteIns(OpCode::SWAP);
            }

            this->emit0byteIns(OpCode::APPEND_STRING);
        }
    }
}


// visitor api
void ByteCodeGenerator::visit(Node &node) {
    node.accept(*this);
}

void ByteCodeGenerator::visitTypeNode(TypeNode &) {
    fatal("unsupported\n");
}

void ByteCodeGenerator::visitNumberNode(NumberNode &node) {
    DSValue value;
    switch(node.kind) {
    case NumberNode::Byte:
    case NumberNode::Int16:
    case NumberNode::Uint16:
    case NumberNode::Int32:
    case NumberNode::Uint32:
        value = DSValue::create<Int_Object>(node.getType(), node.getIntValue());
        break;
    case NumberNode::Int64:
    case NumberNode::Uint64:
        value = DSValue::create<Long_Object>(node.getType(), node.getLongValue());
        break;
    case NumberNode::Float:
        value = DSValue::create<Float_Object>(node.getType(), node.getFloatValue());
        break;
    case NumberNode::Signal:
        value = DSValue::create<Int_Object>(node.getType(), node.getIntValue());
        break;
    }
    this->emitLdcIns(std::move(value));
}

void ByteCodeGenerator::visitStringNode(StringNode &node) {
    if(node.getValue().empty()) {
        this->emit0byteIns(OpCode::PUSH_ESTRING);
    } else {
        bool isTilde = node.isTilde();
        auto &type = node.getType();
        this->emitLdcIns(DSValue::create<String_Object>(type, StringNode::extract(std::move(node))));
        if(isTilde) {
            this->emit0byteIns(OpCode::EXPAND_TILDE);
        }
    }
}

void ByteCodeGenerator::visitStringExprNode(StringExprNode &node) {
    this->generateStringExpr(node, false);
}

void ByteCodeGenerator::visitRegexNode(RegexNode &node) {
    this->emitLdcIns(DSValue::create<Regex_Object>(node.getType(), node.extractRE()));
}

void ByteCodeGenerator::visitArrayNode(ArrayNode &node) {
    this->emitTypeIns(OpCode::NEW_ARRAY, node.getType());
    for(Node *e : node.getExprNodes()) {
        this->visit(*e);
        this->emit0byteIns(OpCode::APPEND_ARRAY);
    }
}

void ByteCodeGenerator::visitMapNode(MapNode &node) {
    this->emitTypeIns(OpCode::NEW_MAP, node.getType());
    const unsigned int size = node.getKeyNodes().size();
    for(unsigned int i = 0; i < size; i++) {
        this->visit(*node.getKeyNodes()[i]);
        this->visit(*node.getValueNodes()[i]);
        this->emit0byteIns(OpCode::APPEND_MAP);
    }
}

void ByteCodeGenerator::visitTupleNode(TupleNode &node) {
    this->emitTypeIns(OpCode::NEW_TUPLE, node.getType());
    const unsigned int size = node.getNodes().size();
    for(unsigned int i = 0; i < size; i++) {
        this->emit0byteIns(OpCode::DUP);
        this->visit(*node.getNodes()[i]);
        this->emit2byteIns(OpCode::STORE_FIELD, i);
    }
}

void ByteCodeGenerator::visitVarNode(VarNode &node) {
    if(node.attr().has(FieldAttribute::ENV)) {
        if(node.attr().has(FieldAttribute::GLOBAL)) {
            this->emit2byteIns(OpCode::LOAD_GLOBAL, node.getIndex());
        } else {
            this->emit1byteIns(OpCode::LOAD_LOCAL, node.getIndex());
        }

        this->emit0byteIns(OpCode::LOAD_ENV);
    } else if(node.attr().has(FieldAttribute::RANDOM)) {
        this->emit0byteIns(OpCode::RAND);
    } else if(node.attr().has(FieldAttribute::SECONDS)) {
        this->emit0byteIns(OpCode::GET_SECOND);
    } else {
        if(node.attr().has(FieldAttribute::GLOBAL)) {
            this->emit2byteIns(OpCode::LOAD_GLOBAL, node.getIndex());
        } else {
            this->emit1byteIns(OpCode::LOAD_LOCAL, node.getIndex());
        }
    }
}

void ByteCodeGenerator::visitAccessNode(AccessNode &node) {
    this->visit(*node.getRecvNode());

    if(node.getRecvNode()->getType().isModType()) {
        this->emit0byteIns(OpCode::POP);
        this->emit2byteIns(OpCode::LOAD_GLOBAL, node.getIndex());
        return;
    }

    switch(node.getAdditionalOp()) {
    case AccessNode::NOP: {
        if(node.attr().has(FieldAttribute::INTERFACE)) {
            std::string desc = encodeFieldDescriptor(
                    node.getRecvNode()->getType(), node.getFieldName().c_str(), node.getType());
            this->emitDescriptorIns(OpCode::INVOKE_GETTER, std::move(desc));
        } else {
            this->emit2byteIns(OpCode::LOAD_FIELD, node.getIndex());
        }
        break;
    }
    case AccessNode::DUP_RECV: {
        this->emit0byteIns(OpCode::DUP);

        if(node.attr().has(FieldAttribute::INTERFACE)) {
            std::string desc = encodeFieldDescriptor(
                    node.getRecvNode()->getType(), node.getFieldName().c_str(), node.getType());
            this->emitDescriptorIns(OpCode::INVOKE_GETTER, std::move(desc));
        } else {
            this->emit2byteIns(OpCode::LOAD_FIELD, node.getIndex());
        }
        break;
    }
    }
}

void ByteCodeGenerator::visitTypeOpNode(TypeOpNode &node) {
    this->visit(*node.getExprNode());

    switch(node.getOpKind()) {
    case TypeOpNode::NO_CAST:
        break;
    case TypeOpNode::TO_VOID:
        this->emit0byteIns(OpCode::POP);
        break;
    case TypeOpNode::NUM_CAST:
        this->emitNumCastIns(node.getExprNode()->getType(), node.getType());
        break;
    case TypeOpNode::TO_STRING:
        this->emitSourcePos(node.getPos());
        this->generateToString();
        break;
    case TypeOpNode::TO_BOOL: {
        this->emitSourcePos(node.getPos());
        auto *handle = node.getExprNode()->getType().lookupMethodHandle(this->symbolTable, OP_BOOL);
        assert(handle != nullptr);
        this->emitCallIns(OpCode::CALL_METHOD, 0, handle->getMethodIndex());
        break;
    }
    case TypeOpNode::CHECK_CAST:
        this->emitSourcePos(node.getPos());
        this->emitTypeIns(OpCode::CHECK_CAST, node.getType());
        break;
    case TypeOpNode::CHECK_UNWRAP:
        this->emitSourcePos(node.getPos());
        this->emit0byteIns(OpCode::CHECK_UNWRAP);
        break;
    case TypeOpNode::PRINT: {
        this->emitSourcePos(node.getPos());
        auto &exprType = node.getExprNode()->getType();
        if(exprType.isOptionType()) {
            auto elementType = static_cast<ReifiedType &>(exprType).getElementTypes()[0];

            auto thenLabel = makeLabel();
            auto mergeLabel = makeLabel();
            this->emitBranchIns(OpCode::TRY_UNWRAP, thenLabel);
            this->emitLdcIns(DSValue::create<String_Object>(this->symbolTable.get(TYPE::String), "(invalid)"));
            this->emitJumpIns(mergeLabel);

            this->markLabel(thenLabel);
            if(*elementType != this->symbolTable.get(TYPE::String)) {
                this->generateToString();
            }

            this->markLabel(mergeLabel);
        } else if(exprType != this->symbolTable.get(TYPE::String)) {
            this->generateToString();
        }
        this->emitTypeIns(OpCode::PRINT, exprType);
        break;
    }
    case TypeOpNode::ALWAYS_FALSE:
        this->emit0byteIns(OpCode::POP);
        this->emit0byteIns(OpCode::PUSH_FALSE);
        break;
    case TypeOpNode::ALWAYS_TRUE:
        this->emit0byteIns(OpCode::POP);
        this->emit0byteIns(OpCode::PUSH_TRUE);
        break;
    case TypeOpNode::INSTANCEOF:
        this->emitTypeIns(OpCode::INSTANCE_OF, node.getTargetTypeNode()->getType());
        break;
    }
}

void ByteCodeGenerator::visitUnaryOpNode(UnaryOpNode &node) {
    if(node.isUnwrapOp()) {
        this->visit(*node.getExprNode());
        this->emit0byteIns(OpCode::UNWRAP);
    } else {
        this->visit(*node.getApplyNode());
    }
}

void ByteCodeGenerator::visitBinaryOpNode(BinaryOpNode &node) {
    auto kind = node.getOp();
    if(kind == COND_AND || kind == COND_OR) {
        auto elseLabel = makeLabel();
        auto mergeLabel = makeLabel();

        this->visit(*node.getLeftNode());
        this->emitBranchIns(elseLabel);

        if(kind == COND_AND) {
            this->visit(*node.getRightNode());
            this->emitJumpIns(mergeLabel);

            this->markLabel(elseLabel);
            this->emit0byteIns(OpCode::PUSH_FALSE);
        } else {
            this->emit0byteIns(OpCode::PUSH_TRUE);
            this->emitJumpIns(mergeLabel);

            this->markLabel(elseLabel);
            this->visit(*node.getRightNode());
        }

        this->markLabel(mergeLabel);
    } else if(kind == NULL_COALE) {
        auto mergeLabel = makeLabel();

        this->visit(*node.getLeftNode());
        this->emitBranchIns(OpCode::TRY_UNWRAP, mergeLabel);

        this->visit(*node.getRightNode());
        this->markLabel(mergeLabel);
    } else if(node.getLeftNode() && node.getLeftNode()->getType().isFuncType()
              && node.getRightNode() && node.getRightNode()->getType().isFuncType()) {
        this->visit(*node.getLeftNode());
        this->visit(*node.getRightNode());
        if(kind == EQ) {
            this->emit0byteIns(OpCode::REF_EQ);
        } else {
            assert(kind == NE);
            this->emit0byteIns(OpCode::REF_NE);
        }
    } else {
        this->visit(*node.getOptNode());
    }
}

void ByteCodeGenerator::visitApplyNode(ApplyNode &node) {
    const unsigned int paramSize = node.getArgNodes().size();
    if(node.isMethodCall()) {
        this->visit(*node.getRecvNode());

        for(Node *e : node.getArgNodes()) {
            this->visit(*e);
        }

        this->emitSourcePos(node.getPos());
        if(node.getHandle()->isInterfaceMethod()) {
            this->emitDescriptorIns(OpCode::INVOKE_METHOD,
                                    encodeMethodDescriptor(node.getMethodName().c_str(), node.getHandle()));
        } else {
            this->emitCallIns(OpCode::CALL_METHOD, node.getArgNodes().size(), node.getHandle()->getMethodIndex());
        }
        return;
    } else {
        this->visit(*node.getExprNode());

        for(Node *e : node.getArgNodes()) {
            this->visit(*e);
        }

        this->emitSourcePos(node.getPos());
        this->emitCallIns(OpCode::CALL_FUNC, paramSize);
    }
}

void ByteCodeGenerator::visitNewNode(NewNode &node) {
    if(node.getType().isOptionType()) {
        this->emit0byteIns(OpCode::NEW_INVALID);
        return;
    }

    unsigned int paramSize = node.getArgNodes().size();

    this->emitTypeIns(OpCode::NEW, node.getType());

    // push arguments
    for(Node *argNode : node.getArgNodes()) {
        this->visit(*argNode);
    }

    // call constructor
    this->emitSourcePos(node.getPos());
    this->emitCallIns(OpCode::CALL_INIT, paramSize);
}

void ByteCodeGenerator::visitCmdNode(CmdNode &node) {
    this->emitSourcePos(node.getPos());

    this->visit(*node.getNameNode());
    this->emit0byteIns(OpCode::NEW_CMD);
    this->emit0byteIns(node.hasRedir() ? OpCode::NEW_REDIR : OpCode::PUSH_NULL);

    for(auto &argNode : node.getArgNodes()) {
        this->visit(*argNode);
    }

    if(node.hasRedir()) {
        this->emit0byteIns(OpCode::DO_REDIR);
    }

    OpCode ins = node.getInLastPipe() ? OpCode::CALL_CMD_LP :
                 node.getInPipe() ? OpCode::CALL_CMD_P : OpCode::CALL_CMD;
    this->emit0byteIns(ins);
}

void ByteCodeGenerator::visitCmdArgNode(CmdArgNode &node) {
    this->generateCmdArg(node);
    this->emit1byteIns(OpCode::ADD_CMD_ARG, node.isIgnorableEmptyString() ? 1 : 0);
}

static RedirOP resolveRedirOp(TokenKind kind) {
    switch(kind) {
#define GEN_CASE(ENUM) case REDIR_##ENUM : return RedirOP::ENUM;
    EACH_RedirOP(GEN_CASE)
#undef GEN_CASE
    default:
        fatal("unsupported redir op: %s\n", toString(kind));
    }
}

void ByteCodeGenerator::visitRedirNode(RedirNode &node) {
    this->generateCmdArg(*node.getTargetNode());
    this->emit1byteIns(OpCode::ADD_REDIR_OP, static_cast<unsigned char>(resolveRedirOp(node.getRedirectOP())));
}

void ByteCodeGenerator::visitPipelineNode(PipelineNode &node) {
    const unsigned int size = node.getNodes().size();

    // init label
    std::vector<Label> labels(size);
    for(unsigned int i = 0; i < size; i++) {
        labels[i] = makeLabel();
    }

    // generate pipeline
    this->emitSourcePos(node.getPos());
    this->emitPipelineIns(labels);

    auto begin = makeLabel();
    auto end = makeLabel();

    // generate pipeline (child)
    this->markLabel(begin);
    for(unsigned int i = 0; i < size - 1; i++) {
        this->markLabel(labels[i]);
        this->visit(*node.getNodes()[i]);
        this->emit0byteIns(OpCode::HALT);
    }
    this->markLabel(end);
    this->catchException(begin, end, this->symbolTable.get(TYPE::_Root));

    // generate last pipe
    this->markLabel(labels[size - 1]);
    this->generateBlock(node.getBaseIndex(), 1, true, [&] {
        this->emit1byteIns(OpCode::STORE_LOCAL, node.getBaseIndex());
        this->visit(*node.getNodes()[size - 1]);
    });
}

void ByteCodeGenerator::visitWithNode(WithNode &node) {
    this->generateBlock(node.getBaseIndex(), 1, true, [&] {
        this->emit0byteIns(OpCode::NEW_REDIR);
        for(auto &e : node.getRedirNodes()) {
            this->visit(*e);
        }
        this->emit0byteIns(OpCode::DO_REDIR);
        this->emit1byteIns(OpCode::STORE_LOCAL, node.getBaseIndex());

        this->visit(*node.getExprNode());
    });
}

static ForkKind resolveForkKind(ForkNode::OpKind kind) {
    switch(kind) {
    case ForkNode::SUB_STR:
        return ForkKind::STR;
    case ForkNode::SUB_ARRAY:
        return ForkKind::ARRAY;
    case ForkNode::COPROC:
        return ForkKind::COPROC;
    case ForkNode::BG:
        return ForkKind::JOB;
    case ForkNode::DISOWN:
        return ForkKind::DISOWN;
    }
    return ForkKind::DISOWN;    // normally unreachable, due to suppress gcc warning
}

void ByteCodeGenerator::visitForkNode(ForkNode &node) {
    auto beginLabel = makeLabel();
    auto endLabel = makeLabel();
    auto mergeLabel = makeLabel();

    this->markLabel(beginLabel);
    this->emitForkIns(resolveForkKind(node.getOpKind()), mergeLabel);
    this->visit(*node.getExprNode());
    this->markLabel(endLabel);

    this->emit0byteIns(OpCode::HALT);
    this->catchException(beginLabel, endLabel, this->symbolTable.get(TYPE::_Root));
    this->markLabel(mergeLabel);
}

void ByteCodeGenerator::visitAssertNode(AssertNode &node) {
    if(this->assertion) {
        this->visit(*node.getCondNode());
        this->emitSourcePos(node.getCondNode()->getPos());
        this->visit(*node.getMessageNode());
        this->emit0byteIns(OpCode::ASSERT);
    }
}

void ByteCodeGenerator::visitBlockNode(BlockNode &node) {
    if(node.getNodes().empty()) {
        return;
    }

    this->generateBlock(node.getBaseIndex(), node.getVarSize(), needReclaim(node), [&]{
        for(auto &e : node.getNodes()) {
            this->visit(*e);
        }
    });
}

void ByteCodeGenerator::visitTypeAliasNode(TypeAliasNode &) { } // do nothing

static bool isEmptyCode(Node &node) {
    return node.is(NodeKind::Empty) ||
           (node.is(NodeKind::Block) && static_cast<BlockNode &>(node).getNodes().empty());
}

void ByteCodeGenerator::visitLoopNode(LoopNode &node) {
    // generate code
    unsigned short localOffset = 0;
    unsigned short localSize = 0;
    if(node.getInitNode()->is(NodeKind::VarDecl)) {
        localOffset = static_cast<VarDeclNode *>(node.getInitNode())->getVarIndex();
        localSize = 1;
    }

    this->generateBlock(localOffset, localSize, localSize > 0, [&]{
        // push loop label
        auto initLabel = makeLabel();
        auto startLabel = makeLabel();
        auto breakLabel = makeLabel();
        auto continueLabel = makeLabel();
        auto breakWithValueLabel = makeLabel();
        this->pushLoopLabels(breakLabel, continueLabel, breakWithValueLabel);

        this->visit(*node.getInitNode());
        if(!isEmptyCode(*node.getIterNode())) {
            this->emitJumpIns(initLabel);
        }

        if(node.isDoWhile()) {
            this->emitJumpIns(startLabel);
        }
        this->markLabel(continueLabel);
        this->visit(*node.getIterNode());

        this->markLabel(initLabel);
        if(node.getCondNode() != nullptr) {
            this->visit(*node.getCondNode());
        } else {
            this->emit0byteIns(OpCode::PUSH_TRUE);
        }
        this->emitBranchIns(breakLabel);

        this->markLabel(startLabel);
        this->visit(*node.getBlockNode());

        this->markLabel(breakLabel);
        if(!node.getType().isVoidType()) {
            this->emit0byteIns(OpCode::NEW_INVALID);
            this->markLabel(breakWithValueLabel);
        }

        // pop loop label
        this->popLoopLabels();
    });
}

void ByteCodeGenerator::visitIfNode(IfNode &node) {
    auto elseLabel = makeLabel();
    auto mergeLabel = makeLabel();

    this->visit(*node.getCondNode());
    this->emitBranchIns(elseLabel);
    this->visit(*node.getThenNode());
    if(!isEmptyCode(*node.getElseNode()) && !node.getThenNode()->getType().isNothingType()) {
        this->emitJumpIns(mergeLabel);
    }

    this->markLabel(elseLabel);
    this->visit(*node.getElseNode());

    this->markLabel(mergeLabel);
}

void ByteCodeGenerator::generateBreakContinue(JumpNode &node) {
    assert(!this->curBuilder().loopLabels.empty());

    // for break with value
    this->visit(*node.getExprNode());

    // reclaim local before jump
    unsigned int blockIndex = this->curBuilder().loopLabels.back().blockIndex;
    const unsigned int startOffset = this->curBuilder().localVars[blockIndex].first;
    unsigned int stopOffset = startOffset + this->curBuilder().localVars[blockIndex].second;

    const unsigned int size = this->curBuilder().localVars.size();
    for(; blockIndex < size; blockIndex++) {
        auto &pair = this->curBuilder().localVars[blockIndex];
        stopOffset = pair.first + pair.second;
    }

    if(stopOffset - startOffset > 0) {
        this->emit2byteIns(OpCode::RECLAIM_LOCAL, startOffset, stopOffset - startOffset);
    }

    // add finally before jump
    if(node.isLeavingBlock()) {
        this->enterFinally();
    }

    if(node.getOpKind() == JumpNode::BREAK) {
        if(node.getExprNode()->getType().isVoidType()) {
            this->emitJumpIns(this->peekLoopLabels().breakLabel);
        } else {
            this->emitJumpIns(this->peekLoopLabels().breakWithValueLabel);
        }
    } else {
        assert(node.getExprNode()->getType().isVoidType());
        this->emitJumpIns(this->peekLoopLabels().continueLabel);
    }
}

void ByteCodeGenerator::visitJumpNode(JumpNode &node) {
    switch(node.getOpKind()) {
    case JumpNode::BREAK:
    case JumpNode::CONTINUE:
        this->generateBreakContinue(node);
        break;
    case JumpNode::THROW: {
        this->visit(*node.getExprNode());
        this->emit0byteIns(OpCode::THROW);
        break;
    }
    case JumpNode::RETURN: {
        this->visit(*node.getExprNode());

        // add finally before return
        this->enterFinally();

        if(this->inUDC()) {
            assert(node.getExprNode()->getType() == this->symbolTable.get(TYPE::Int32));
            this->emit0byteIns(OpCode::RETURN_UDC);
        } else if(node.getExprNode()->getType().isVoidType()) {
            this->emit0byteIns(OpCode::RETURN);
        } else {
            this->emit0byteIns(OpCode::RETURN_V);
        }
        break;
    }
    }
}

void ByteCodeGenerator::visitCatchNode(CatchNode &node) {
    if(node.getBlockNode()->getNodes().empty()) {
        this->emit0byteIns(OpCode::POP);
    } else {
        this->emit1byteIns(OpCode::STORE_LOCAL, node.getVarIndex());
        this->visit(*node.getBlockNode());
    }
}

void ByteCodeGenerator::visitTryNode(TryNode &node) {
    auto finallyLabel = makeLabel();

    const bool hasFinally = node.getFinallyNode() != nullptr;
    if(hasFinally) {
        this->curBuilder().finallyLabels.push_back(finallyLabel);
    }

    auto beginLabel = makeLabel();
    auto endLabel = makeLabel();
    auto mergeLabel = makeLabel();

    // generate try block
    this->markLabel(beginLabel);
    this->visit(*node.getExprNode());
    this->markLabel(endLabel);
    if(!node.getExprNode()->getType().isNothingType()) {
        if(hasFinally) {
            this->enterFinally();
        }
        this->emitJumpIns(mergeLabel);
    }

    // generate catch
    auto &blockNode = *findInnerNode<BlockNode>(node.getExprNode());
    auto maxLocalSize = blockNode.getMaxVarSize();
    for(auto &catchNode : node.getCatchNodes()) {
        unsigned int varSize = findInnerNode<CatchNode>(catchNode)->getBlockNode()->getMaxVarSize();
        if(maxLocalSize < varSize) {
            maxLocalSize = varSize;
        }

        auto &catchType = findInnerNode<CatchNode>(catchNode)->getTypeNode()->getType();
        this->catchException(beginLabel, endLabel, catchType, blockNode.getBaseIndex(), blockNode.getVarSize());
        this->visit(*catchNode);
        if(!catchNode->getType().isNothingType()) {
            if(hasFinally) {
                this->enterFinally();
            }
            this->emitJumpIns(mergeLabel);
        }
    }

    // generate finally
    if(hasFinally) {
        this->curBuilder().finallyLabels.pop_back();

        this->markLabel(finallyLabel);
        this->catchException(beginLabel, finallyLabel, this->symbolTable.get(TYPE::Any),
                             blockNode.getBaseIndex(), maxLocalSize);
        this->visit(*node.getFinallyNode());
        this->emit0byteIns(OpCode::EXIT_FINALLY);
    }

    this->markLabel(mergeLabel);
}

void ByteCodeGenerator::visitVarDeclNode(VarDeclNode &node) {
    switch(node.getKind()) {
    case VarDeclNode::VAR:
    case VarDeclNode::CONST:
        this->visit(*node.getExprNode());
        break;
    case VarDeclNode::IMPORT_ENV: {
        this->emitLdcIns(DSValue::create<String_Object>(this->symbolTable.get(TYPE::String), node.getVarName()));
        this->emit0byteIns(OpCode::DUP);
        const bool hashDefault = node.getExprNode() != nullptr;
        if(hashDefault) {
            this->visit(*node.getExprNode());
        }

        this->emitSourcePos(node.getPos());
        this->emit1byteIns(OpCode::IMPORT_ENV, hashDefault ? 1 : 0);
        break;
    }
    case VarDeclNode::EXPORT_ENV: {
        this->emitLdcIns(DSValue::create<String_Object>(this->symbolTable.get(TYPE::String), node.getVarName()));
        this->emit0byteIns(OpCode::DUP);
        this->visit(*node.getExprNode());
        this->emit0byteIns(OpCode::STORE_ENV);
        break;
    }
    }

    if(node.isGlobal()) {
        this->emit2byteIns(OpCode::STORE_GLOBAL, node.getVarIndex());
    } else {
        this->emit1byteIns(OpCode::STORE_LOCAL, node.getVarIndex());
    }
}

void ByteCodeGenerator::visitAssignNode(AssignNode &node) {
    auto *assignableNode = static_cast<AssignableNode *>(node.getLeftNode());
    unsigned int index = assignableNode->getIndex();
    if(node.isFieldAssign()) {
        auto *accessNode = static_cast<AccessNode *>(node.getLeftNode());
        if(node.isSelfAssignment()) {
            this->visit(*node.getLeftNode());
        } else {
            this->visit(*accessNode->getRecvNode());
            if(accessNode->getRecvNode()->getType().isModType()) {
                this->emit0byteIns(OpCode::POP);
            }
        }
        this->visit(*node.getRightNode());

        if(assignableNode->attr().has(FieldAttribute::INTERFACE)) {
            std::string desc = encodeFieldDescriptor(
                    accessNode->getRecvNode()->getType(), accessNode->getFieldName().c_str(), accessNode->getType());
            this->emitDescriptorIns(OpCode::INVOKE_SETTER, std::move(desc));
        } else if(accessNode->getRecvNode()->getType().isModType()) {
            this->emit2byteIns(OpCode::STORE_GLOBAL, index);
        } else {
            this->emit2byteIns(OpCode::STORE_FIELD, index);
        }
    } else {
        if(node.isSelfAssignment()) {
            this->visit(*node.getLeftNode());
        }
        this->visit(*node.getRightNode());
        auto *varNode = static_cast<VarNode *>(node.getLeftNode());

        if(varNode->attr().has(FieldAttribute::ENV)) {
            if(varNode->attr().has(FieldAttribute::GLOBAL)) {
                this->emit2byteIns(OpCode::LOAD_GLOBAL, index);
            } else {
                this->emit1byteIns(OpCode::LOAD_LOCAL, index);
            }

            this->emit0byteIns(OpCode::SWAP);
            this->emit0byteIns(OpCode::STORE_ENV);
        } else if(varNode->attr().has(FieldAttribute::SECONDS)) {
            this->emit0byteIns(OpCode::SET_SECOND);
        } else {
            if(varNode->attr().has(FieldAttribute::GLOBAL)) {
                this->emit2byteIns(OpCode::STORE_GLOBAL, index);
            } else {
                this->emit1byteIns(OpCode::STORE_LOCAL, index);
            }
        }
    }
}

void ByteCodeGenerator::visitElementSelfAssignNode(ElementSelfAssignNode &node) {
    this->visit(*node.getRecvNode());
    this->visit(*node.getIndexNode());
    this->emit0byteIns(OpCode::DUP2);

    this->visit(*node.getGetterNode());
    this->visit(*node.getRightNode());

    this->visit(*node.getSetterNode());
}

void ByteCodeGenerator::visitFunctionNode(FunctionNode &node) {
    this->initCodeBuilder(CodeKind::FUNCTION, node.getMaxVarNum());
    this->visit(*node.getBlockNode());
    auto func = DSValue::create<FuncObject>(node.getFuncType(), this->finalizeCodeBuilder(node.getFuncName()));

    this->emitLdcIns(func);
    this->emit2byteIns(OpCode::STORE_GLOBAL, node.getVarIndex());
}

void ByteCodeGenerator::visitInterfaceNode(InterfaceNode &) { } // do nothing

void ByteCodeGenerator::visitUserDefinedCmdNode(UserDefinedCmdNode &node) {
    this->initCodeBuilder(CodeKind::USER_DEFINED_CMD, node.getMaxVarNum());
    this->visit(*node.getBlockNode());
    auto func = DSValue::create<FuncObject>(this->finalizeCodeBuilder(node.getCmdName()));

    this->emitLdcIns(func);
    this->emit2byteIns(OpCode::STORE_GLOBAL, node.getUdcIndex());
}

void ByteCodeGenerator::visitSourceNode(SourceNode &node) {
    unsigned int index = node.getIndex();
    if(node.isFirstAppear()) {
        this->emit0byteIns(OpCode::INIT_MODULE);
        if(index > 0) {
            this->emit0byteIns(OpCode::DUP);
        }
        this->emit2byteIns(OpCode::STORE_GLOBAL, node.getModIndex());
        if(index > 0) {
            this->emit2byteIns(OpCode::STORE_GLOBAL, index);
        }
    } else if(index > 0) {
        this->emit2byteIns(OpCode::LOAD_GLOBAL, node.getModIndex());
        this->emit2byteIns(OpCode::STORE_GLOBAL, index);
    }
}

void ByteCodeGenerator::visitEmptyNode(EmptyNode &) { } // do nothing

void ByteCodeGenerator::initCodeBuilder(CodeKind kind, const SourceInfoPtr &srcInfo,
                                        unsigned short localVarNum) {
    // push new builder
    this->builders.emplace_back(srcInfo);

    // generate header
    this->curBuilder().append8(static_cast<unsigned char>(kind));
    this->curBuilder().append32(0);
    this->curBuilder().append8(localVarNum);
    this->curBuilder().append16(0);
}

CompiledCode ByteCodeGenerator::finalizeCodeBuilder(const std::string &name) {
    this->curBuilder().finalize();

    // set max stack depth
    this->curBuilder().emit16(6, this->curBuilder().maxStackDepth);

    // extract code
    const unsigned int codeSize = this->curBuilder().codeBuffer.size();
    this->curBuilder().emit32(1, codeSize);
    unsigned char *code = extract(std::move(this->curBuilder().codeBuffer));

    // create constant pool
    const unsigned int constSize = this->curBuilder().constBuffer.size();
    auto *constPool = new DSValue[constSize + 1];
    for(unsigned int i = 0; i < constSize; i++) {
        constPool[i] = std::move(this->curBuilder().constBuffer[i]);
    }
    constPool[constSize] = nullptr; // sentinel

    // extract source pos entry
    this->curBuilder().sourcePosEntries.push_back({0, 0});
    auto *entries = extract(std::move(this->curBuilder().sourcePosEntries));

    // create exception entry
    const unsigned int exceptEntrySize = this->curBuilder().catchBuilders.size();
    auto *except = new ExceptionEntry[exceptEntrySize + 1];
    for(unsigned int i = 0; i < exceptEntrySize; i++) {
        except[i] = this->curBuilder().catchBuilders[i].toEntry();
    }
    except[exceptEntrySize] = {
            .type = nullptr,
            .begin = 0,
            .end = 0,
            .dest = 0,
            .localOffset = 0,
            .localSize = 0,
    };  // sentinel

    // remove current builder
    auto srcInfo = this->builders.back().srcInfo;
    this->builders.pop_back();

    return CompiledCode(srcInfo, name.empty() ? nullptr : name.c_str(),
                        code, constPool, entries, except);
}

void ByteCodeGenerator::exitModule(SourceNode &node) {
    this->curBuilder().emit8(5, node.getMaxVarNum());
    this->emitIns(OpCode::RETURN);
    auto func = DSValue::create<FuncObject>(node.getModType(), this->finalizeCodeBuilder(node.toModName()));
    this->emitLdcIns(func);
    this->visit(node);
}

static unsigned int digit(unsigned int n) {
    unsigned int c;
    if(n == 0) {
        return 1;
    }
    for(c = 0; n > 0; c++) {
        n /= 10;
    }
    return c;
}

static std::string formatNum(unsigned int width, unsigned int num) {
    std::string str;
    unsigned int numWidth = digit(num);
    for(unsigned int i = 0; i < width - numWidth; i++) {
        str += ' ';
    }
    str += std::to_string(num);
    return str;
}


static void dumpCodeImpl(FILE *fp, DSState &ctx, const CompiledCode &c,
                         std::vector<const CompiledCode *> *list) {
    const unsigned int codeSize = c.getCodeSize();

    fputs("DSCode: ", fp);
    switch(c.getKind()) {
    case CodeKind::TOPLEVEL:
        fputs("top level", fp);
        break;
    case CodeKind::FUNCTION:
        fprintf(fp, "function %s", c.getName());
        break;
    case CodeKind::USER_DEFINED_CMD:
        fprintf(fp, "command %s", c.getName());
        break;
    default:
        break;
    }
    fputc('\n', fp);
    fprintf(fp, "  code size: %d\n", c.getCodeSize());
    fprintf(fp, "  max stack depth: %d\n", c.getStackDepth());
    fprintf(fp, "  number of local variable: %d\n", c.getLocalVarNum());
    if(c.getKind() == CodeKind::TOPLEVEL) {
        fprintf(fp, "  number of global variable: %d\n", getPool(ctx).getMaxGVarIndex());
    }

#if 0
    stream << "Line Number Table:" << std::endl;
    {
        const unsigned int size = c.getSrcInfo()->getLineNumTable().size();
        for(unsigned int i = 0; i < size; i++) {
            stream << "  line " << std::setw(digit(size)) << (i + 1) <<
            ", pos " << c.getSrcInfo()->getLineNumTable()[i] << std::endl;
        }
    }
#endif
    fputs("Code:\n", fp);
    {
        const char *opName[] = {
#define GEN_NAME(CODE, N, S) #CODE,
                OPCODE_LIST(GEN_NAME)
#undef GEN_NAME
        };

        for(unsigned int i = c.getCodeOffset(); i < codeSize; i++) {
            auto code = static_cast<OpCode>(c.getCode()[i]);
            fprintf(fp, "  %s: %s", formatNum(digit(codeSize), i).c_str(), opName[static_cast<unsigned char>(code)]);
            if(isTypeOp(code)) {
                unsigned long v = read64(c.getCode(), i + 1);
                i += 8;
                fprintf(fp, "  %s", getPool(ctx).getTypeName(*reinterpret_cast<DSType *>(v)));
            } else {
                const int byteSize = getByteSize(code);
                if(code == OpCode::CALL_METHOD) {
                    fprintf(fp, "  %d  %d", read16(c.getCode(), i + 1), read16(c.getCode(), i + 3));
                } else {
                    switch(byteSize) {
                    case 1:
                        fprintf(fp, "  %d", static_cast<unsigned int>(read8(c.getCode(), i + 1)));
                        break;
                    case 2:
                        fprintf(fp, "  %d", read16(c.getCode(), i + 1));
                        break;
                    case 3:
                        fprintf(fp, "  %d", read24(c.getCode(), i + 1));
                        break;
                    case 4:
                        fprintf(fp, "  %d", read32(c.getCode(), i + 1));
                        break;
                    case -1: {
                        auto s = static_cast<unsigned int>(read8(c.getCode(), i + 1));
                        fprintf(fp, " %d", s);
                        for(unsigned int index = 0; index < s; index++) {
                            fprintf(fp, "  %d", read16(c.getCode(), i + 2 + index * 2));
                        }
                        break;
                    }
                    default:
                        break;  // do nothing
                    }
                }
                if(byteSize >= 0) {
                    i += byteSize;
                } else {
                    i += -1 * byteSize + 2 * read8(c.getCode(), i + 1);
                }
            }
            fputc('\n', fp);
        }
    }


    fputs("Constant Pool:\n", fp);
    {
        unsigned int constSize;
        for(constSize = 0; c.getConstPool()[constSize]; constSize++);
        for(unsigned int i = 0; c.getConstPool()[i]; i++) {
            fprintf(fp, "  %s: ", formatNum(digit(constSize), i).c_str());
            auto &v = c.getConstPool()[i];
            switch(v.kind()) {
            case DSValueKind::NUMBER:
                fprintf(fp, "%lu", static_cast<unsigned long>(v.value()));
                break;
            case DSValueKind::OBJECT:
                if(list != nullptr && (v->getType() == nullptr || v->getType()->isFuncType())) {
                    list->push_back(&static_cast<FuncObject *>(v.get())->getCode());
                }
                fprintf(fp, "%s %s",
                        (v->getType() != nullptr ? getPool(ctx).getTypeName(*v->getType()) : "(null)"),
                        v->toString(ctx, nullptr).c_str());
                break;
            case DSValueKind::INVALID:
                break;
            }
            fputc('\n', fp);
        }
    }


    fputs("Source Pos Entry:\n", fp);
    {
        auto &srcInfo = c.getSrcInfo();
        const unsigned int maxLineNum = srcInfo->getLineNumTable().size() + srcInfo->getLineNumOffset();
        for(unsigned int i = 0; c.getSourcePosEntries()[i].address != 0; i++) {
            const auto &e = c.getSourcePosEntries()[i];
            fprintf(fp, "  lineNum: %s, address: %s, pos: %d\n",
                    formatNum(digit(maxLineNum), srcInfo->getLineNum(e.pos)).c_str(),
                    formatNum(digit(codeSize), e.address).c_str(), e.pos);
        }
    }

    fputs("Exception Table:\n", fp);
    for(unsigned int i = 0; c.getExceptionEntries()[i].type != nullptr; i++) {
        const auto &e = c.getExceptionEntries()[i];
        fprintf(fp, "  begin: %d, end: %d, type: %s, dest: %d, offset: %d, size: %d\n",
                e.begin, e.end, getPool(ctx).getTypeName(*e.type), e.dest, e.localOffset, e.localSize);
    }

    fflush(fp);
}

void dumpCode(FILE *fp, DSState &ctx, const CompiledCode &c) {
    fprintf(fp, "Source File: %s\n", c.getSrcInfo()->getSourceName().c_str());

    std::vector<const CompiledCode *> list;

    dumpCodeImpl(fp, ctx, c, &list);
    for(auto &e : list) {
        fputc('\n', fp);
        dumpCodeImpl(fp, ctx, *e, nullptr);
    }
}

} // namespace ydsh
