/*
 * Copyright (C) 2021 Nagisa Sekiguchi
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

#include <core.h>
#include <frontend.h>
#include <misc/files.h>
#include <ydsh/ydsh.h>

#include "ast.h"

namespace ydsh::lsp {

// ########################
// ##     ASTContext     ##
// ########################

static void consumeAllInput(FrontEnd &frontEnd) {
  while (frontEnd) {
    if (!frontEnd(nullptr)) {
      break;
    }
  }
}

static IntrusivePtr<NameScope> createBuiltin(TypePool &pool, unsigned int &gvarCount) {
  auto builtin = IntrusivePtr<NameScope>::create(gvarCount);
  bindBuiltinVariables(nullptr, pool, *builtin);

  ModuleLoader loader; // dummy
  const char *embed = getEmbeddedScript();
  Lexer lexer("(builtin)", ByteBuffer(embed, embed + strlen(embed)), getCWD());
  FrontEnd frontEnd(loader, std::move(lexer), pool, builtin, DS_EXEC_MODE_CHECK_ONLY, false);
  consumeAllInput(frontEnd);
  return builtin;
}

ASTContext::ASTContext(unsigned int modID, Source &&source) : source(std::move(source)) {
  auto builtin = createBuiltin(this->pool, this->gvarCount);
  (void)builtin; // FIXME:
  this->scope = IntrusivePtr<NameScope>::create(std::ref(this->gvarCount), modID);
  this->scope->importForeignHandles(this->pool.getBuiltinModType(), true);
}

} // namespace ydsh::lsp