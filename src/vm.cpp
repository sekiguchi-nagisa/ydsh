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

#include <pwd.h>

#include <cerrno>
#include <cstdlib>
#include <random>

#include "logger.h"
#include "misc/files.h"
#include "misc/glob.hpp"
#include "misc/num_util.hpp"
#include "opcode.h"
#include "redir.h"
#include "vm.h"

// #####################
// ##     DSState     ##
// #####################

VMEvent DSState::eventDesc{};

SigSet DSState::pendingSigSet;

/**
 * if environmental variable SHLVL dose not exist, set 0.
 */
static unsigned int getShellLevel() {
  char *shlvl = getenv(ENV_SHLVL);
  unsigned int level = 0;
  if (shlvl != nullptr) {
    auto pair = convertToNum<int64_t>(shlvl);
    if (!pair.second) {
      level = 0;
    } else {
      level = pair.first;
    }
  }
  return level;
}

static unsigned int originalShellLevel() {
  static unsigned int level = getShellLevel();
  return level;
}

static void setPWDs() {
  auto v = getCWD();
  const char *cwd = v ? v.get() : ".";

  const char *pwd = getenv(ENV_PWD);
  if (strcmp(cwd, ".") == 0 || !pwd || *pwd != '/' || !isSameFile(pwd, cwd)) {
    setenv(ENV_PWD, cwd, 1);
    pwd = cwd;
  }

  const char *oldpwd = getenv(ENV_OLDPWD);
  if (!oldpwd || *oldpwd != '/' || !S_ISDIR(getStMode(oldpwd))) {
    setenv(ENV_OLDPWD, pwd, 1);
  }
}

static void initEnv() {
  // set locale
  setlocale(LC_ALL, "");
  setlocale(LC_MESSAGES, "C");

  // set environmental variables

  // update shell level
  setenv(ENV_SHLVL, std::to_string(originalShellLevel() + 1).c_str(), 1);

  // set HOME
  struct passwd *pw = getpwuid(getuid());
  if (pw == nullptr) {
    fatal_perror("getpwuid failed\n");
  }
  setenv(ENV_HOME, pw->pw_dir, 1);

  // set LOGNAME
  setenv(ENV_LOGNAME, pw->pw_name, 1);

  // set USER
  setenv(ENV_USER, pw->pw_name, 1);

  // set PWD/OLDPWD
  setPWDs();
}

DSState::DSState()
    : builtinModScope(this->modLoader.createGlobalScope("(builtin)")),
      emptyFDObj(toObjPtr<UnixFdObject>(DSValue::create<UnixFdObject>(-1))),
      baseTime(std::chrono::system_clock::now()) {
  // init envs
  initEnv();
  const char *pwd = getenv(ENV_PWD);
  assert(pwd);
  if (*pwd == '/') {
    this->logicalWorkingDir = expandDots(nullptr, pwd);
  }
}

void DSState::updatePipeStatus(unsigned int size, const Proc *procs, bool mergeExitStatus) const {
  auto &obj = typeAs<ArrayObject>(this->getGlobal(BuiltinVarOffset::PIPESTATUS));
  obj.refValues().clear();
  obj.refValues().reserve(size + (mergeExitStatus ? 1 : 0));

  for (unsigned int i = 0; i < size; i++) {
    obj.refValues().push_back(DSValue::createInt(procs[i].exitStatus()));
  }
  if (mergeExitStatus) {
    obj.refValues().push_back(this->getGlobal(BuiltinVarOffset::EXIT_STATUS));
  }
}

