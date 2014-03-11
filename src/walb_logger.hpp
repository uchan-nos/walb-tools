#pragma once
/**
 * @file
 * @brief wrapper of cybozu logger.
 * @author HOSHINO Takashi
 *
 * (C) 2013 Cybozu Labs, Inc.
 */
#include "cybozu/log.hpp"
#include "util.hpp"
#include <map>

#ifdef DEBUG
#define LOGd(...) LOGd2(__VA_ARGS__, "")
#define LOGd2(fmt, ...) \
    cybozu::PutLog(cybozu::LogDebug, "DEBUG (%s:%d) " fmt "%s", __func__, __LINE__, __VA_ARGS__)
#else
#define LOGd(...)
#endif

#define LOGi(...) cybozu::PutLog(cybozu::LogInfo, "INFO " __VA_ARGS__)
#define LOGw(...) cybozu::PutLog(cybozu::LogWarning, "WARNING " __VA_ARGS__)
#define LOGe(...) cybozu::PutLog(cybozu::LogError, "ERROR " __VA_ARGS__)

#define LOGd_(...)
#define LOGi_(...)
#define LOGw_(...)
#define LOGe_(...)

namespace walb {

/**
 * Logger interface.
 *
 * You can call one of them to change output target:
 *   cybozu::OpenLogFile() for a file.
 *   cybozu::SetLogFILE() for FILE pointer.
 *   default is syslog.
 *   These are not thread-safe. You must call one of them at once.
 * You can change priority of putting logs with:
 *   cybozu::SetLogPriority().
 */
class Logger
{
public:
    virtual ~Logger() noexcept = default;
    virtual void write(cybozu::LogPriority pri, const char *msg) const noexcept = 0;

    void write(cybozu::LogPriority pri, const std::string &msg) const noexcept {
        write(pri, msg.c_str());
    }
    void writeV(cybozu::LogPriority pri, const char *format, va_list args) const noexcept {
        try {
            std::string msg;
            cybozu::vformat(msg, format, args);
            write(pri, msg);
        } catch (...) {
            write(pri, "Logger::write() error.");
        }
    }

#ifdef __GNUC__
    void writeF(cybozu::LogPriority pri, const char *format, ...) const noexcept __attribute__((format(printf, 3, 4)));
    #define WALB_LOGGER_FORMAT_ATTR __attribute__((format(printf, 2, 3)))
#else
    void writeF(cybozu::LogPriority pri, const char *format, ...) const noexcept;
    #define WALB_LOGGER_FORMAT_ATTR
#endif

    void debug(UNUSED const std::string &msg) const noexcept {
#ifdef DEBUG
        write(cybozu::LogDebug, msg);
#endif
    }
    void info(const std::string &msg) const noexcept { write(cybozu::LogInfo, msg); }
    void warn(const std::string &msg) const noexcept { write(cybozu::LogWarning, msg); }
    void error(const std::string &msg) const noexcept { write(cybozu::LogError, msg); }

    void debug(UNUSED const char *format, ...) const noexcept WALB_LOGGER_FORMAT_ATTR;
    void info(const char *format, ...) const noexcept WALB_LOGGER_FORMAT_ATTR;
    void warn(const char *format, ...) const noexcept WALB_LOGGER_FORMAT_ATTR;
    void error(const char *format, ...) const noexcept WALB_LOGGER_FORMAT_ATTR;

    void writeAndThrow(cybozu::LogPriority pri, const cybozu::Exception &e) const {
        write(pri, e.what());
        throw e;
    }
    void errorThrow(const cybozu::Exception &e) const {
        writeAndThrow(cybozu::LogError, e);
    }
};

inline void Logger::writeF(cybozu::LogPriority pri, const char *format, ...) const noexcept {
    try {
        va_list args;
        va_start(args, format);
        writeV(pri, format, args);
        va_end(args);
    } catch (...) {
        write(pri, "Logger::write() error.");
    }
}
inline void Logger::debug(UNUSED const char *format, ...) const noexcept {
#ifdef DEBUG
    va_list args;
    va_start(args, format);
    writeV(cybozu::LogDebug, format, args);
    va_end(args);
#endif
}
inline void Logger::info(const char *format, ...) const noexcept {
    va_list args;
    va_start(args, format);
    writeV(cybozu::LogInfo, format, args);
    va_end(args);
}
inline void Logger::warn(const char *format, ...) const noexcept {
    va_list args;
    va_start(args, format);
    writeV(cybozu::LogWarning, format, args);
    va_end(args);
}
inline void Logger::error(const char *format, ...) const noexcept {
    va_list args;
    va_start(args, format);
    writeV(cybozu::LogError, format, args);
    va_end(args);
}

namespace logger_local {

inline const char *getPriStr(cybozu::LogPriority pri)
{
    static const std::map<int, std::string> priMap = {
        { cybozu::LogDebug, "DEBUG" },
        { cybozu::LogInfo, "INFO" },
        { cybozu::LogWarning, "WARNING" },
        { cybozu::LogError, "ERROR" },
    };
    std::map<int, std::string>::const_iterator itr = priMap.find(pri);
    assert(itr != priMap.cend());
    return itr->second.c_str();
}

} // namespace logger_local

/**
 * Simple logger.
 */
class SimpleLogger : public Logger
{
public:
    void write(cybozu::LogPriority pri, const char *msg) const noexcept override {
        cybozu::PutLog(pri, "%s %s", logger_local::getPriStr(pri), msg);
    }
};

/**
 * Logger for protocols.
 */
class ProtocolLogger : public Logger
{
private:
    std::string selfId_;
    std::string remoteId_;
public:
    ProtocolLogger(const std::string &selfId, const std::string &remoteId)
        : selfId_(selfId), remoteId_(remoteId) {}
    void write(cybozu::LogPriority pri, const char *msg) const noexcept override {
        cybozu::PutLog(pri, "%s [%s][%s] %s", logger_local::getPriStr(pri)
                       , selfId_.c_str(), remoteId_.c_str(), msg);
    }
};

} //namespace walb
