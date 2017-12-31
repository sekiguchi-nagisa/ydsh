/*
 * Copyright (C) 2017 Nagisa Sekiguchi
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

#ifndef YDSH_JOB_H
#define YDSH_JOB_H

#include <unistd.h>

#include <vector>
#include <type_traits>

#include "misc/resource.hpp"

struct DSState;

namespace ydsh {

void tryToForeground(const DSState &st);

class JobTable;

struct JobTrait;

class Proc {
public:
    enum State : unsigned char {
        RUNNING,
        STOPPED,    // stopped by SIGSTOP or SIGTSTP
        TERMINATED, // already called waitpid
    };

private:
    pid_t pid_;
    State state_;

    /**
     * enabled when `state' is TERMINATED.
     */
    unsigned char exitStatus_;

public:
    Proc() = default;

    explicit Proc(pid_t pid) : pid_(pid), state_(RUNNING), exitStatus_(0) {}

    pid_t pid() const {
        return this->pid_;
    }

    State state() const {
        return this->state_;
    }

    unsigned char exitStatus() const {
        return this->exitStatus_;
    }

    int wait();

    void send(int sigNum);
};

/**
 * after fork, reset signal setting in child process.
 */
Proc xfork(DSState &st, pid_t pgid, bool foreground);

class JobImpl {
private:
    static_assert(std::is_pod<Proc>::value, "failed");

    unsigned long refCount{0};

    /**
     * after detach, will be 0
     */
    unsigned int jobId{0};

    /**
     * pid of owner process (JobEntry creator)
     */
    const pid_t ownerPid;

    unsigned short procSize;

    /**
     * if all process are terminated. will be TERMINATED
     */
    Proc::State state{Proc::RUNNING};

    /**
     * if already closed, will be -1.
     */
    int oldStdin{-1};

    /**
     * initial size is procSize + 1 (due to append process)
     */
    Proc procs[];

    friend class JobTable;

    friend struct JobTrait;

    JobImpl(unsigned int size, const Proc *procs, bool saveStdin) : ownerPid(getpid()), procSize(size) {
        for(unsigned int i = 0; i < this->procSize; i++) {
            this->procs[i] = procs[i];
        }
        if(saveStdin) {
            this->oldStdin = dup(STDIN_FILENO);
        }
    }

    ~JobImpl() = default;

public:
    NON_COPYABLE(JobImpl);

    unsigned short getProcSize() const {
        return this->procSize;
    }

    bool available() const {
        return this->state != Proc::TERMINATED;
    }

    /**
     *
     * @param index
     * @return
     * after termiantion, return -1.
     */
    pid_t getPid(unsigned int index) const {
        return this->procs[index].pid();
    }

    /**
     *
     * @return
     * after detached, return 0.
     */
    unsigned int getJobId() const {
        return this->jobId;
    }

    pid_t getOwnerPid() const {
        return this->ownerPid;
    }

    bool hasOwnership() const {
        return this->ownerPid == getpid();
    }

    /**
     * call only once
     * @param proc
     */
    void append(Proc proc) {
        this->procs[this->procSize++] = proc;
    }

    /**
     * restore STDIN_FD
     * if has no ownership, do nothing.
     * @return
     * if restore fd, return true.
     * if already called, return false
     */
    bool restoreStdin();

    /**
     * send signal to all processes.
     * @param sigNum
     */
    void send(int sigNum) {
        for(unsigned int i = 0; i < this->procSize; i++) {
            this->procs[i].send(sigNum);
        }
    }

    /**
     * wait for termination.
     * after termination, `state' will be TERMINATED.
     * @return
     * exit status of last process.
     * if cannot terminate (has no-wnership or stopped), return -1.
     */
    int wait();
};

struct JobTrait {
    static unsigned long useCount(const JobImpl *ptr) noexcept {
        return ptr->refCount;
    }

    static void increase(JobImpl *ptr) noexcept {
        if(ptr != nullptr) {
            ptr->refCount++;
        }
    }

    static void decrease(JobImpl *ptr) noexcept {
        if(ptr != nullptr && --ptr->refCount == 0) {
            free(ptr);
        }
    }
};

using Job = IntrusivePtr<JobImpl, JobTrait>;

class JobTable {    //FIXME: send signal to managed jobs
private:
    std::vector<Job> entries;

    /**
     * latest attached entry.
     */
    Job latestEntry;

public:
    JobTable() = default;
    ~JobTable() = default;

    static Job newEntry(unsigned int size, const Proc *procs, bool saveStdin = true);

    static Job newEntry(Proc proc) {
        Proc procs[1] = {proc};
        return newEntry(1, procs, false);
    }

    void attach(Job job);

    /**
     * remove job from JobTable
     * @param jobId
     * if 0, do nothing.
     * @return
     * detached job.
     * if specified job is not found, return null
     */
    Job detach(unsigned int jobId);

    /**
     * if has ownership, wait termination.
     * @param entry
     * @return
     * exit status of last process.
     * after waiting termination, remove entry.
     */
    int waitAndDetach(Job &entry) {
        int ret = entry->wait();
        if(!entry->available()) {
            this->detach(entry->getJobId());
        }
        return ret;
    }

    void detachAll() {
        for(auto &e : this->entries) {
            e->jobId = 0;
        }
        this->entries.clear();
        this->latestEntry.reset();
    }

    Job &getLatestEntry() {
        return this->latestEntry;
    }

    std::vector<Job>::iterator begin() {
        return this->entries.begin();
    }

    std::vector<Job>::iterator end() {
        return this->entries.end();
    }

    /**
     *
     * @param jobId
     * @return
     * if not found, return nullptr
     */
    Job findEntry(unsigned int jobId) const;

private:
    /**
     *
     * @return
     * first is entry index.
     * second is job id.
     */
    std::pair<unsigned int, unsigned int> findEmptyEntry() const;   //FIXME: binary search

    std::vector<Job>::const_iterator findEntryIter(unsigned int jobId) const;
};

} // namespace ydsh

#endif //YDSH_JOB_H