namespace ydsh {

bool VM::checkCast(DSState &state, const DSType &targetType) {
  if (!instanceOf(state.typePool, state.stack.peek(), targetType)) {
    auto &stackTopType = state.typePool.get(state.stack.pop().getTypeID());
    std::string str("cannot cast `");
    str += stackTopType.getNameRef();
    str += "' to `";
    str += targetType.getNameRef();
    str += "'";
    raiseError(state, TYPE::TypeCastError, std::move(str));
    return false;
  }
  return true;
}

bool VM::checkAssertion(DSState &state) {
  auto msg = state.stack.pop();
  auto ref = msg.asStrRef();
  if (!state.stack.pop().asBool()) {
    raiseError(state, TYPE::_AssertFail, ref.toString());
    return false;
  }
  return true;
}

const char *VM::loadEnv(DSState &state, bool hasDefault) {
  DSValue dValue;
  if (hasDefault) {
    dValue = state.stack.pop();
  }
  auto nameObj = state.stack.pop();
  auto nameRef = nameObj.asStrRef();
  assert(!nameRef.hasNullChar());
  const char *name = nameRef.data();
  const char *env = getenv(name);
  if (env == nullptr && hasDefault) {
    auto ref = dValue.asStrRef();
    if (ref.hasNullChar()) {
      std::string message = SET_ENV_ERROR;
      message += name;
      raiseError(state, TYPE::IllegalAccessError, std::move(message));
      return nullptr;
    }
    setenv(name, ref.data(), 1);
    env = getenv(name);
  }

  if (env == nullptr) {
    std::string message = UNDEF_ENV_ERROR;
    message += name;
    raiseError(state, TYPE::IllegalAccessError, std::move(message));
  }
  return env;
}

bool VM::storeEnv(DSState &state) {
  auto value = state.stack.pop();
  auto name = state.stack.pop();
  auto nameRef = name.asStrRef();
  auto valueRef = value.asStrRef();
  assert(!nameRef.hasNullChar());
  if (setenv(valueRef.hasNullChar() ? "" : nameRef.data(), valueRef.data(), 1) == 0) {
    return true;
  }
  std::string str = SET_ENV_ERROR;
  str += nameRef;
  raiseError(state, TYPE::IllegalAccessError, std::move(str));
  return false;
}

void VM::pushNewObject(DSState &state, const DSType &type) {
  DSValue value;
  if (state.typePool.isArrayType(type)) {
    value = DSValue::create<ArrayObject>(type);
  } else if (state.typePool.isMapType(type)) {
    value = DSValue::create<MapObject>(type);
  } else if (state.typePool.isTupleType(type)) {
    value = DSValue::create<BaseObject>(type);
  } else if (!type.isRecordType()) {
    value = DSValue::createDummy(type);
  } else {
    fatal("currently, DSObject allocation not supported\n");
  }
  state.stack.push(std::move(value));
}

bool VM::prepareUserDefinedCommandCall(DSState &state, const DSCode &code, DSValue &&argvObj,
                                       DSValue &&restoreFD, const CmdCallAttr attr) {
  if (hasFlag(attr, CmdCallAttr::SET_VAR)) {
    // reset exit status
    state.setExitStatus(0);
  }

  // set parameter
  state.stack.reserve(3);
  state.stack.push(DSValue::createNum(static_cast<unsigned int>(attr)));
  state.stack.push(std::move(restoreFD));
  state.stack.push(std::move(argvObj));

  if (!windStackFrame(state, 3, 3, code)) {
    return false;
  }

  if (hasFlag(attr, CmdCallAttr::SET_VAR)) { // set variable
    auto &argv = typeAs<ArrayObject>(state.stack.getLocal(UDC_PARAM_ARGV));
    auto cmdName = argv.takeFirst();
    const unsigned int argSize = argv.getValues().size();
    state.stack.setLocal(UDC_PARAM_ARGV + 1, DSValue::createInt(argSize)); // #
    state.stack.setLocal(UDC_PARAM_ARGV + 2, std::move(cmdName));          // 0
    unsigned int limit = 9;
    if (argSize < limit) {
      limit = argSize;
    }

    unsigned int index = 0;
    for (; index < limit; index++) {
      state.stack.setLocal(index + UDC_PARAM_ARGV + 3, argv.getValues()[index]);
    }

    for (; index < 9; index++) {
      state.stack.setLocal(index + UDC_PARAM_ARGV + 3, DSValue::createStr()); // set remain
    }
  }
  return true;
}

#define CODE(ctx) ((ctx).stack.code())
#define GET_CODE(ctx) (CODE(ctx)->getCode())
#define CONST_POOL(ctx) (static_cast<const CompiledCode *>(CODE(ctx))->getConstPool())

/* for substitution */

static DSValue readAsStr(int fd) {
  std::string str;
  while (true) {
    char buf[256];
    ssize_t readSize = read(fd, buf, std::size(buf));
    if (readSize == -1 && (errno == EAGAIN || errno == EINTR)) {
      continue;
    }
    if (readSize <= 0) {
      break;
    }
    str.append(buf, readSize);
  }

  // remove last newlines
  for (; !str.empty() && str.back() == '\n'; str.pop_back())
    ;

  return DSValue::createStr(std::move(str));
}

static DSValue readAsStrArray(const DSState &state, int fd) {
  auto ifsRef = state.getGlobal(BuiltinVarOffset::IFS).asStrRef();
  unsigned int skipCount = 1;
  std::string str;
  auto obj = DSValue::create<ArrayObject>(state.typePool.get(TYPE::StringArray));
  auto &array = typeAs<ArrayObject>(obj);

  while (true) {
    char buf[256];
    ssize_t readSize = read(fd, buf, std::size(buf));
    if (readSize == -1 && (errno == EINTR || errno == EAGAIN)) {
      continue;
    }
    if (readSize <= 0) {
      break;
    }

    for (ssize_t i = 0; i < readSize; i++) {
      char ch = buf[i];
      bool fieldSep = matchFieldSep(ifsRef, ch);
      if (fieldSep && skipCount > 0) {
        if (isSpace(ch)) {
          continue;
        }
        if (--skipCount == 1) {
          continue;
        }
      }
      skipCount = 0;
      if (fieldSep) {
        array.append(DSValue::createStr(std::move(str)));
        str = "";
        skipCount = isSpace(ch) ? 2 : 1;
        continue;
      }
      str += ch;
    }
  }

  // remove last newline
  for (; !str.empty() && str.back() == '\n'; str.pop_back())
    ;

  // append remain
  if (!str.empty() || !hasSpace(ifsRef)) {
    array.append(DSValue::createStr(std::move(str)));
  }

  return obj;
}

static ObjPtr<UnixFdObject> newFD(const DSState &st, int &fd) {
  if (fd < 0) {
    return st.emptyFDObj;
  }
  int v = fd;
  fd = -1;
  auto value = DSValue::create<UnixFdObject>(v);
  typeAs<UnixFdObject>(value).closeOnExec(true);
  return toObjPtr<UnixFdObject>(value);
}

bool VM::attachAsyncJob(DSState &state, unsigned int procSize, const Proc *procs, ForkKind forkKind,
                        PipeSet &pipeSet, DSValue &ret) {
  switch (forkKind) {
  case ForkKind::NONE: {
    auto entry = JobObject::create(procSize, procs, false, state.emptyFDObj, state.emptyFDObj);
    // job termination
    auto waitOp =
        state.isRootShell() && state.isJobControl() ? WaitOp::BLOCK_UNTRACED : WaitOp::BLOCKING;
    int status = entry->wait(waitOp);
    int errNum = errno;
    state.updatePipeStatus(entry->getProcSize(), entry->getProcs(), false);
    if (entry->available()) {
      state.jobTable.attach(entry);
    }
    state.tryToBeForeground();
    state.jobTable.waitForAny();
    state.setExitStatus(status);
    if (errNum != 0) {
      raiseSystemError(state, errNum, "wait failed");
      return false;
    }
    ret = exitStatusToBool(status);
    break;
  }
  case ForkKind::IN_PIPE:
  case ForkKind::OUT_PIPE: {
    int &fd = forkKind == ForkKind::IN_PIPE ? pipeSet.in[WRITE_PIPE] : pipeSet.out[READ_PIPE];
    auto fdObj = newFD(state, fd);
    auto entry = JobObject::create(procSize, procs, false,
                                   forkKind == ForkKind::IN_PIPE ? fdObj : state.emptyFDObj,
                                   forkKind == ForkKind::OUT_PIPE ? fdObj : state.emptyFDObj);
    state.jobTable.attach(entry, true);
    ret = fdObj;
    break;
  }
  case ForkKind::COPROC:
  case ForkKind::JOB:
  case ForkKind::DISOWN: {
    bool disown = forkKind == ForkKind::DISOWN;
    auto entry = JobObject::create(procSize, procs, false, newFD(state, pipeSet.in[WRITE_PIPE]),
                                   newFD(state, pipeSet.out[READ_PIPE]));
    state.jobTable.attach(entry, disown);
    ret = DSValue(entry.get());
    break;
  }
  default:
    break;
  }
  return true;
}

bool VM::forkAndEval(DSState &state) {
  const auto forkKind = static_cast<ForkKind>(read8(GET_CODE(state), state.stack.pc()));
  const unsigned short offset = read16(GET_CODE(state), state.stack.pc() + 1);

  // flush standard stream due to prevent mixing io buffer
  flushStdFD();

  // set in/out pipe
  PipeSet pipeset(forkKind);
  const bool rootShell = state.isRootShell();
  pid_t pgid = rootShell ? 0 : getpgid(0);
  auto proc = Proc::fork(state, pgid, needForeground(forkKind) && rootShell);
  if (proc.pid() > 0) { // parent process
    tryToClose(pipeset.in[READ_PIPE]);
    tryToClose(pipeset.out[WRITE_PIPE]);

    DSValue obj;

    switch (forkKind) {
    case ForkKind::STR:
    case ForkKind::ARRAY: {
      tryToClose(pipeset.in[WRITE_PIPE]);
      const bool isStr = forkKind == ForkKind::STR;
      obj =
          isStr ? readAsStr(pipeset.out[READ_PIPE]) : readAsStrArray(state, pipeset.out[READ_PIPE]);
      auto waitOp =
          state.isRootShell() && state.isJobControl() ? WaitOp::BLOCK_UNTRACED : WaitOp::BLOCKING;
      int ret = proc.wait(waitOp); // wait exit
      int errNum = errno;
      tryToClose(pipeset.out[READ_PIPE]); // close read pipe after wait, due to prevent EPIPE
      if (!proc.is(Proc::State::TERMINATED)) {
        state.jobTable.attach(JobObject::create(proc, state.emptyFDObj, state.emptyFDObj));
      }
      state.setExitStatus(ret);
      state.tryToBeForeground();
      if (ret < 0) {
        raiseSystemError(state, errNum, "wait failed");
        return false;
      }
      break;
    }
    default:
      Proc procs[1] = {proc};
      if (!attachAsyncJob(state, 1, procs, forkKind, pipeset, obj)) {
        return false;
      }
    }

    // push object
    if (obj) {
      state.stack.push(std::move(obj));
    }

    state.stack.pc() += offset - 1;
  } else if (proc.pid() == 0) { // child process
    pipeset.setupChildStdin(forkKind, state.isJobControl());
    pipeset.setupChildStdout();
    pipeset.closeAll();

    state.stack.pc() += 3;
  } else {
    raiseSystemError(state, EAGAIN, "fork failed");
    return false;
  }
  return true;
}

/* for pipeline evaluation */
static NativeCode initCode(OpCode op) {
  NativeCode::ArrayType code;
  code[0] = static_cast<char>(op);
  code[1] = static_cast<char>(OpCode::RETURN_V);
  return NativeCode(code);
}

static const FieldHandle *lookupUdcImpl(const NameScope &scope, const TypePool &pool,
                                        const ModType *belongType, const char *cmdName) {
  std::string name = toCmdFullName(cmdName);
  if (belongType == nullptr) {
    return scope.lookup(name);
  } else {
    return belongType->lookupVisibleSymbolAtModule(pool, name);
  }
}

/**
 * lookup user-defined command from module.
 * if modType is null, lookup from root module
 * @param state
 * @param name
 * @param cmd
 * @param modType
 * may be null
 * @return
 */
static bool lookupUdc(const DSState &state, const char *name, ResolvedCmd &cmd,
                      const ModType *modType) {
  auto handle = lookupUdcImpl(*state.rootModScope, state.typePool, modType, name);
  const FuncObject *udcObj = nullptr;
  if (handle) {
    auto &v = state.getGlobal(handle->getIndex());
    if (v) {
      udcObj = &typeAs<FuncObject>(v);
    } else {
      cmd = ResolvedCmd::illegalUdc();
      return true;
    }
  }

  if (udcObj) {
    auto &type = state.typePool.get(udcObj->getTypeID());
    if (type.isModType()) {
      cmd = ResolvedCmd::fromMod(static_cast<const ModType &>(type), modType);
    } else {
      assert(type.isVoidType());
      modType = getUnderlyingModType(state.typePool, state.modLoader, &udcObj->getCode());
      cmd = ResolvedCmd::fromUdc(*udcObj, modType);
    }
    return true;
  }
  return false;
}

ResolvedCmd CmdResolver::operator()(DSState &state, StringRef ref, const ModType *modType) const {
  // first, check user-defined command
  if (hasFlag(this->resolveOp, FROM_UDC)) {
    auto fqn = hasFlag(this->resolveOp, USE_FQN) ? ref.find('\0') : StringRef::npos;
    const char *cmdName = ref.data();
    if (fqn != StringRef::npos) {
      modType = nullptr;
      if (cmdName[0] != '\0') {
        auto ret = state.typePool.getType(cmdName);
        if (!ret || !ret.asOk()->isModType() || ref.find('\0', fqn + 1) != StringRef::npos) {
          return ResolvedCmd::invalid();
        }
        modType = static_cast<const ModType *>(ret.asOk());
      }
      cmdName = ref.begin() + fqn + 1;
    } else if (!modType) {
      modType = getCurRuntimeModule(state);
    }
    ResolvedCmd cmd{};
    if (lookupUdc(state, cmdName, cmd, modType)) {
      return cmd;
    } else if (fqn != StringRef::npos) {
      return ResolvedCmd::invalid();
    }
  }

  // second, check builtin command
  const char *cmdName = ref.data();
  if (hasFlag(this->resolveOp, FROM_BUILTIN)) {
    builtin_command_t bcmd = lookupBuiltinCommand(cmdName);
    if (bcmd != nullptr) {
      return ResolvedCmd::fromBuiltin(bcmd);
    }

    static std::pair<const char *, NativeCode> sb[] = {
        {"command", initCode(OpCode::BUILTIN_CMD)},
        {"eval", initCode(OpCode::BUILTIN_EVAL)},
        {"exec", initCode(OpCode::BUILTIN_EXEC)},
    };
    for (auto &e : sb) {
      if (strcmp(cmdName, e.first) == 0) {
        return ResolvedCmd::fromBuiltin(e.second);
      }
    }
  }

  // resolve external command path
  auto cmd = ResolvedCmd::fromExternal(nullptr);
  if (hasFlag(this->resolveOp, FROM_EXTERNAL)) {
    cmd = ResolvedCmd::fromExternal(state.pathCache.searchPath(cmdName, this->searchOp));

    // if command not found or directory, lookup _cmd_fallback_handler
    if (hasFlag(this->resolveOp, FROM_FALLBACK) &&
        (cmd.filePath() == nullptr || S_ISDIR(getStMode(cmd.filePath())))) {
      if (getBuiltinGlobal(state, VAR_CMD_FALLBACK).isObject()) {
        bool r = lookupUdc(state, CMD_FALLBACK_HANDLER, cmd, &state.typePool.getBuiltinModType());
        (void)r;
        assert(r);
      }
    }
  }
  return cmd;
}

static void raiseCmdError(DSState &state, const char *cmdName, int errnum) {
  std::string str = EXEC_ERROR;
  str += cmdName;
  if (errnum == ENOENT) {
    str += ": command not found";
    raiseError(state, TYPE::SystemError, std::move(str));
  } else {
    raiseSystemError(state, errnum, std::move(str));
  }
}

static void raiseInvalidCmdError(DSState &state, StringRef ref) {
  std::string message = "command contains null character: `";
  for (char ch : ref) {
    if (ch == '\0') {
      message += "\\x00";
    } else {
      message += ch;
    }
  }
  message += "'";
  raiseSystemError(state, EINVAL, std::move(message));
}

int VM::forkAndExec(DSState &state, const char *filePath, char *const *argv,
                    DSValue &&redirConfig) {
  // setup self pipe
  int selfpipe[2];
  if (pipe(selfpipe) < 0) {
    fatal_perror("pipe creation error");
  }
  if (!setCloseOnExec(selfpipe[WRITE_PIPE], true)) {
    fatal_perror("fcntl error");
  }

  bool rootShell = state.isRootShell();
  pid_t pgid = rootShell ? 0 : getpgid(0);
  auto proc = Proc::fork(state, pgid, rootShell);
  if (proc.pid() == -1) {
    raiseCmdError(state, argv[0], EAGAIN);
    return 1;
  } else if (proc.pid() == 0) { // child
    close(selfpipe[READ_PIPE]);
    xexecve(filePath, argv, nullptr);

    int errnum = errno;
    ssize_t r = write(selfpipe[WRITE_PIPE], &errnum, sizeof(int));
    (void)r; // FIXME:
    exit(-1);
  } else { // parent process
    close(selfpipe[WRITE_PIPE]);
    redirConfig = nullptr; // restore redirconfig

    ssize_t readSize;
    int errnum = 0;
    while ((readSize = read(selfpipe[READ_PIPE], &errnum, sizeof(int))) == -1) {
      if (errno != EAGAIN && errno != EINTR) {
        break;
      }
    }
    close(selfpipe[READ_PIPE]);
    if (readSize > 0 && errnum == ENOENT) { // remove cached path
      state.pathCache.removePath(argv[0]);
    }

    // wait process or job termination
    int status;
    auto waitOp = rootShell && state.isJobControl() ? WaitOp::BLOCK_UNTRACED : WaitOp::BLOCKING;
    status = proc.wait(waitOp);
    int errNum2 = errno;
    if (!proc.is(Proc::State::TERMINATED)) {
      state.jobTable.attach(JobObject::create(proc, state.emptyFDObj, state.emptyFDObj));
    }
    int ret = state.tryToBeForeground();
    LOG(DUMP_EXEC, "tryToBeForeground: %d, %s", ret, strerror(errno));
    state.jobTable.waitForAny();
    if (errnum != 0) {
      errNum2 = errnum;
    }
    if (errNum2 != 0) {
      raiseCmdError(state, argv[0], errNum2);
      return 1;
    }
    return status;
  }
}

bool VM::prepareSubCommand(DSState &state, const ModType &modType, DSValue &&argvObj,
                           DSValue &&restoreFD) {
  auto &array = typeAs<ArrayObject>(argvObj);
  if (array.size() == 1) {
    ERROR(array, "require subcommand");
    pushExitStatus(state, 2);
    return true;
  }

  auto subcmd = array.getValues()[1].asStrRef();
  if (subcmd[0] == '_') {
    ERROR(array, "cannot resolve private subcommand: %s", toPrintable(subcmd).c_str());
    pushExitStatus(state, 1);
    return true;
  }

  std::string key = toCmdFullName(subcmd);
  auto *handle = modType.lookupField(key);
  if (!handle) {
    ERROR(array, "undefined subcommand: %s", toPrintable(subcmd).c_str());
    pushExitStatus(state, 2);
    return true;
  }
  auto &udc = typeAs<FuncObject>(state.getGlobal(handle->getIndex())).getCode();
  array.takeFirst();
  return prepareUserDefinedCommandCall(state, udc, std::move(argvObj), std::move(restoreFD),
                                       CmdCallAttr::SET_VAR);
}

bool VM::callCommand(DSState &state, CmdResolver resolver, DSValue &&argvObj, DSValue &&redirConfig,
                     CmdCallAttr attr) {
  auto &array = typeAs<ArrayObject>(argvObj);
  auto cmd = resolver(state, array.getValues()[0].asStrRef());

  switch (cmd.kind()) {
  case ResolvedCmd::USER_DEFINED:
  case ResolvedCmd::BUILTIN_S: {
    if (cmd.kind() == ResolvedCmd::USER_DEFINED) {
      setFlag(attr, CmdCallAttr::SET_VAR);
    }
    return prepareUserDefinedCommandCall(state, cmd.udc(), std::move(argvObj),
                                         std::move(redirConfig), attr);
  }
  case ResolvedCmd::BUILTIN: {
    int status = cmd.builtinCmd()(state, array);
    flushStdFD();
    if (state.hasError()) {
      return false;
    }
    pushExitStatus(state, status);
    return true;
  }
  case ResolvedCmd::MODULE:
    return prepareSubCommand(state, cmd.modType(), std::move(argvObj), std::move(redirConfig));
  case ResolvedCmd::EXTERNAL: {
    // create argv
    const unsigned int size = array.getValues().size();
    char *argv[size + 1];
    for (unsigned int i = 0; i < size; i++) {
      argv[i] = const_cast<char *>(array.getValues()[i].asCStr());
    }
    argv[size] = nullptr;

    if (hasFlag(attr, CmdCallAttr::NEED_FORK)) {
      int status = forkAndExec(state, cmd.filePath(), argv, std::move(redirConfig));
      pushExitStatus(state, status);
    } else {
      xexecve(cmd.filePath(), argv, nullptr);
      raiseCmdError(state, argv[0], errno);
    }
    return !state.hasError();
  }
  case ResolvedCmd::INVALID:
    raiseInvalidCmdError(state, array.getValues()[0].asStrRef());
    return false;
  case ResolvedCmd::ILLEGAL_UDC: {
    std::string value = "attemp to access uninitialized user-defined command: `";
    value += array.getValues()[0].asStrRef();
    value += "'";
    raiseError(state, TYPE::IllegalAccessError, std::move(value));
    return false;
  }
  }
  return true; // normally unreachable, but need to suppress gcc warning.
}

int invalidOptionError(const ArrayObject &obj, const GetOptState &s);

bool VM::builtinCommand(DSState &state, DSValue &&argvObj, DSValue &&redir, CmdCallAttr attr) {
  auto &arrayObj = typeAs<ArrayObject>(argvObj);

  bool useDefaultPath = false;

  /**
   * if 0, ignore
   * if 1, show description
   * if 2, show detailed description
   */
  unsigned char showDesc = 0;

  GetOptState optState;
  for (int opt; (opt = optState(arrayObj, "pvV")) != -1;) {
    switch (opt) {
    case 'p':
      useDefaultPath = true;
      break;
    case 'v':
      showDesc = 1;
      break;
    case 'V':
      showDesc = 2;
      break;
    default:
      int s = invalidOptionError(arrayObj, optState);
      pushExitStatus(state, s);
      return true;
    }
  }

  unsigned int index = optState.index;
  const unsigned int argc = arrayObj.getValues().size();
  if (index < argc) {
    if (showDesc == 0) { // execute command
      if (arrayObj.getValues()[1].asStrRef().hasNullChar()) {
        auto name = toPrintable(arrayObj.getValues()[1].asStrRef());
        ERROR(arrayObj, "contains null characters: %s", name.c_str());
        pushExitStatus(state, 1);
        return true;
      }

      auto &values = arrayObj.refValues();
      values.erase(values.begin(), values.begin() + index);

      auto resolve = CmdResolver(
          CmdResolver::FROM_BUILTIN | CmdResolver::FROM_EXTERNAL | CmdResolver::FROM_FALLBACK,
          useDefaultPath ? FilePathCache::USE_DEFAULT_PATH : FilePathCache::NON);
      return callCommand(state, resolve, std::move(argvObj), std::move(redir), attr);
    }

    // show command description
    unsigned int successCount = 0;
    for (; index < argc; index++) {
      auto ref = arrayObj.getValues()[index].asStrRef();
      auto cmd = CmdResolver(CmdResolver::NO_FALLBACK | CmdResolver::USE_FQN,
                             FilePathCache::DIRECT_SEARCH)(state, ref);
      switch (cmd.kind()) {
      case ResolvedCmd::USER_DEFINED:
      case ResolvedCmd::MODULE: {
        successCount++;
        fputs(toPrintable(ref).c_str(), stdout);
        if (showDesc == 2) {
          fputs(" is an user-defined command", stdout);
        }
        fputc('\n', stdout);
        continue;
      }
      case ResolvedCmd::BUILTIN_S:
      case ResolvedCmd::BUILTIN: {
        successCount++;
        fputs(ref.data(), stdout);
        if (showDesc == 2) {
          fputs(" is a shell builtin command", stdout);
        }
        fputc('\n', stdout);
        continue;
      }
      case ResolvedCmd::EXTERNAL: {
        const char *path = cmd.filePath();
        if (path != nullptr && isExecutable(path)) {
          successCount++;
          const char *commandName = ref.data();
          if (showDesc == 1) {
            printf("%s\n", path);
          } else if (state.pathCache.isCached(commandName)) {
            printf("%s is hashed (%s)\n", commandName, path);
          } else {
            printf("%s is %s\n", commandName, path);
          }
          continue;
        }
        break;
      }
      case ResolvedCmd::INVALID:
      case ResolvedCmd::ILLEGAL_UDC:
        break;
      }

      if (showDesc == 2) {
        if (cmd.kind() == ResolvedCmd::ILLEGAL_UDC) {
          ERROR(arrayObj, "%s: uninitialized", toPrintable(ref).c_str());
        } else {
          ERROR(arrayObj, "%s: not found", toPrintable(ref).c_str());
        }
      }
    }
    pushExitStatus(state, successCount > 0 ? 0 : 1);
    return true;
  }
  pushExitStatus(state, 0);
  return true;
}

void VM::builtinExec(DSState &state, DSValue &&array, DSValue &&redir) {
  auto &argvObj = typeAs<ArrayObject>(array);
  bool clearEnv = false;
  StringRef progName;
  GetOptState optState;

  if (redir) {
    typeAs<RedirObject>(redir).ignoreBackup();
  }

  for (int opt; (opt = optState(argvObj, "ca:")) != -1;) {
    switch (opt) {
    case 'c':
      clearEnv = true;
      break;
    case 'a':
      progName = optState.optArg;
      break;
    default:
      int s = invalidOptionError(argvObj, optState);
      pushExitStatus(state, s);
      return;
    }
  }

  unsigned int index = optState.index;
  const unsigned int argc = argvObj.getValues().size();
  if (index < argc) { // exec
    if (argvObj.getValues()[index].asStrRef().hasNullChar()) {
      auto name = toPrintable(argvObj.getValues()[index].asStrRef());
      ERROR(argvObj, "contains null characters: %s", name.c_str());
      pushExitStatus(state, 1);
      return;
    }

    char *argv2[argc - index + 1];
    for (unsigned int i = index; i < argc; i++) {
      argv2[i - index] = const_cast<char *>(argvObj.getValues()[i].asCStr());
    }
    argv2[argc - index] = nullptr;

    const char *filePath = state.pathCache.searchPath(argv2[0], FilePathCache::DIRECT_SEARCH);
    if (progName.data() != nullptr) {
      argv2[0] = const_cast<char *>(progName.data());
    }

    char *envp[] = {nullptr};
    xexecve(filePath, argv2, clearEnv ? envp : nullptr);
    PERROR(argvObj, "%s", argvObj.getValues()[index].asCStr());
    exit(1);
  }
  pushExitStatus(state, 0);
}

bool VM::callPipeline(DSState &state, bool lastPipe, ForkKind forkKind) {
  /**
   * ls | grep .
   * ==> pipeSize == 1, procSize == 2
   *
   * if lastPipe is true,
   *
   * ls | { grep . ;}
   * ==> pipeSize == 1, procSize == 1
   */
  const unsigned int procSize = read8(GET_CODE(state), state.stack.pc()) - 1;
  const unsigned int pipeSize = procSize - (lastPipe ? 0 : 1);

  assert(pipeSize > 0);

  PipeSet pipeset(forkKind);
  int pipefds[pipeSize][2];
  initAllPipe(pipeSize, pipefds);

  // fork
  Proc childs[procSize];
  const bool rootShell = state.isRootShell();
  pid_t pgid = rootShell ? 0 : getpgid(0);
  Proc proc; // NOLINT

  unsigned int procIndex;
  for (procIndex = 0; procIndex < procSize && (proc = Proc::fork(state, pgid, rootShell)).pid() > 0;
       procIndex++) {
    childs[procIndex] = proc;
    if (pgid == 0) {
      pgid = proc.pid();
    }
  }

  /**
   * code layout
   *
   * +----------+-------------+------------------+    +--------------------+
   * | PIPELINE | size: 1byte | br1(child): 2yte | ~  | brN(parent): 2byte |
   * +----------+-------------+------------------+    +--------------------+
   */
  if (proc.pid() == 0) {  // child
    if (procIndex == 0) { // first process
      ::dup2(pipefds[procIndex][WRITE_PIPE], STDOUT_FILENO);
      pipeset.setupChildStdin(forkKind, state.isJobControl());
    }
    if (procIndex > 0 && procIndex < pipeSize) { // other process.
      ::dup2(pipefds[procIndex - 1][READ_PIPE], STDIN_FILENO);
      ::dup2(pipefds[procIndex][WRITE_PIPE], STDOUT_FILENO);
    }
    if (procIndex == pipeSize && !lastPipe) { // last process
      ::dup2(pipefds[procIndex - 1][READ_PIPE], STDIN_FILENO);
      pipeset.setupChildStdout();
    }
    pipeset.closeAll();
    closeAllPipe(pipeSize, pipefds);

    // set pc to next instruction
    state.stack.pc() += read16(GET_CODE(state), state.stack.pc() + 1 + procIndex * 2) - 1;
  } else if (procIndex == procSize) { // parent (last pipeline)
    if (lastPipe) {
      /**
       * in last pipe, save current stdin before call dup2
       */
      auto jobEntry = JobObject::create(procSize, childs, true, state.emptyFDObj, state.emptyFDObj);
      state.jobTable.attach(jobEntry);
      ::dup2(pipefds[procIndex - 1][READ_PIPE], STDIN_FILENO);
      closeAllPipe(pipeSize, pipefds);
      state.stack.push(DSValue::create<PipelineObject>(state, std::move(jobEntry)));
    } else {
      tryToClose(pipeset.in[READ_PIPE]);
      tryToClose(pipeset.out[WRITE_PIPE]);
      closeAllPipe(pipeSize, pipefds);
      DSValue obj;
      if (!attachAsyncJob(state, procSize, childs, forkKind, pipeset, obj)) {
        return false;
      }
      if (obj) {
        state.stack.push(std::move(obj));
      }
    }

    // set pc to next instruction
    state.stack.pc() += read16(GET_CODE(state), state.stack.pc() + 1 + procIndex * 2) - 1;
  } else {
    // force terminate forked process.
    for (unsigned int i = 0; i < procIndex; i++) {
      childs[i].send(SIGKILL);
    }

    raiseSystemError(state, EAGAIN, "fork failed");
    return false;
  }
  return true;
}

void VM::addCmdArg(DSState &state, bool skipEmptyStr) {
  /**
   * stack layout
   *
   * ===========> stack grow
   * +------+-------+-------+
   * | argv | redir | value |
   * +------+-------+-------+
   */
  DSValue value = state.stack.pop();
  auto &valueType = state.typePool.get(value.getTypeID());

  auto &argv = typeAs<ArrayObject>(state.stack.peekByOffset(1));
  if (valueType.is(TYPE::String)) { // String
    if (skipEmptyStr && value.asStrRef().empty()) {
      return;
    }
    argv.append(std::move(value));
    return;
  }

  if (valueType.is(TYPE::UnixFD)) { // UnixFD
    if (!state.stack.peek()) {
      state.stack.pop();
      state.stack.push(DSValue::create<RedirObject>());
    }
    auto strObj = DSValue::createStr(value.toString());
    typeAs<UnixFdObject>(value).closeOnExec(false);
    typeAs<RedirObject>(state.stack.peek()).addRedirOp(RedirOP::NOP, std::move(value));
    argv.append(std::move(strObj));
    return;
  }

  assert(valueType.is(TYPE::StringArray)); // Array<String>
  auto &arrayObj = typeAs<ArrayObject>(value);
  for (auto &element : arrayObj.getValues()) {
    if (element.asStrRef().empty()) {
      continue;
    }
    argv.append(element);
  }
}

class GlobIter {
private:
  const DSValue *cur;
  const char *ptr{nullptr};

public:
  explicit GlobIter(const DSValue &value) : cur(&value) {
    if (this->cur->hasStrRef()) {
      this->ptr = this->cur->asStrRef().begin();
    }
  }

