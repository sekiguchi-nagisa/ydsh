/*
 * Copyright (C) 2019 Nagisa Sekiguchi
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

#include <sys/wait.h>
#include <climits>

#include "redir.h"
#include "vm.h"
#include "logger.h"

namespace ydsh {

PipelineState::~PipelineState() {
    /**
     * due to prevent write blocking of child processes, force to restore stdin before call wait.
     * in some situation, raise SIGPIPE in child processes.
     */
    bool restored = this->entry->restoreStdin();
    auto waitOp = state.isRootShell() && state.isJobControl() ? Proc::BLOCK_UNTRACED : Proc::BLOCKING;
    this->entry->wait(waitOp);

    if(restored) {
        int ret = tryToBeForeground(this->state);
        LOG(DUMP_EXEC, "tryToBeForeground: %d, %s", ret, strerror(errno));
    }
    if(this->entry->available()) {
        // job is still running, attach to JobTable
        this->state.jobTable.attach(this->entry);
    }
    this->state.jobTable.updateStatus();
}

RedirConfig::~RedirConfig() {
    this->restoreFDs();
    for(int fd : this->oldFds) {
        close(fd);
    }
}

static int doIOHere(const String_Object &value) {
    pipe_t pipe[1];
    initAllPipe(1, pipe);

    dup2(pipe[0][READ_PIPE], STDIN_FILENO);

    if(value.size() + 1 <= PIPE_BUF) {
        int errnum = 0;
        if(write(pipe[0][WRITE_PIPE], value.getValue(), sizeof(char) * value.size()) < 0) {
            errnum = errno;
        }
        if(errnum == 0 && write(pipe[0][WRITE_PIPE], "\n", 1) < 0) {
            errnum = errno;
        }
        closeAllPipe(1, pipe);
        return errnum;
    } else {
        pid_t pid = fork();
        if(pid < 0) {
            return errno;
        }
        if(pid == 0) {   // child
            pid = fork();   // double-fork (not wait IO-here process termination.)
            if(pid == 0) {  // child
                close(pipe[0][READ_PIPE]);
                dup2(pipe[0][WRITE_PIPE], STDOUT_FILENO);
                if(write(STDOUT_FILENO, value.getValue(), value.size()) < 0) {
                    exit(1);
                }
                if(write(STDOUT_FILENO, "\n", 1) < 0) {
                    exit(1);
                }
            }
            exit(0);
        }
        closeAllPipe(1, pipe);
        waitpid(pid, nullptr, 0);
        return 0;
    }
}

/**
 * if failed, return non-zero value(errno)
 */
static int redirectToFile(const DSValue &fileName, const char *mode, int targetFD) {
    auto &type = *fileName->getType();
    if(type.is(TYPE::String)) {
        FILE *fp = fopen(typeAs<String_Object>(fileName)->getValue(), mode);
        if(fp == nullptr) {
            return errno;
        }
        int fd = fileno(fp);
        if(dup2(fd, targetFD) < 0) {
            int e = errno;
            fclose(fp);
            return e;
        }
        fclose(fp);
    } else {
        assert(type.is(TYPE::UnixFD));
        int fd = typeAs<UnixFD_Object>(fileName)->getValue();
        if(strchr(mode, 'a') != nullptr) {
            if(lseek(fd, 0, SEEK_END) == -1) {
                return errno;
            }
        }
        if(dup2(fd, targetFD) < 0) {
            return errno;
        }
    }
    return 0;
}

static int redirectImpl(const std::pair<RedirOP, DSValue> &pair) {
    switch(pair.first) {
    case RedirOP::IN_2_FILE:
        return redirectToFile(pair.second, "rb", STDIN_FILENO);
    case RedirOP::OUT_2_FILE:
        return redirectToFile(pair.second, "wb", STDOUT_FILENO);
    case RedirOP::OUT_2_FILE_APPEND:
        return redirectToFile(pair.second, "ab", STDOUT_FILENO);
    case RedirOP::ERR_2_FILE:
        return redirectToFile(pair.second, "wb", STDERR_FILENO);
    case RedirOP::ERR_2_FILE_APPEND:
        return redirectToFile(pair.second, "ab", STDERR_FILENO);
    case RedirOP::MERGE_ERR_2_OUT_2_FILE: {
        int r = redirectToFile(pair.second, "wb", STDOUT_FILENO);
        if(r != 0) {
            return r;
        }
        if(dup2(STDOUT_FILENO, STDERR_FILENO) < 0) { return errno; }
        return 0;
    }
    case RedirOP::MERGE_ERR_2_OUT_2_FILE_APPEND: {
        int r = redirectToFile(pair.second, "ab", STDOUT_FILENO);
        if(r != 0) {
            return r;
        }
        if(dup2(STDOUT_FILENO, STDERR_FILENO) < 0) { return errno; }
        return 0;
    }
    case RedirOP::MERGE_ERR_2_OUT:
        if(dup2(STDOUT_FILENO, STDERR_FILENO) < 0) { return errno; }
        return 0;
    case RedirOP::MERGE_OUT_2_ERR:
        if(dup2(STDERR_FILENO, STDOUT_FILENO) < 0) { return errno; }
        return 0;
    case RedirOP::HERE_STR:
        return doIOHere(*typeAs<String_Object>(pair.second));
    case RedirOP::NOP:
        break;
    }
    return 0;   // do nothing
}

bool RedirConfig::redirect(DSState &st) {
    this->backupFDs();
    for(auto &pair : this->ops) {
        int r = redirectImpl(pair);
        if(this->backupFDset > 0 && r != 0) {
            std::string msg = REDIR_ERROR;
            if(pair.second) {
                auto *type = pair.second->getType();
                if(type->is(TYPE::String)) {
                    if(!typeAs<String_Object>(pair.second)->empty()) {
                        msg += ": ";
                        msg += typeAs<String_Object>(pair.second)->getValue();
                    }
                } else if(type->is(TYPE::UnixFD)) {
                    msg += ": ";
                    msg += std::to_string(typeAs<UnixFD_Object>(pair.second)->getValue());
                }
            }
            raiseSystemError(st, r, std::move(msg));
            return false;
        }
    }
    return true;
}

} // namespace ydsh