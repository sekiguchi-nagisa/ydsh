/*
 * Copyright (C) 2015-2016 Nagisa Sekiguchi
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

#ifndef YDSH_LOGGER_H
#define YDSH_LOGGER_H

#include <config.h>
#include "misc/logger_base.hpp"
#include "misc/flag_util.hpp"

#define EACH_LOGGING_POLICY(E) \
    E(TRACE_TOKEN) \
    E(DUMP_EXEC) \
    E(DUMP_CONSOLE) \
    E(DUMP_WAIT) \
    E(TRACE_MODULE)


namespace ydsh {

#ifdef USE_LOGGING
constexpr bool useLogging = true;
#else
constexpr bool useLogging = false;
#endif


class Logger : public ydsh::SingletonLogger<Logger> {
private:
    unsigned int whiteList{0};

public:
    enum Policy : unsigned int {
#define GEN_ENUM(E) E,
        EACH_LOGGING_POLICY(GEN_ENUM)
#undef GEN_ENUM
    };

    Logger() : ydsh::SingletonLogger<Logger>("YDSH") {
        this->syncSetting();
    }

    bool checkPolicy(Policy policy) const {
        return hasFlag(this->whiteList, 1u << static_cast<unsigned int>(policy));
    }

private:
    void syncSetting() {
        LoggerBase::syncSetting();
        const char *policies[] = {
#define GEN_STR(E) "YDSH_" #E,
                EACH_LOGGING_POLICY(GEN_STR)
#undef GEN_STR
        };
        for(unsigned int i = 0; i < arraySize(policies); i++) {
            if(getenv(policies[i])) {
                setFlag(this->whiteList, 1u << i);
            }
        }
        this->severity = LogLevel::INFO;
    }
};

} // namespace ydsh


#define LOG(P, fmt, ...) \
do { using namespace ydsh; \
    if(useLogging && Logger::instance().checkPolicy(Logger::P)) { \
        int old = errno; \
        Logger::Info("%s(%s):%d: " fmt, __FILE_NAME__, __func__, __LINE__, ## __VA_ARGS__); \
        errno = old; \
    } \
} while(false)

#define LOG_IF(P, B) \
do { using namespace ydsh; \
    if(useLogging && Logger::instance().checkPolicy(Logger::P)) { \
        B \
    } \
} while(false)

#define LOG_EXPR(P, FUNC) LOG_IF(P, { LOG(P, "%s", FUNC().c_str()); })


#endif //YDSH_LOGGER_H
