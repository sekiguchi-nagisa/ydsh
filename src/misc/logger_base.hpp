/*
 * Copyright (C) 2018 Nagisa Sekiguchi
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

#ifndef YDSH_LOGGER_BASE_HPP
#define YDSH_LOGGER_BASE_HPP

#include <unistd.h>

#include <cstring>
#include <string>
#include <ctime>
#include <cstdarg>
#include <mutex>

#include "resource.hpp"
#include "util.hpp"
#include "fatal.h"

namespace ydsh {

enum class LogLevel : unsigned int {
    INFO, WARNING, ERROR, FATAL, NONE
};

inline const char *toString(LogLevel level) {
    const char *str[] = {
            "info", "warning", "error", "fatal", "none"
    };
    return str[static_cast<unsigned int>(level)];
}

namespace __detail_logger {

template<bool T>
class LoggerBase {
protected:
    static_assert(T, "not allowed instantiation");

    std::string prefix;
    FILE *fp{nullptr};
    LogLevel severity{LogLevel::FATAL};
    std::mutex outMutex;

    /**
     * if prefix is mepty string, treat as null logger
     * @param prefix
     */
    explicit LoggerBase(const char *prefix) : prefix(prefix) {
        this->syncSetting();
    }

    ~LoggerBase() {
        if(this->fp && this->fp != stderr) {
            fclose(this->fp);
        }
    }

    void log(LogLevel level, const char *fmt, va_list list);

public:
    void syncSetting();

    void operator()(LogLevel level, const char *fmt, ...) __attribute__ ((format(printf, 3, 4))) {
        va_list arg;
        va_start(arg, fmt);
        this->log(level, fmt, arg);
        va_end(arg);
    }

    bool enabled(LogLevel level) const {
        return static_cast<unsigned int>(level) < static_cast<unsigned int>(LogLevel::NONE) &&
                static_cast<unsigned int>(level) >= static_cast<unsigned int>(this->severity);
    }
};

template <bool T>
void LoggerBase<T>::log(LogLevel level, const char *fmt, va_list list) {
    if(!this->enabled(level)) {
        return;
    }

    // create body
    char *str = nullptr;
    if(vasprintf(&str, fmt, list) == -1) {
        fatal_perror("");
    }

    // create header
    char header[128];
    header[0] = '\0';

    time_t timer = time(nullptr);
    struct tm local{};
    tzset();
    if(localtime_r(&timer, &local)) {
        char buf[32];
        strftime(buf, arraySize(buf), "%F %T", &local);
        snprintf(header, arraySize(header), "%s [%d] [%s] ", buf, getpid(), toString(level));
    }

    // print body
    fprintf(this->fp, "%s%s\n", header, str);
    fflush(this->fp);
    free(str);

    if(level == LogLevel::FATAL) {
        abort();
    }
}

template <bool T>
void LoggerBase<T>::syncSetting() {
    std::lock_guard<std::mutex> guard(this->outMutex);

    if(this->prefix.empty()) {
        this->severity = LogLevel::NONE;
        return;
    }

    std::string key = this->prefix;
    key += "_LEVEL";
    const char *level = getenv(key.c_str());
    if(level != nullptr) {
        for(auto i = static_cast<unsigned int>(LogLevel::INFO);
            i < static_cast<unsigned int>(LogLevel::NONE) + 1; i++) {
            auto s = static_cast<LogLevel>(i);
            if(strcasecmp(toString(s), level) == 0) {
                this->severity = s;
                break;
            }
        }
    }

    key = this->prefix;
    key += "_APPENDER";
    const char *appender = getenv(key.c_str());
    if(appender != nullptr && strlen(appender) != 0) {
        if(this->fp && this->fp != stderr) {
            fclose(this->fp);
            this->fp = nullptr;
        }
        this->fp = fopen(appender, "w");
    }
    if(!this->fp) {
        this->fp = stderr;
    }
}

} // namespace __detail_logger

using LoggerBase = __detail_logger::LoggerBase<true>;

struct NullLogger : public LoggerBase {
    NullLogger() : LoggerBase("") {}
};

template <typename T>
class SingletonLogger : public LoggerBase, public Singleton<T> {
protected:
    explicit SingletonLogger(const char *prefix) : LoggerBase(prefix) {}

public:
    static void Info(const char *fmt, ...) __attribute__ ((format(printf, 1, 2))) {
        va_list arg;
        va_start(arg, fmt);
        Singleton<T>::instance().log(LogLevel::INFO, fmt, arg);
        va_end(arg);
    }

    static void Warning(const char *fmt, ...) __attribute__ ((format(printf, 1, 2))) {
        va_list arg;
        va_start(arg, fmt);
        Singleton<T>::instance().log(LogLevel::WARNING, fmt, arg);
        va_end(arg);
    }

    static void Error(const char *fmt, ...) __attribute__ ((format(printf, 1, 2))) {
        va_list arg;
        va_start(arg, fmt);
        Singleton<T>::instance().log(LogLevel::ERROR, fmt, arg);
        va_end(arg);
    }

    static void Fatal(const char *fmt, ...) __attribute__ ((format(printf, 1, 2))) {
        va_list arg;
        va_start(arg, fmt);
        Singleton<T>::instance().log(LogLevel::FATAL, fmt, arg);
        va_end(arg);
    }

    static bool Enabled(LogLevel level) {
        return Singleton<T>::instance().enabled(level);
    }
};

} // namespace ydsh


#endif //YDSH_LOGGER_BASE_HPP