  char operator*() const { return this->ptr == nullptr ? '\0' : *this->ptr; }

  bool operator==(const GlobIter &other) const {
    return this->cur == other.cur && this->ptr == other.ptr;
  }

  bool operator!=(const GlobIter &other) const { return !(*this == other); }

  GlobIter &operator++() {
    if (this->ptr) {
      this->ptr++;
      if (*this->ptr == '\0') { // if StringRef iterator reaches null, increment DSValue iterator
        this->ptr = nullptr;
      }
    }
    if (!this->ptr) {
      this->cur++;
      if (this->cur->hasStrRef()) {
        this->ptr = this->cur->asStrRef().begin();
      }
    }
    return *this;
  }

  const DSValue *getIter() const { return this->cur; }
};

struct DSValueGlobMeta {
  static bool isAny(GlobIter iter) {
    auto &v = *iter.getIter();
    return v.kind() == DSValueKind::GLOB_META && v.asGlobMeta() == GlobMeta::ANY;
  }

  static bool isZeroOrMore(GlobIter iter) {
    auto &v = *iter.getIter();
    return v.kind() == DSValueKind::GLOB_META && v.asGlobMeta() == GlobMeta::ZERO_OR_MORE;
  }

  static void preExpand(std::string &path) { expandTilde(path, true); }
};

static void raiseGlobbingError(DSState &state, const VMState &st, unsigned int size,
                               const char *message) {
  std::string value = message;
  value += " `";
  for (unsigned int i = 0; i < size; i++) {
    auto &v = st.peekByOffset(size - i);
    if (v.hasStrRef()) {
      for (auto &e : v.asStrRef()) {
        if (e != '\0') {
          value += e;
        } else {
          value += "\\x00";
        }
      }
    } else {
      value += v.toString();
    }
  }
  value += "'";
  raiseError(state, TYPE::GlobbingError, std::move(value));
}

bool VM::addGlobbingPath(DSState &state, const unsigned int size, bool tilde) {
  /**
   * stack layout
   *
   * ===========> stack grow
   * +------+-------+--------+     +--------+----------+
   * | argv | redir | param1 | ~~~ | paramN | paramN+1 |
   * +------+-------+--------+     +--------+----------+
   *
   * after evaluation
   * +------+-------+
   * | argv | redir |
   * +------+-------+
   */
  GlobIter begin(state.stack.peekByOffset(size));
  GlobIter end(state.stack.peek());

  // check if glob path fragments have null character
  for (unsigned int i = 0; i < size; i++) {
    auto &v = state.stack.peekByOffset(size - i);
    if (v.hasStrRef()) {
      auto ref = v.asStrRef();
      if (ref.hasNullChar()) {
        raiseGlobbingError(state, state.stack, size, "glob pattern has null characters");
        return false;
      }
    }
  }

  auto &argv = state.stack.peekByOffset(size + 2);
  const unsigned int oldSize = typeAs<ArrayObject>(argv).size();
  auto appender = [&](std::string &&path) {
    typeAs<ArrayObject>(argv).append(DSValue::createStr(std::move(path)));
    return true; // FIXME: check array size limit
  };
  GlobMatchOption option{};
  if (tilde) {
    setFlag(option, GlobMatchOption::TILDE);
  }
  if (hasFlag(state.runtimeOption, RuntimeOption::DOTGLOB)) {
    setFlag(option, GlobMatchOption::DOTGLOB);
  }
  if (hasFlag(state.runtimeOption, RuntimeOption::FASTGLOB)) {
    setFlag(option, GlobMatchOption::FASTGLOB);
  }
  auto matcher = createGlobMatcher<DSValueGlobMeta>(nullptr, begin, end, option);
  auto ret = matcher(appender);
  if (ret == GlobMatchResult::MATCH || hasFlag(state.runtimeOption, RuntimeOption::NULLGLOB)) {
    typeAs<ArrayObject>(argv).sortAsStrArray(oldSize);
    for (unsigned int i = 0; i <= size; i++) {
      state.stack.popNoReturn();
    }
    return true;
  } else {
    const char *msg = ret == GlobMatchResult::NOMATCH ? "No matches for glob pattern"
                                                      : "number of glob results reaches limit";
    raiseGlobbingError(state, state.stack, size, msg);
    return false;
  }
}

static NativeCode initSignalTrampoline() noexcept {
  NativeCode::ArrayType code;
  code[0] = static_cast<char>(OpCode::LOAD_LOCAL);
  code[1] = 1;
  code[2] = static_cast<char>(OpCode::LOAD_LOCAL);
  code[3] = 2;
  code[4] = static_cast<char>(OpCode::CALL_FUNC);
  code[5] = 1;
  code[6] = static_cast<char>(OpCode::EXIT_SIG);
  code[7] = static_cast<char>(OpCode::RETURN);
  return NativeCode(code);
}

static auto signalTrampoline = initSignalTrampoline();

bool VM::kickSignalHandler(DSState &state, int sigNum, DSValue &&func) {
  state.stack.reserve(3);
  state.stack.push(state.getGlobal(BuiltinVarOffset::EXIT_STATUS));
  state.stack.push(std::move(func));
  state.stack.push(DSValue::createSig(sigNum));

  return windStackFrame(state, 3, 3, signalTrampoline);
}

bool VM::checkVMEvent(DSState &state) {
  if (hasFlag(DSState::eventDesc, VMEvent::SIGNAL) && !hasFlag(DSState::eventDesc, VMEvent::MASK)) {
    SignalGuard guard;
    int sigNum = DSState::popPendingSignal();
    if (sigNum == SIGCHLD) {
      state.jobTable.waitForAny();
    } else if (DSValue handler; (handler = state.sigVector.lookup(sigNum)) != nullptr) {
      setFlag(DSState::eventDesc, VMEvent::MASK);
      if (!kickSignalHandler(state, sigNum, std::move(handler))) {
        return false;
      }
    }
  }

  if (state.hook != nullptr) {
    assert(hasFlag(DSState::eventDesc, VMEvent::HOOK));
    auto op = static_cast<OpCode>(GET_CODE(state)[state.stack.pc()]);
    state.hook->vmFetchHook(state, op);
  }
  return true;
}

#define vmdispatch(V) switch (V)

#if 0
#define vmcase(code)                                                                               \
  case OpCode::code: {                                                                             \
    fprintf(stderr, "pc: %u, code: %s\n", state.pc(), #code);                                      \
  }
#else
#define vmcase(code) case OpCode::code:
#endif

#define vmnext continue
#define vmerror goto EXCEPT

#define TRY(E)                                                                                     \
  do {                                                                                             \
    if (!(E)) {                                                                                    \
      vmerror;                                                                                     \
    }                                                                                              \
  } while (false)

bool VM::mainLoop(DSState &state) {
  OpCode op;
  while (true) {
    if (!empty(DSState::eventDesc)) {
      TRY(checkVMEvent(state));
    }

    // fetch next opcode
    op = static_cast<OpCode>(GET_CODE(state)[state.stack.pc()++]);

    // dispatch instruction
    vmdispatch(op) {
      vmcase(HALT) { return true; }
      vmcase(ASSERT) {
        TRY(checkAssertion(state));
        vmnext;
      }
      vmcase(PRINT) {
        unsigned int v = read24(GET_CODE(state), state.stack.pc());
        state.stack.pc() += 3;

        auto &stackTopType = state.typePool.get(v);
        assert(!stackTopType.isVoidType());
        auto ref = state.stack.peek().asStrRef();
        std::string value = ": ";
        value += stackTopType.getNameRef();
        value += " = ";
        value += ref;
        value += "\n";
        fwrite(value.c_str(), sizeof(char), value.size(), stdout);
        fflush(stdout);
        state.stack.popNoReturn();
        vmnext;
      }
      vmcase(INSTANCE_OF) {
        unsigned int v = read24(GET_CODE(state), state.stack.pc());
        state.stack.pc() += 3;

        auto &targetType = state.typePool.get(v);
        auto value = state.stack.pop();
        bool ret = instanceOf(state.typePool, value, targetType);
        state.stack.push(DSValue::createBool(ret));
        vmnext;
      }
      vmcase(CHECK_CAST) {
        unsigned int v = read24(GET_CODE(state), state.stack.pc());
        state.stack.pc() += 3;
        TRY(checkCast(state, state.typePool.get(v)));
        vmnext;
      }
      vmcase(PUSH_NULL) {
        state.stack.push(nullptr);
        vmnext;
      }
      vmcase(PUSH_TRUE) {
        state.stack.push(DSValue::createBool(true));
        vmnext;
      }
      vmcase(PUSH_FALSE) {
        state.stack.push(DSValue::createBool(false));
        vmnext;
      }
      vmcase(PUSH_SIG) {
        unsigned int value = read8(GET_CODE(state), state.stack.pc());
        state.stack.pc()++;
        state.stack.push(DSValue::createSig(value));
        vmnext;
      }
      vmcase(PUSH_INT) {
        unsigned int value = read8(GET_CODE(state), state.stack.pc());
        state.stack.pc()++;
        state.stack.push(DSValue::createInt(value));
        vmnext;
      }
      vmcase(PUSH_STR0) {
        state.stack.push(DSValue::createStr());
        vmnext;
      }
      vmcase(PUSH_STR1) vmcase(PUSH_STR2) vmcase(PUSH_STR3) {
        char data[3];
        unsigned int size = op == OpCode::PUSH_STR1 ? 1 : op == OpCode::PUSH_STR2 ? 2 : 3;
        for (unsigned int i = 0; i < size; i++) {
          data[i] = read8(GET_CODE(state), state.stack.pc());
          state.stack.pc()++;
        }
        state.stack.push(DSValue::createStr(StringRef(data, size)));
        vmnext;
      }
      vmcase(PUSH_META) {
        unsigned int value = read8(GET_CODE(state), state.stack.pc());
        state.stack.pc()++;
        state.stack.push(DSValue::createGlobMeta(static_cast<GlobMeta>(value)));
        vmnext;
      }
      vmcase(PUSH_TYPE) {
        unsigned int v = read24(GET_CODE(state), state.stack.pc());
        state.stack.pc() += 3;
        auto &type = state.typePool.get(v);
        state.stack.push(DSValue::createType(type));
        vmnext;
      }
      vmcase(PUSH_INVALID) {
        state.stack.push(DSValue::createInvalid());
        vmnext;
      }
      vmcase(LOAD_CONST) {
        unsigned char index = read8(GET_CODE(state), state.stack.pc());
        state.stack.pc()++;
        state.stack.push(CONST_POOL(state)[index]);
        vmnext;
      }
      vmcase(LOAD_CONST_W) {
        unsigned short index = read16(GET_CODE(state), state.stack.pc());
        state.stack.pc() += 2;
        state.stack.push(CONST_POOL(state)[index]);
        vmnext;
      }
      vmcase(LOAD_CONST_T) {
        unsigned int index = read24(GET_CODE(state), state.stack.pc());
        state.stack.pc() += 3;
        state.stack.push(CONST_POOL(state)[index]);
        vmnext;
      }
      vmcase(LOAD_GLOBAL) {
        unsigned short index = read16(GET_CODE(state), state.stack.pc());
        state.stack.pc() += 2;
        auto v = state.getGlobal(index);
        if (!v) { // normally unreachable
          raiseError(state, TYPE::IllegalAccessError,
                     "attemp to access uninitialized global variable");
          vmerror;
        }
        state.stack.push(std::move(v));
        vmnext;
      }
      vmcase(STORE_GLOBAL) {
        unsigned short index = read16(GET_CODE(state), state.stack.pc());
        state.stack.pc() += 2;
        state.setGlobal(index, state.stack.pop());
        vmnext;
      }
      vmcase(LOAD_LOCAL) {
        unsigned char index = read8(GET_CODE(state), state.stack.pc());
        state.stack.pc()++;
        state.stack.loadLocal(index);
        vmnext;
      }
      vmcase(STORE_LOCAL) {
        unsigned char index = read8(GET_CODE(state), state.stack.pc());
        state.stack.pc()++;
        state.stack.storeLocal(index);
        vmnext;
      }
      vmcase(LOAD_FIELD) {
        unsigned short index = read16(GET_CODE(state), state.stack.pc());
        state.stack.pc() += 2;
        state.stack.loadField(index);
        vmnext;
      }
      vmcase(STORE_FIELD) {
        unsigned short index = read16(GET_CODE(state), state.stack.pc());
        state.stack.pc() += 2;
        state.stack.storeField(index);
        vmnext;
      }
      vmcase(IMPORT_ENV) {
        unsigned char b = read8(GET_CODE(state), state.stack.pc());
        state.stack.pc()++;
        TRY(loadEnv(state, b > 0));
        vmnext;
      }
      vmcase(LOAD_ENV) {
        const char *value = loadEnv(state, false);
        TRY(value);
        state.stack.push(DSValue::createStr(value));
        vmnext;
      }
      vmcase(STORE_ENV) {
        TRY(storeEnv(state));
        vmnext;
      }
      vmcase(NEW_ENV_CTX) {
        state.stack.push(DSValue::create<EnvCtxObject>(state));
        vmnext;
      }
      vmcase(ADD2ENV_CTX) {
        auto value = state.stack.pop();
        auto name = state.stack.pop();
        typeAs<EnvCtxObject>(state.stack.peek()).setAndSaveEnv(std::move(name), std::move(value));
        vmnext;
      }
      vmcase(POP) {
        state.stack.popNoReturn();
        vmnext;
      }
      vmcase(DUP) {
        state.stack.dup();
        vmnext;
      }
      vmcase(DUP2) {
        state.stack.dup2();
        vmnext;
      }
      vmcase(SWAP) {
        state.stack.swap();
        vmnext;
      }
      vmcase(CONCAT) vmcase(APPEND) {
        const bool selfConcat = op == OpCode::APPEND;
        auto right = state.stack.pop();
        auto left = state.stack.pop();
        TRY(concatAsStr(state, left, right, selfConcat));
        state.stack.push(std::move(left));
        vmnext;
      }
      vmcase(APPEND_ARRAY) {
        DSValue v = state.stack.pop();
        typeAs<ArrayObject>(state.stack.peek()).append(std::move(v));
        vmnext;
      }
      vmcase(APPEND_MAP) {
        DSValue value = state.stack.pop();
        DSValue key = state.stack.pop();
        typeAs<MapObject>(state.stack.peek()).set(std::move(key), std::move(value));
        vmnext;
      }
      vmcase(NEW) {
        unsigned int v = read24(GET_CODE(state), state.stack.pc());
        state.stack.pc() += 3;
        auto &type = state.typePool.get(v);
        pushNewObject(state, type);
        vmnext;
      }
      vmcase(CALL_METHOD) {
        unsigned short paramSize = read8(GET_CODE(state), state.stack.pc());
        state.stack.pc()++;
        unsigned short index = read16(GET_CODE(state), state.stack.pc());
        state.stack.pc() += 2;
        TRY(prepareMethodCall(state, index, paramSize));
        vmnext;
      }
      vmcase(CALL_FUNC) {
        unsigned int paramSize = read8(GET_CODE(state), state.stack.pc());
        state.stack.pc()++;
        TRY(prepareFuncCall(state, paramSize));
        vmnext;
      }
      vmcase(CALL_BUILTIN) {
        unsigned int index = read8(GET_CODE(state), state.stack.pc());
        state.stack.pc()++;
        DSValue returnValue = nativeFuncInfoTable()[index].func_ptr(state);
        TRY(!state.hasError());
        if (returnValue) {
          state.stack.push(std::move(returnValue));
        }
        vmnext;
      }
      vmcase(CALL_BUILTIN2) {
        unsigned int paramSize = read8(GET_CODE(state), state.stack.pc());
        state.stack.pc()++;
        unsigned int index = read8(GET_CODE(state), state.stack.pc());
        state.stack.pc()++;
        auto old = state.stack.nativeWind(paramSize);
        auto ret = nativeFuncInfoTable()[index].func_ptr(state);
        state.stack.nativeUnwind(old);
        TRY(!state.hasError());
        if (ret) {
          state.stack.push(std::move(ret));
        }
        vmnext;
      }
      vmcase(RETURN) {
        state.stack.unwind();
        if (state.stack.checkVMReturn()) {
          return true;
        }
        vmnext;
      }
      vmcase(RETURN_V) {
        DSValue v = state.stack.pop();
        state.stack.unwind();
        state.stack.push(std::move(v));
        if (state.stack.checkVMReturn()) {
          return true;
        }
        vmnext;
      }
      vmcase(RETURN_UDC) {
        auto v = state.stack.pop();
        state.stack.unwind();
        pushExitStatus(state, v.asInt());
        if (state.stack.checkVMReturn()) {
          return true;
        }
        vmnext;
      }
      vmcase(EXIT_SIG) {
        auto v = state.stack.getLocal(0); // old exit status
        unsetFlag(DSState::eventDesc, VMEvent::MASK);
        state.setGlobal(BuiltinVarOffset::EXIT_STATUS, std::move(v));
        vmnext;
      }
      vmcase(BRANCH) {
        unsigned short offset = read16(GET_CODE(state), state.stack.pc());
        if (state.stack.pop().asBool()) {
          state.stack.pc() += 2;
        } else {
          state.stack.pc() += offset - 1;
        }
        vmnext;
      }
      vmcase(GOTO) {
        unsigned int index = read32(GET_CODE(state), state.stack.pc());
        state.stack.pc() = index;
        vmnext;
      }
      vmcase(THROW) {
        auto obj = state.stack.pop();
        state.throwObject(std::move(obj), 1);
        vmerror;
      }
      vmcase(ENTER_FINALLY) {
        const unsigned int offset = read16(GET_CODE(state), state.stack.pc());
        const unsigned int savedIndex = state.stack.pc() + 2;
        state.stack.push(DSValue::createNum(savedIndex));
        state.stack.pc() += offset - 1;
        vmnext;
      }
      vmcase(EXIT_FINALLY) {
        if (state.stack.restoreThrownObject()) {
          vmerror;
        } else {
          assert(state.stack.peek().kind() == DSValueKind::NUMBER);
          unsigned int index = state.stack.pop().asNum();
          state.stack.pc() = index;
          vmnext;
        }
      }
      vmcase(LOOKUP_HASH) {
        auto key = state.stack.pop();
        auto map = state.stack.pop();
        if (!key.isInvalid()) {
          auto &valueMap = typeAs<MapObject>(map).getValueMap();
          auto iter = valueMap.find(key);
          if (iter != valueMap.end()) {
            assert(iter->second.kind() == DSValueKind::NUMBER);
            unsigned int index = iter->second.asNum();
            state.stack.pc() = index;
          }
        }
        vmnext;
      }
      vmcase(REF_EQ) {
        auto v1 = state.stack.pop();
        auto v2 = state.stack.pop();
        state.stack.push(DSValue::createBool(v1 == v2));
        vmnext;
      }
      vmcase(REF_NE) {
        auto v1 = state.stack.pop();
        auto v2 = state.stack.pop();
        state.stack.push(DSValue::createBool(v1 != v2));
        vmnext;
      }
      vmcase(FORK) {
        TRY(forkAndEval(state));
        vmnext;
      }
      vmcase(PIPELINE) vmcase(PIPELINE_LP) vmcase(PIPELINE_ASYNC) {
        bool lastPipe = op == OpCode::PIPELINE_LP;
        auto kind = ForkKind::NONE;
        if (op == OpCode::PIPELINE_ASYNC) {
          unsigned char v = read8(GET_CODE(state), state.stack.pc());
          state.stack.pc()++;
          kind = static_cast<ForkKind>(v);
        }
        TRY(callPipeline(state, lastPipe, kind));
        vmnext;
      }
      vmcase(EXPAND_TILDE) {
        std::string str = state.stack.pop().asStrRef().toString();
        expandTilde(str, true);
        state.stack.push(DSValue::createStr(std::move(str)));
        vmnext;
      }
      vmcase(NEW_CMD) {
        auto v = state.stack.pop();
        auto obj = DSValue::create<ArrayObject>(state.typePool.get(TYPE::StringArray));
        auto &argv = typeAs<ArrayObject>(obj);
        argv.append(std::move(v));
        state.stack.push(std::move(obj));
        vmnext;
      }
      vmcase(ADD_CMD_ARG) {
        unsigned char v = read8(GET_CODE(state), state.stack.pc());
        state.stack.pc()++;
        addCmdArg(state, v > 0);
        vmnext;
      }
      vmcase(ADD_GLOBBING) {
        unsigned int size = read8(GET_CODE(state), state.stack.pc());
        state.stack.pc()++;
        unsigned int v = read8(GET_CODE(state), state.stack.pc());
        state.stack.pc()++;
        TRY(addGlobbingPath(state, size, v > 0));
        vmnext;
      }
      vmcase(CALL_CMD) vmcase(CALL_CMD_NOFORK) {
        bool needFork = op != OpCode::CALL_CMD_NOFORK;
        CmdCallAttr attr{};
        if (needFork) {
          setFlag(attr, CmdCallAttr::NEED_FORK);
        }

        auto redir = state.stack.pop();
        auto argv = state.stack.pop();

        TRY(callCommand(state, CmdResolver(), std::move(argv), std::move(redir), attr));
        vmnext;
      }
      vmcase(BUILTIN_CMD) {
        auto v = state.stack.getLocal(UDC_PARAM_ATTR).asNum();
        auto attr = static_cast<CmdCallAttr>(v);
        DSValue redir = state.stack.getLocal(UDC_PARAM_REDIR);
        DSValue argv = state.stack.getLocal(UDC_PARAM_ARGV);
        bool ret = builtinCommand(state, std::move(argv), std::move(redir), attr);
        flushStdFD();
        TRY(ret);
        vmnext;
      }
      vmcase(BUILTIN_EVAL) {
        auto v = state.stack.getLocal(UDC_PARAM_ATTR).asNum();
        auto attr = static_cast<CmdCallAttr>(v);
        DSValue redir = state.stack.getLocal(UDC_PARAM_REDIR);
        DSValue argv = state.stack.getLocal(UDC_PARAM_ARGV);

        typeAs<ArrayObject>(argv).takeFirst();
        auto &array = typeAs<ArrayObject>(argv);
        if (!array.getValues().empty()) {
          TRY(callCommand(state,
                          CmdResolver(CmdResolver::FROM_DEFAULT_WITH_FQN, FilePathCache::NON),
                          std::move(argv), std::move(redir), attr));
        } else {
          pushExitStatus(state, 0);
        }
        vmnext;
      }
      vmcase(BUILTIN_EXEC) {
        DSValue redir = state.stack.getLocal(UDC_PARAM_REDIR);
        DSValue argv = state.stack.getLocal(UDC_PARAM_ARGV);
        builtinExec(state, std::move(argv), std::move(redir));
        vmnext;
      }
      vmcase(NEW_REDIR) {
        state.stack.push(DSValue::create<RedirObject>());
        vmnext;
      }
      vmcase(ADD_REDIR_OP) {
        unsigned char v = read8(GET_CODE(state), state.stack.pc());
        state.stack.pc()++;
        auto value = state.stack.pop();
        typeAs<RedirObject>(state.stack.peek())
            .addRedirOp(static_cast<RedirOP>(v), std::move(value));
        vmnext;
      }
      vmcase(DO_REDIR) {
        TRY(typeAs<RedirObject>(state.stack.peek()).redirect(state));
        vmnext;
      }
      vmcase(RAND) {
        std::random_device rand;
        std::default_random_engine engine(rand());
        std::uniform_int_distribution<int64_t> dist;
        int64_t v = dist(engine);
        state.stack.push(DSValue::createInt(v));
        vmnext;
      }
      vmcase(GET_SECOND) {
        auto now = std::chrono::system_clock::now();
        auto diff = now - state.baseTime;
        auto sec = std::chrono::duration_cast<std::chrono::seconds>(diff);
        int64_t v = state.getGlobal(BuiltinVarOffset::SECONDS).asInt();
        v += sec.count();
        state.stack.push(DSValue::createInt(v));
        vmnext;
      }
      vmcase(SET_SECOND) {
        state.baseTime = std::chrono::system_clock::now();
        auto v = state.stack.pop();
        state.setGlobal(BuiltinVarOffset::SECONDS, std::move(v));
        vmnext;
      }
      vmcase(UNWRAP) {
        if (state.stack.peek().kind() == DSValueKind::INVALID) {
          raiseError(state, TYPE::UnwrappingError, std::string("invalid value"));
          vmerror;
        }
        vmnext;
      }
      vmcase(CHECK_UNWRAP) {
        bool b = state.stack.pop().kind() != DSValueKind::INVALID;
        state.stack.push(DSValue::createBool(b));
        vmnext;
      }
      vmcase(TRY_UNWRAP) {
        unsigned short offset = read16(GET_CODE(state), state.stack.pc());
        if (state.stack.peek().kind() == DSValueKind::INVALID) {
          state.stack.popNoReturn();
          state.stack.pc() += 2;
        } else {
          state.stack.pc() += offset - 1;
        }
        vmnext;
      }
      vmcase(RECLAIM_LOCAL) {
        unsigned char offset = read8(GET_CODE(state), state.stack.pc());
        state.stack.pc()++;
        unsigned char size = read8(GET_CODE(state), state.stack.pc());
        state.stack.pc()++;

        state.stack.reclaimLocals(offset, size);
        vmnext;
      }
    }

  EXCEPT:
    assert(state.hasError());
    if (!handleException(state)) {
      return false;
    }
  }
}

bool VM::handleException(DSState &state) {
  if (state.hook != nullptr) {
    state.hook->vmThrowHook(state);
  }

  for (; !state.stack.checkVMReturn(); state.stack.unwind()) {
    if (!CODE(state)->is(CodeKind::NATIVE)) {
      auto *cc = static_cast<const CompiledCode *>(CODE(state));

      // search exception entry
      const unsigned int occurredPC = state.stack.pc() - 1;
      const DSType &occurredType = state.typePool.get(state.stack.getThrownObject().getTypeID());

      for (unsigned int i = 0; cc->getExceptionEntries()[i].type != nullptr; i++) {
        const ExceptionEntry &entry = cc->getExceptionEntries()[i];
        if (occurredPC >= entry.begin && occurredPC < entry.end &&
            entry.type->isSameOrBaseTypeOf(occurredType)) {
          if (entry.type->is(TYPE::_ProcGuard)) {
            /**
             * when exception entry indicate exception guard of sub-shell,
             * immediately break interpreter
             * (due to prevent signal handler interrupt and to load thrown object to stack)
             */
            return false;
          }
          state.stack.pc() = entry.dest;
          state.stack.clearOperands();
          state.stack.reclaimLocals(entry.localOffset, entry.localSize);
          if (entry.type->is(TYPE::_Root)) { // finally block
            state.stack.saveThrownObject();
          } else { // catch block
            state.stack.loadThrownObject();
          }
          return true;
        }
      }
    } else if (CODE(state) == &signalTrampoline) { // within signal trampoline
      unsetFlag(DSState::eventDesc, VMEvent::MASK);
    }
  }
  return false;
}

EvalRet VM::startEval(DSState &state, EvalOP op, DSError *dsError, DSValue &value) {
  installSignalHandler(state, SIGCHLD, nullptr);

  const unsigned int oldLevel = state.subshellLevel;

  // run main loop
  const auto ret = mainLoop(state);
  /**
   * if return form subshell, subshellLevel is greater than old.
   */
  const bool subshell = oldLevel != state.subshellLevel;
  DSValue thrown;
  if (ret) {
    if (hasFlag(op, EvalOP::HAS_RETURN)) {
      value = state.stack.pop();
    }
  } else {
    if (!subshell && hasFlag(op, EvalOP::PROPAGATE)) {
      return EvalRet::HAS_ERROR;
    }
    thrown = state.stack.takeThrownObject();
  }

  // handle uncaught exception and termination handler
  auto kind = handleUncaughtException(state, thrown, dsError);
  if (subshell || !hasFlag(op, EvalOP::SKIP_TERM) || !ret) {
    callTermHook(state, kind, std::move(thrown));
  }

  if (subshell) {
    terminate(state.getMaskedExitStatus());
  }
  return ret ? EvalRet::SUCCESS : EvalRet::HANDLED_ERROR;
}

bool VM::callToplevel(DSState &state, const CompiledCode &code, DSError *dsError) {
  state.globals.resize(state.rootModScope->getMaxGlobalVarIndex());
  state.stack.reset();

  state.stack.wind(0, 0, code);

  EvalOP op{};
  if (hasFlag(state.compileOption, CompileOption::INTERACTIVE)) {
    setFlag(op, EvalOP::SKIP_TERM);
  }

  DSValue ret;
  auto s = startEval(state, op, dsError, ret);
  assert(s != EvalRet::HAS_ERROR);
  return s == EvalRet::SUCCESS;
}

unsigned int VM::prepareArguments(VMState &state, DSValue &&recv,
                                  std::pair<unsigned int, std::array<DSValue, 3>> &&args) {
  state.clearThrownObject();

  // push arguments
  unsigned int size = args.first;
  state.reserve(size + 1);
  state.push(std::move(recv));
  for (unsigned int i = 0; i < size; i++) {
    state.push(std::move(args.second[i]));
  }
  return size;
}

#define RAISE_STACK_OVERFLOW(state)                                                                \
  do {                                                                                             \
    raiseError(state, TYPE::StackOverflowError, "interpreter recursion depth reaches limit");      \
    return nullptr;                                                                                \
  } while (false)

#define GUARD_RECURSION(state)                                                                     \
  RecursionGuard _guard(state.stack);                                                              \
  do {                                                                                             \
    if (!_guard.checkLimit()) {                                                                    \
      RAISE_STACK_OVERFLOW(state);                                                                 \
    }                                                                                              \
  } while (false)

static NativeCode initCmdTrampoline() noexcept {
  NativeCode::ArrayType code;
  code[0] = static_cast<char>(OpCode::LOAD_LOCAL);
  code[1] = 0;
  code[2] = static_cast<char>(OpCode::PUSH_NULL);
  code[3] = static_cast<char>(OpCode::CALL_CMD);
  code[4] = static_cast<char>(OpCode::RETURN_V);
  return NativeCode(code);
}

DSValue VM::execCommand(DSState &state, std::vector<DSValue> &&argv, bool propagate) {
  GUARD_RECURSION(state);

  static auto cmdTrampoline = initCmdTrampoline();

  DSValue ret;
  auto obj = DSValue::create<ArrayObject>(state.typePool.get(TYPE::StringArray), std::move(argv));
  prepareArguments(state.stack, std::move(obj), {0, {}});
  if (windStackFrame(state, 1, 1, cmdTrampoline)) {
    startEval(state, EvalOP::SKIP_TERM | EvalOP::PROPAGATE | EvalOP::HAS_RETURN, nullptr, ret);
  }

  if (!propagate) {
    DSValue thrown = state.stack.takeThrownObject();
    handleUncaughtException(state, thrown, nullptr);
  }
  return ret;
}

DSValue VM::callMethod(DSState &state, const MethodHandle &handle, DSValue &&recv,
                       std::pair<unsigned int, std::array<DSValue, 3>> &&args) {
  assert(handle.getParamSize() == args.first);

  GUARD_RECURSION(state);

  unsigned int size = prepareArguments(state.stack, std::move(recv), std::move(args));

  DSValue ret;
  NativeCode code;
  if (handle.isNative()) {
    code = NativeCode(handle.getMethodIndex(), !handle.getReturnType().isVoidType());
  }

  if (handle.isNative() ? windStackFrame(state, size + 1, size + 1, code)
                        : prepareMethodCall(state, handle.getMethodIndex(), size)) {
    EvalOP op = EvalOP::PROPAGATE | EvalOP::SKIP_TERM;
    if (!handle.getReturnType().isVoidType()) {
      setFlag(op, EvalOP::HAS_RETURN);
    }
    startEval(state, op, nullptr, ret);
  }
  return ret;
}

DSValue VM::callFunction(DSState &state, DSValue &&funcObj,
                         std::pair<unsigned int, std::array<DSValue, 3>> &&args) {
  GUARD_RECURSION(state);

  auto &type = state.typePool.get(funcObj.getTypeID());
  unsigned int size = prepareArguments(state.stack, std::move(funcObj), std::move(args));

  DSValue ret;
  if (prepareFuncCall(state, size)) {
    assert(type.isFuncType());
    EvalOP op = EvalOP::PROPAGATE | EvalOP::SKIP_TERM;
    if (!static_cast<const FunctionType &>(type).getReturnType().isVoidType()) {
      setFlag(op, EvalOP::HAS_RETURN);
    }
    startEval(state, op, nullptr, ret);
  }
  return ret;
}

static int parseExitStatus(const ErrorObject &obj) {
  auto ref = obj.getMessage().asStrRef();
  auto r = ref.lastIndexOf(" ");
  ref.removePrefix(r + 1);
  auto pair = convertToNum<int32_t>(ref.begin(), ref.end());
  assert(pair.second);
  return pair.first;
}

DSErrorKind VM::handleUncaughtException(DSState &state, const DSValue &except, DSError *dsError) {
  if (!except) {
    return DS_ERROR_KIND_SUCCESS;
  }

  auto &errorType = state.typePool.get(except.getTypeID());
  DSErrorKind kind = DS_ERROR_KIND_RUNTIME_ERROR;
  state.setExitStatus(1);
  if (errorType.is(TYPE::_ShellExit)) {
    kind = DS_ERROR_KIND_EXIT;
    state.setExitStatus(parseExitStatus(typeAs<ErrorObject>(except)));
  } else if (errorType.is(TYPE::_AssertFail)) {
    kind = DS_ERROR_KIND_ASSERTION_ERROR;
  }

  // get error line number
  unsigned int errorLineNum = 0;
  std::string sourceName;
  if (state.typePool.get(TYPE::Error).isSameOrBaseTypeOf(errorType) ||
      kind != DS_ERROR_KIND_RUNTIME_ERROR) {
    auto &obj = typeAs<ErrorObject>(except);
    errorLineNum = getOccurredLineNum(obj.getStackTrace());
    const char *ptr = getOccurredSourceName(obj.getStackTrace());
    sourceName = ptr;
  }

  // print error message
  auto oldStatus = state.getGlobal(BuiltinVarOffset::EXIT_STATUS);
  if (kind == DS_ERROR_KIND_RUNTIME_ERROR) {
    fputs("[runtime error]\n", stderr);
    const bool bt = state.typePool.get(TYPE::Error).isSameOrBaseTypeOf(errorType);
    auto *handle = state.typePool.lookupMethod(errorType, bt ? "backtrace" : OP_STR);
    assert(handle);

    DSValue ret = VM::callMethod(state, *handle, DSValue(except), makeArgs());
    if (state.hasError()) {
      state.stack.clearThrownObject();
      fputs("cannot obtain string representation\n", stderr);
    } else if (!bt) {
      auto ref = ret.asStrRef();
      fwrite(ref.data(), sizeof(char), ref.size(), stderr);
      fputc('\n', stderr);
    }
  } else if (kind == DS_ERROR_KIND_ASSERTION_ERROR ||
             hasFlag(state.runtimeOption, RuntimeOption::TRACE_EXIT)) {
    typeAs<ErrorObject>(except).printStackTrace(state);
  }
  fflush(stderr);
  state.setGlobal(BuiltinVarOffset::EXIT_STATUS, std::move(oldStatus));

  if (dsError != nullptr) {
    *dsError = {.kind = kind,
                .fileName = sourceName.empty() ? nullptr : strdup(sourceName.c_str()),
                .lineNum = errorLineNum,
                .chars = 0,
                .name = strdup(kind == DS_ERROR_KIND_RUNTIME_ERROR ? errorType.getName() : "")};
  }
  return kind;
}

void VM::callTermHook(DSState &state, DSErrorKind kind, DSValue &&except) {
  auto funcObj = getBuiltinGlobal(state, VAR_TERM_HOOK);
  if (funcObj.kind() == DSValueKind::INVALID) {
    return;
  }

  int termKind = TERM_ON_EXIT;
  if (kind == DS_ERROR_KIND_RUNTIME_ERROR) {
    termKind = TERM_ON_ERR;
  } else if (kind == DS_ERROR_KIND_ASSERTION_ERROR) {
    termKind = TERM_ON_ASSERT;
  }

  auto oldExitStatus = state.getGlobal(BuiltinVarOffset::EXIT_STATUS);
  auto args = makeArgs(DSValue::createInt(termKind),
                       termKind == TERM_ON_ERR ? std::move(except) : oldExitStatus);

  setFlag(DSState::eventDesc, VMEvent::MASK);
  VM::callFunction(state, std::move(funcObj), std::move(args)); // ignore exception
  state.stack.clearThrownObject();

  // restore old value
  state.setGlobal(BuiltinVarOffset::EXIT_STATUS, std::move(oldExitStatus));
  unsetFlag(DSState::eventDesc, VMEvent::MASK);
}

} // namespace ydsh